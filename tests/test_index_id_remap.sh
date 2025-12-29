#!/usr/bin/env bash
set -euo pipefail

VERBOSE=${VERBOSE:-0}
log_verbose() {
  if [ "$VERBOSE" = "1" ]; then
    echo -e "\033[0;36m  [SQL] $1\033[0m"
  fi
}

DB_USER=${DB_USER:-root}
DB_PASS=${DB_PASS:-}
SRC_DB=${SRC_DB:-test_index_remap_src}
TGT_DB=${TGT_DB:-test_index_remap_tgt}
TABLE_NAME=${TABLE_NAME:-remap_tbl}

PARSER_DIR=${PARSER_DIR:-/home/cslog/mysql/percona-parser}
IB_PARSER=${IB_PARSER:-$PARSER_DIR/build/ib_parser}
OUT_DIR=${OUT_DIR:-/tmp/ibd-index-remap}

SOURCE_IBD="$OUT_DIR/source.ibd"
SOURCE_SDI="$OUT_DIR/source_sdi.json"
TARGET_IBD="$OUT_DIR/target.ibd"
TARGET_SDI="$OUT_DIR/target_sdi.json"
REBUILT_IBD="$OUT_DIR/rebuilt.ibd"
REBUILT_CFG="$OUT_DIR/rebuilt.cfg"

MYSQL=(mysql -u"$DB_USER")
if [ -n "$DB_PASS" ]; then
  MYSQL+=(-p"$DB_PASS")
fi

DATADIR=$("${MYSQL[@]}" -N -B -e "SELECT @@datadir;")
DATADIR=${DATADIR%/}
SRC_IBD_PATH="$DATADIR/$SRC_DB/${TABLE_NAME}.ibd"
TGT_IBD_PATH="$DATADIR/$TGT_DB/${TABLE_NAME}.ibd"
TGT_IBD_DEST="$DATADIR/$TGT_DB/${TABLE_NAME}.ibd"
TGT_CFG_DEST="$DATADIR/$TGT_DB/${TABLE_NAME}.cfg"

mkdir -p "$OUT_DIR"

wait_for_mysql() {
  for _ in $(seq 1 30); do
    if "${MYSQL[@]}" -e "SELECT 1;" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

cleanup_datadir() {
  "${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $SRC_DB; DROP DATABASE IF EXISTS $TGT_DB;" >/dev/null 2>&1 || true
  if [ -d "$DATADIR/$SRC_DB" ] || [ -d "$DATADIR/$TGT_DB" ]; then
    sudo systemctl stop mysql
    sudo rm -rf "$DATADIR/$SRC_DB" "$DATADIR/$TGT_DB"
    sudo systemctl start mysql
    sleep 3
    if ! wait_for_mysql; then
      echo "MySQL did not come up in time after cleanup."
      exit 1
    fi
  fi
}

extract_index_ids() {
  python3 - "$1" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    doc = json.load(f)

for elem in doc:
    if not isinstance(elem, dict):
        continue
    obj = elem.get("object")
    if not isinstance(obj, dict):
        continue
    if obj.get("dd_object_type") != "Table":
        continue
    dd = obj.get("dd_object", {})
    for idx in dd.get("indexes", []):
        name = idx.get("name")
        se = idx.get("se_private_data", "")
        idx_id = None
        for part in se.split(";"):
            if part.startswith("id="):
                idx_id = part.split("=", 1)[1]
                break
        if name and idx_id:
            print(f"{name}:{idx_id}")
    break
PY
}

echo "==========================================="
echo "Testing index-id remap for IMPORT TABLESPACE"
echo "==========================================="

if [ ! -f "$IB_PARSER" ]; then
  echo "ib_parser not found, building..."
  make -C "$PARSER_DIR/build" -j"$(nproc)"
fi

cleanup_datadir

echo "==> Creating compressed source table"
log_verbose "DROP DATABASE IF EXISTS $SRC_DB"
log_verbose "CREATE DATABASE $SRC_DB"
"${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $SRC_DB; CREATE DATABASE $SRC_DB;"

"${MYSQL[@]}" "$SRC_DB" <<SQL
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY AUTO_INCREMENT,
  a INT,
  b VARCHAR(20),
  c INT,
  KEY idx_ab (a, b),
  KEY idx_c (c)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;

INSERT INTO $TABLE_NAME (a, b, c) VALUES
  (10, 'alpha', 100),
  (20, 'bravo', 200),
  (10, 'charlie', 300),
  (20, 'delta', 400);
SQL

echo "==> Exporting source .ibd and SDI"
log_verbose "FLUSH TABLES $TABLE_NAME FOR EXPORT"
"${MYSQL[@]}" "$SRC_DB" -e "FLUSH TABLES $TABLE_NAME FOR EXPORT;"

if ! sudo test -f "$SRC_IBD_PATH"; then
  echo "Missing source .ibd at $SRC_IBD_PATH"
  "${MYSQL[@]}" "$SRC_DB" -e "UNLOCK TABLES;"
  exit 1
fi

sudo cp "$SRC_IBD_PATH" "$SOURCE_IBD"
sudo chown "$(id -u)":"$(id -g)" "$SOURCE_IBD"
"${MYSQL[@]}" "$SRC_DB" -e "UNLOCK TABLES;"

ibd2sdi "$SOURCE_IBD" > "$SOURCE_SDI"

echo "==> Creating target table (uncompressed)"
log_verbose "DROP DATABASE IF EXISTS $TGT_DB"
log_verbose "CREATE DATABASE $TGT_DB"
"${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $TGT_DB; CREATE DATABASE $TGT_DB;"

"${MYSQL[@]}" "$TGT_DB" <<SQL
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY AUTO_INCREMENT,
  a INT,
  b VARCHAR(20),
  c INT,
  KEY idx_ab (a, b),
  KEY idx_c (c)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
SQL

echo "==> Exporting target .ibd and SDI"
log_verbose "FLUSH TABLES $TABLE_NAME FOR EXPORT"
"${MYSQL[@]}" "$TGT_DB" -e "FLUSH TABLES $TABLE_NAME FOR EXPORT;"

if ! sudo test -f "$TGT_IBD_PATH"; then
  echo "Missing target .ibd at $TGT_IBD_PATH"
  "${MYSQL[@]}" "$TGT_DB" -e "UNLOCK TABLES;"
  exit 1
fi

sudo cp "$TGT_IBD_PATH" "$TARGET_IBD"
sudo chown "$(id -u)":"$(id -g)" "$TARGET_IBD"
"${MYSQL[@]}" "$TGT_DB" -e "UNLOCK TABLES;"

ibd2sdi "$TARGET_IBD" > "$TARGET_SDI"

echo "==> Source index ids"
extract_index_ids "$SOURCE_SDI" || true

echo "==> Target index ids"
extract_index_ids "$TARGET_SDI" || true

echo "==> Rebuilding with index-id remap"
"$IB_PARSER" 5 "$SOURCE_IBD" "$REBUILT_IBD" \
  --sdi-json="$SOURCE_SDI" \
  --target-sdi-json="$TARGET_SDI" \
  --target-ibd="$TARGET_IBD" \
  --use-target-sdi-root \
  --use-target-space-id \
  --cfg-out="$REBUILT_CFG"

echo "==> Dropping source database to avoid tablespace ID conflict"
log_verbose "DROP DATABASE IF EXISTS $SRC_DB"
"${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $SRC_DB;"

echo "==> Preparing target table for import"
log_verbose "ALTER TABLE $TABLE_NAME DISCARD TABLESPACE"
"${MYSQL[@]}" "$TGT_DB" -e "ALTER TABLE $TABLE_NAME DISCARD TABLESPACE;"

sudo systemctl stop mysql
sleep 2

if [ -f "$DATADIR/$SRC_DB/${TABLE_NAME}.ibd" ]; then
  sudo rm -f "$DATADIR/$SRC_DB/${TABLE_NAME}.ibd"
fi

sudo cp "$REBUILT_IBD" "$TGT_IBD_DEST"
sudo cp "$REBUILT_CFG" "$TGT_CFG_DEST"
sudo chown mysql:mysql "$TGT_IBD_DEST" "$TGT_CFG_DEST"
sudo chmod 640 "$TGT_IBD_DEST" "$TGT_CFG_DEST"

sudo systemctl start mysql
sleep 3
if ! wait_for_mysql; then
  echo "MySQL did not come up in time."
  exit 1
fi

echo "==> Importing tablespace"
"${MYSQL[@]}" "$TGT_DB" -e "ALTER TABLE $TABLE_NAME IMPORT TABLESPACE;"

echo "==> Verifying row count"
EXPECTED_ROWS=4
ACTUAL_ROWS=$("${MYSQL[@]}" -N -B "$TGT_DB" -e "SELECT COUNT(*) FROM $TABLE_NAME;")
if [ "$ACTUAL_ROWS" != "$EXPECTED_ROWS" ]; then
  echo "Row count mismatch: expected $EXPECTED_ROWS got $ACTUAL_ROWS"
  exit 1
fi

echo "OK: index-id remap IMPORT TABLESPACE succeeded."

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
DB_NAME=${DB_NAME:-test_cfg_import}
TABLE_NAME=${TABLE_NAME:-instant_cfg}

PARSER_DIR=${PARSER_DIR:-/home/cslog/mysql/innodb-parser}
IB_PARSER=${IB_PARSER:-$PARSER_DIR/build/ib_parser}
OUT_DIR=${OUT_DIR:-/tmp/ibd-cfg-import}

SOURCE_IBD="$OUT_DIR/source.ibd"
SOURCE_SDI="$OUT_DIR/source_sdi.json"
REBUILT_IBD="$OUT_DIR/rebuilt.ibd"
REBUILT_CFG="$OUT_DIR/rebuilt.cfg"

MYSQL=(mysql -u"$DB_USER")
if [ -n "$DB_PASS" ]; then
  MYSQL+=(-p"$DB_PASS")
fi

DATADIR=$("${MYSQL[@]}" -N -B -e "SELECT @@datadir;")
DATADIR=${DATADIR%/}
IBD_PATH="$DATADIR/$DB_NAME/${TABLE_NAME}.ibd"
TARGET_IBD="$DATADIR/$DB_NAME/${TABLE_NAME}.ibd"
TARGET_CFG="$DATADIR/$DB_NAME/${TABLE_NAME}.cfg"

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

echo "==========================================="
echo "Testing .cfg generation for IMPORT TABLESPACE"
echo "==========================================="

if [ ! -f "$IB_PARSER" ]; then
  echo "ib_parser not found, building..."
  make -C "$PARSER_DIR/build" -j"$(nproc)"
fi

echo "==> Creating compressed source table with instant column"
log_verbose "DROP DATABASE IF EXISTS $DB_NAME"
log_verbose "CREATE DATABASE $DB_NAME"
"${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $DB_NAME; CREATE DATABASE $DB_NAME;"

log_verbose "CREATE TABLE $TABLE_NAME (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50), qty INT) ROW_FORMAT=COMPRESSED"
"${MYSQL[@]}" "$DB_NAME" <<SQL
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(50),
  qty INT
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;

INSERT INTO $TABLE_NAME (name, qty) VALUES
  ('alpha', 10),
  ('beta', 20),
  ('gamma', 30);

-- ALGORITHM=INSTANT not supported on COMPRESSED tables, use INPLACE
ALTER TABLE $TABLE_NAME
  ADD COLUMN note VARCHAR(40) NOT NULL DEFAULT 'n/a',
  ALGORITHM=INPLACE;

INSERT INTO $TABLE_NAME (name, qty, note) VALUES
  ('delta', 40, 'new row'),
  ('epsilon', 50, 'new row');
SQL

echo "==> Exporting .ibd and SDI"
log_verbose "FLUSH TABLES $TABLE_NAME FOR EXPORT"
"${MYSQL[@]}" "$DB_NAME" -e "FLUSH TABLES $TABLE_NAME FOR EXPORT;"

if ! sudo test -f "$IBD_PATH"; then
  echo "Missing .ibd at $IBD_PATH"
  "${MYSQL[@]}" "$DB_NAME" -e "UNLOCK TABLES;"
  exit 1
fi

sudo cp "$IBD_PATH" "$SOURCE_IBD"
sudo chown "$(id -u)":"$(id -g)" "$SOURCE_IBD"
"${MYSQL[@]}" "$DB_NAME" -e "UNLOCK TABLES;"

ibd2sdi "$SOURCE_IBD" > "$SOURCE_SDI"

echo "==> Rebuilding and writing .cfg from SDI"
"$IB_PARSER" 5 "$SOURCE_IBD" "$REBUILT_IBD" \
  --sdi-json="$SOURCE_SDI" \
  --cfg-out="$REBUILT_CFG"

echo "==> Preparing target table for import"
log_verbose "DROP TABLE IF EXISTS $TABLE_NAME"
"${MYSQL[@]}" "$DB_NAME" -e "DROP TABLE IF EXISTS $TABLE_NAME;"

# Create target table with matching schema (DYNAMIC for rebuilt 16KB pages)
"${MYSQL[@]}" "$DB_NAME" <<SQL
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(50),
  qty INT,
  note VARCHAR(40) NOT NULL DEFAULT 'n/a'
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;
SQL

log_verbose "ALTER TABLE $TABLE_NAME DISCARD TABLESPACE"
"${MYSQL[@]}" "$DB_NAME" -e "ALTER TABLE $TABLE_NAME DISCARD TABLESPACE;"

echo "==> Copying rebuilt .ibd and .cfg"
sudo systemctl stop mysql
sleep 2

sudo cp "$REBUILT_IBD" "$TARGET_IBD"
sudo cp "$REBUILT_CFG" "$TARGET_CFG"
sudo chown mysql:mysql "$TARGET_IBD" "$TARGET_CFG"
sudo chmod 640 "$TARGET_IBD" "$TARGET_CFG"

sudo systemctl start mysql
sleep 3
if ! wait_for_mysql; then
  echo "MySQL did not come up in time."
  exit 1
fi

echo "==> Importing tablespace"
"${MYSQL[@]}" "$DB_NAME" -e "ALTER TABLE $TABLE_NAME IMPORT TABLESPACE;"

echo "==> Verifying row count"
EXPECTED_ROWS=5
ACTUAL_ROWS=$("${MYSQL[@]}" -N -B "$DB_NAME" -e "SELECT COUNT(*) FROM $TABLE_NAME;")
if [ "$ACTUAL_ROWS" != "$EXPECTED_ROWS" ]; then
  echo "Row count mismatch: expected $EXPECTED_ROWS got $ACTUAL_ROWS"
  exit 1
fi

echo "OK: .cfg generation and IMPORT TABLESPACE succeeded."

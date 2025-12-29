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
DB_NAME=${DB_NAME:-test_sdi_external}
TABLE_NAME=${TABLE_NAME:-sdi_external}
COLUMN_COUNT=${COLUMN_COUNT:-400}
COLUMN_COMMENT_BYTES=${COLUMN_COMMENT_BYTES:-512}

PARSER_DIR=${PARSER_DIR:-/home/cslog/mysql/percona-parser}
IB_PARSER=${IB_PARSER:-$PARSER_DIR/build/ib_parser}
OUT_DIR=${OUT_DIR:-/tmp/ibd-sdi-external}

SOURCE_IBD="$OUT_DIR/source.ibd"
SOURCE_SDI="$OUT_DIR/source_sdi.json"
REBUILT_IBD="$OUT_DIR/rebuilt.ibd"
REBUILT_SDI="$OUT_DIR/rebuilt_sdi.json"

MYSQL=(mysql -u"$DB_USER")
if [ -n "$DB_PASS" ]; then
  MYSQL+=(-p"$DB_PASS")
fi

DATADIR=$("${MYSQL[@]}" -N -B -e "SELECT @@datadir;")
DATADIR=${DATADIR%/}
IBD_PATH="$DATADIR/$DB_NAME/${TABLE_NAME}.ibd"

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
echo "Testing SDI rebuild with external SDI BLOB"
echo "==========================================="

if [ ! -f "$IB_PARSER" ]; then
  echo "ib_parser not found, building..."
  make -C "$PARSER_DIR/build" -j"$(nproc)"
fi

if ! wait_for_mysql; then
  echo "MySQL is not available."
  exit 1
fi

echo "==> Creating compressed table with many columns"
log_verbose "DROP DATABASE IF EXISTS $DB_NAME"
log_verbose "CREATE DATABASE $DB_NAME"
"${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $DB_NAME; CREATE DATABASE $DB_NAME;"

col_output=$(COLUMN_COUNT="$COLUMN_COUNT" COLUMN_COMMENT_BYTES="$COLUMN_COMMENT_BYTES" python3 - <<'PY'
import hashlib
import os

count = int(os.environ["COLUMN_COUNT"])
comment_len = int(os.environ["COLUMN_COMMENT_BYTES"])

def make_comment(seed):
    out = []
    counter = 0
    while len("".join(out)) < comment_len:
        h = hashlib.sha256(f"{seed}-{counter}".encode()).hexdigest()
        out.append(h)
        counter += 1
    return "".join(out)[:comment_len]

cols = []
names = []
for i in range(1, count + 1):
    name = f"c{i:03d}"
    comment = make_comment(name)
    cols.append(f"{name} INT COMMENT '{comment}'")
    names.append(name)

print("|".join(cols))
print("|".join(names))
PY
)

col_defs_str=${col_output%%$'\n'*}
col_names_str=${col_output#*$'\n'}
col_defs_str=${col_defs_str//|/,}
col_names_str=${col_names_str//|/,}
values=$(printf '0%.0s,' $(seq 1 "$COLUMN_COUNT"))
values=${values%,}

SQL_FILE="$OUT_DIR/sdi_external_schema.sql"
cat > "$SQL_FILE" <<SQL
CREATE TABLE $TABLE_NAME (id INT PRIMARY KEY, $col_defs_str) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;
INSERT INTO $TABLE_NAME (id, $col_names_str) VALUES (1, $values);
SQL

"${MYSQL[@]}" "$DB_NAME" < "$SQL_FILE"

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

echo "==> Rebuilding with --sdi-json"
"$IB_PARSER" 5 "$SOURCE_IBD" "$REBUILT_IBD" --sdi-json="$SOURCE_SDI"

echo "==> Validating rebuilt SDI"
if ! ibd2sdi "$REBUILT_IBD" > "$REBUILT_SDI" 2>/tmp/sdi_external_err.log; then
  echo "ibd2sdi failed on rebuilt file."
  cat /tmp/sdi_external_err.log
  exit 1
fi

PAGE_SUMMARY=$(innochecksum -S "$REBUILT_IBD" 2>/dev/null || true)
SDI_BLOB_COUNT=$(echo "$PAGE_SUMMARY" | awk '$2 == "SDI" && $3 == "BLOB" {print $1}')
if [ -z "$SDI_BLOB_COUNT" ]; then
  SDI_BLOB_COUNT=0
fi

if [ "$SDI_BLOB_COUNT" -lt 1 ]; then
  echo "Expected SDI BLOB pages in rebuilt file, found $SDI_BLOB_COUNT."
  echo "$PAGE_SUMMARY"
  exit 1
fi

echo "OK: external SDI BLOB pages rebuilt ($SDI_BLOB_COUNT pages)."

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
DB_NAME=${DB_NAME:-test_json_decode}
TABLE_NAME=${TABLE_NAME:-json_fixture}

PARSER_DIR=${PARSER_DIR:-/home/cslog/mysql/innodb-parser}
IB_PARSER=${IB_PARSER:-$PARSER_DIR/build/ib_parser}
OUT_IBD=${OUT_IBD:-$PARSER_DIR/tests/json_test.ibd}
OUT_SDI=${OUT_SDI:-$PARSER_DIR/tests/json_test_sdi.json}
OUT_DIR=${OUT_DIR:-/tmp/ibd-json-validate}

MYSQL=(mysql --default-character-set=utf8mb4 -u"$DB_USER")
if [ -n "$DB_PASS" ]; then
  MYSQL+=(-p"$DB_PASS")
fi

DATADIR=$("${MYSQL[@]}" -N -B -e "SELECT @@datadir;")
DATADIR=${DATADIR%/}
IBD_PATH="$DATADIR/$DB_NAME/${TABLE_NAME}.ibd"

echo "==> Creating fixture schema and data"
log_verbose "DROP DATABASE IF EXISTS $DB_NAME"
log_verbose "CREATE DATABASE $DB_NAME"
"${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $DB_NAME; CREATE DATABASE $DB_NAME;"

log_verbose "CREATE TABLE $TABLE_NAME with JSON column"
"${MYSQL[@]}" "$DB_NAME" <<SQL
SET NAMES utf8mb4;
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY,
  doc JSON
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

INSERT INTO $TABLE_NAME (id, doc) VALUES
  (1, JSON_OBJECT(
    'name','Alice',
    'age', 30,
    'active', true,
    'tags', JSON_ARRAY('blue','green'),
    'meta', JSON_OBJECT('k','v')
  )),
  (2, JSON_ARRAY(1,2,3, JSON_OBJECT('k','v'), null, false)),
  (3, JSON_OBJECT(
    'unicode', CONVERT(0x436166C3A920F09F9A80 USING utf8mb4),
    'quote', 'He said hi',
    'path', 'a/b'
  ));
SQL

echo "==> Exporting .ibd and SDI"
log_verbose "FLUSH TABLES $TABLE_NAME FOR EXPORT"
"${MYSQL[@]}" "$DB_NAME" -e "FLUSH TABLES $TABLE_NAME FOR EXPORT;"

if ! sudo test -f "$IBD_PATH"; then
  echo "Missing .ibd at $IBD_PATH"
  "${MYSQL[@]}" "$DB_NAME" -e "UNLOCK TABLES;"
  exit 1
fi

sudo cp "$IBD_PATH" "$OUT_IBD"
sudo chown "$(id -u)":"$(id -g)" "$OUT_IBD"
"${MYSQL[@]}" "$DB_NAME" -e "UNLOCK TABLES;"

ibd2sdi "$OUT_IBD" > "$OUT_SDI"

echo "==> Parsing with ib_parser"
mkdir -p "$OUT_DIR"
parser_out="$OUT_DIR/ib_parser.jsonl"
"$IB_PARSER" 3 "$OUT_IBD" "$OUT_SDI" \
  --format=jsonl \
  --output="$parser_out" \
  > "$OUT_DIR/ib_parser.log"

echo "==> Comparing against MySQL output"
mysql_out="$OUT_DIR/mysql.jsonl"
"${MYSQL[@]}" -N -B \
  -e "SET NAMES utf8mb4; \
      SELECT JSON_OBJECT( \
        'id', id, \
        'doc', doc \
      ) FROM ${DB_NAME}.${TABLE_NAME} ORDER BY id;" \
  > "$mysql_out"

python3 - "$parser_out" "$OUT_DIR/ib_parser.norm" <<'PY'
import json
import sys

inp, outp = sys.argv[1], sys.argv[2]
with open(inp, "r", encoding="utf-8") as f, open(outp, "w", encoding="utf-8") as out:
    for line in f:
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        out.write(json.dumps(obj, sort_keys=True, separators=(",", ":")) + "\n")
PY

python3 - "$mysql_out" "$OUT_DIR/mysql.norm" <<'PY'
import json
import sys

inp, outp = sys.argv[1], sys.argv[2]
with open(inp, "r", encoding="utf-8") as f, open(outp, "w", encoding="utf-8") as out:
    for line in f:
        line = line.strip()
        if not line:
            continue
        obj = json.loads(line)
        out.write(json.dumps(obj, sort_keys=True, separators=(",", ":")) + "\n")
PY

sort "$OUT_DIR/ib_parser.norm" > "$OUT_DIR/ib_parser.sorted"
sort "$OUT_DIR/mysql.norm" > "$OUT_DIR/mysql.sorted"
if diff -u "$OUT_DIR/mysql.sorted" "$OUT_DIR/ib_parser.sorted"; then
  echo "OK: ib_parser output matches MySQL."
else
  echo "Mismatch: see $OUT_DIR for details."
  exit 1
fi

echo "==> Fixture written to:"
echo "    $OUT_IBD"
echo "    $OUT_SDI"

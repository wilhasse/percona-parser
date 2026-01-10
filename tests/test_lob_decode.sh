#!/usr/bin/env bash
set -euo pipefail

# Verbose output helper
VERBOSE=${VERBOSE:-0}
log_verbose() {
    if [ "$VERBOSE" = "1" ]; then
        echo -e "\033[0;36m  [SQL] $1\033[0m"
    fi
}

DB_USER=${DB_USER:-root}
DB_PASS=${DB_PASS:-}
DB_NAME=${DB_NAME:-test_lob_decode}
TABLE_NAME=${TABLE_NAME:-lob_fixture}
TZ_NAME=${TZ_NAME:-America/Sao_Paulo}

PARSER_DIR=${PARSER_DIR:-/home/cslog/mysql/innodb-parser}
IB_PARSER=${IB_PARSER:-$PARSER_DIR/build/ib_parser}
OUT_IBD=${OUT_IBD:-$PARSER_DIR/tests/lob_test.ibd}
OUT_SDI=${OUT_SDI:-$PARSER_DIR/tests/lob_test_sdi.json}
OUT_DIR=${OUT_DIR:-/tmp/ibd-lob-validate}
LOB_MAX_BYTES=${LOB_MAX_BYTES:-4000000}

MYSQL=(mysql -u"$DB_USER")
if [ -n "$DB_PASS" ]; then
  MYSQL+=(-p"$DB_PASS")
fi

DATADIR=$("${MYSQL[@]}" -N -B -e "SELECT @@datadir;")
DATADIR=${DATADIR%/}
IBD_PATH="$DATADIR/$DB_NAME/${TABLE_NAME}.ibd"

MYSQL_TZ="$TZ_NAME"
if ! "${MYSQL[@]}" -N -B -e "SET time_zone='${MYSQL_TZ}'; SELECT 1;" >/dev/null 2>&1; then
  MYSQL_TZ="-03:00"
fi
echo "Using MySQL time_zone: ${MYSQL_TZ}"

echo "==> Creating fixture schema and data"
log_verbose "DROP DATABASE IF EXISTS $DB_NAME"
log_verbose "CREATE DATABASE $DB_NAME"
"${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $DB_NAME; CREATE DATABASE $DB_NAME;"

log_verbose "CREATE TABLE $TABLE_NAME (id INT, note VARCHAR(50), big_text LONGTEXT, big_blob LONGBLOB) ROW_FORMAT=DYNAMIC"
"${MYSQL[@]}" "$DB_NAME" <<SQL
SET time_zone='${MYSQL_TZ}';
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY,
  note VARCHAR(50),
  big_text LONGTEXT,
  big_blob LONGBLOB
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

SET @txt = REPEAT('abcdefghijklmnopqrstuvwxyz', 4000);
SET @blob = UNHEX(REPEAT('ABCD', 20000));

INSERT INTO $TABLE_NAME (id, note, big_text, big_blob)
VALUES
  (1, 'alpha', @txt, @blob),
  (2, 'beta', CONCAT(@txt, 'Z'), UNHEX(REPEAT('1234', 15000)));
SQL
log_verbose "INSERT INTO $TABLE_NAME: 2 rows with LONGTEXT (~104KB) and LONGBLOB (~40KB/30KB)"

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
IB_PARSER_TZ="$TZ_NAME" "$IB_PARSER" 3 "$OUT_IBD" "$OUT_SDI" \
  --format=jsonl \
  --output="$parser_out" \
  --lob-max-bytes="$LOB_MAX_BYTES" \
  > "$OUT_DIR/ib_parser.log"

echo "==> Comparing against MySQL output"
mysql_out="$OUT_DIR/mysql.jsonl"
"${MYSQL[@]}" -N -B \
  -e "SET time_zone='${MYSQL_TZ}'; \
      SELECT JSON_OBJECT( \
        'id', id, \
        'note', note, \
        'big_text', big_text, \
        'big_blob', HEX(big_blob) \
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

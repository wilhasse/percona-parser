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
DB_NAME=${DB_NAME:-test_charset_decode}
TABLE_NAME=${TABLE_NAME:-charset_fixture}

PARSER_DIR=${PARSER_DIR:-/home/cslog/mysql/percona-parser}
IB_PARSER=${IB_PARSER:-$PARSER_DIR/build/ib_parser}
OUT_IBD=${OUT_IBD:-$PARSER_DIR/tests/charset_test.ibd}
OUT_SDI=${OUT_SDI:-$PARSER_DIR/tests/charset_test_sdi.json}
OUT_DIR=${OUT_DIR:-/tmp/ibd-charset-validate}

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

log_verbose "CREATE TABLE $TABLE_NAME with latin1 + utf8mb4 columns"
"${MYSQL[@]}" "$DB_NAME" <<SQL
SET NAMES utf8mb4;
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY,
  latin1_text VARCHAR(50) CHARACTER SET latin1 COLLATE latin1_swedish_ci,
  utf8_text VARCHAR(50) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci,
  emoji_text VARCHAR(50) CHARACTER SET utf8mb4,
  notes TEXT CHARACTER SET utf8mb4
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

INSERT INTO $TABLE_NAME
  (id, latin1_text, utf8_text, emoji_text, notes)
VALUES
  (1, 'Café', 'São Paulo', 'Rocket 🚀', 'Emoji 😀 and accents: áéíóú'),
  (2, 'Niño', 'München', 'Smile 🙂', 'naïve façade');
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
        'latin1_text', latin1_text, \
        'utf8_text', utf8_text, \
        'emoji_text', emoji_text, \
        'notes', notes \
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

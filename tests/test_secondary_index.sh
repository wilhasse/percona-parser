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
DB_NAME=${DB_NAME:-test_secondary_index}
TABLE_NAME=${TABLE_NAME:-idx_fixture}
INDEX_NAME=${INDEX_NAME:-idx_ab}

PARSER_DIR=${PARSER_DIR:-/home/cslog/mysql/percona-parser}
IB_PARSER=${IB_PARSER:-$PARSER_DIR/build/ib_parser}
OUT_IBD=${OUT_IBD:-$PARSER_DIR/tests/secondary_index.ibd}
OUT_SDI=${OUT_SDI:-$PARSER_DIR/tests/secondary_index_sdi.json}
OUT_DIR=${OUT_DIR:-/tmp/ibd-secondary-index}

MYSQL=(mysql -u"$DB_USER")
if [ -n "$DB_PASS" ]; then
  MYSQL+=(-p"$DB_PASS")
fi

DATADIR=$("${MYSQL[@]}" -N -B -e "SELECT @@datadir;")
DATADIR=${DATADIR%/}
IBD_PATH="$DATADIR/$DB_NAME/${TABLE_NAME}.ibd"

mkdir -p "$OUT_DIR"

if [ ! -f "$IB_PARSER" ]; then
  echo "ib_parser not found, building..."
  make -C "$PARSER_DIR/build" -j"$(nproc)"
fi

echo "==> Creating fixture schema and data"
log_verbose "DROP DATABASE IF EXISTS $DB_NAME"
log_verbose "CREATE DATABASE $DB_NAME"
"${MYSQL[@]}" -e "DROP DATABASE IF EXISTS $DB_NAME; CREATE DATABASE $DB_NAME;"

"${MYSQL[@]}" "$DB_NAME" <<SQL
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY,
  a INT,
  b VARCHAR(20),
  c INT,
  KEY idx_ab (a, b),
  KEY idx_c (c)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

INSERT INTO $TABLE_NAME (id, a, b, c) VALUES
  (1, 10, 'alpha', 100),
  (2, 20, 'bravo', 200),
  (3, 10, 'charlie', 300),
  (4, 20, 'delta', 400);
SQL

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

echo "==> Parsing secondary index with ib_parser"
parser_out="$OUT_DIR/ib_parser.jsonl"
"$IB_PARSER" 3 "$OUT_IBD" "$OUT_SDI" \
  --index="$INDEX_NAME" \
  --format=jsonl \
  --output="$parser_out" \
  > "$OUT_DIR/ib_parser.log"

echo "==> Comparing against MySQL output"
mysql_out="$OUT_DIR/mysql.jsonl"
"${MYSQL[@]}" -N -B \
  -e "SELECT JSON_OBJECT('a', a, 'b', b, 'id', id)
      FROM ${DB_NAME}.${TABLE_NAME}
      ORDER BY a, b, id;" \
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
  echo "OK: secondary index output matches MySQL."
else
  echo "Mismatch: see $OUT_DIR for details."
  exit 1
fi

echo "==> Fixture written to:"
echo "    $OUT_IBD"
echo "    $OUT_SDI"

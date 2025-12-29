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
DB_NAME=${DB_NAME:-test_types_decode}
TABLE_NAME=${TABLE_NAME:-types_fixture}
TZ_NAME=${TZ_NAME:-America/Sao_Paulo}

PARSER_DIR=${PARSER_DIR:-/home/cslog/mysql/percona-parser}
IB_PARSER=${IB_PARSER:-$PARSER_DIR/build/ib_parser}
OUT_IBD=${OUT_IBD:-$PARSER_DIR/tests/types_test.ibd}
OUT_SDI=${OUT_SDI:-$PARSER_DIR/tests/types_test_sdi.json}
OUT_DIR=${OUT_DIR:-/tmp/ibd-types-validate}

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

log_verbose "CREATE TABLE $TABLE_NAME with columns: id INT, amount DECIMAL(10,2), d DATE, t TIME(6), dt DATETIME(6), ts TIMESTAMP, y YEAR, e ENUM, s SET, b BIT(10), note VARCHAR(50)"
"${MYSQL[@]}" "$DB_NAME" <<SQL
SET time_zone='${MYSQL_TZ}';
CREATE TABLE $TABLE_NAME (
  id INT PRIMARY KEY,
  amount DECIMAL(10,2),
  d DATE,
  t TIME(6),
  dt DATETIME(6),
  ts TIMESTAMP(0) NULL DEFAULT NULL,
  y YEAR,
  e ENUM('small','medium','large') NOT NULL,
  s SET('red','green','blue'),
  b BIT(10),
  note VARCHAR(50)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

INSERT INTO $TABLE_NAME
  (id, amount, d, t, dt, ts, y, e, s, b, note)
VALUES
  (1, 1234.56, '2024-12-31', '12:34:56.123456', '2024-12-31 12:34:56.123456',
   '2024-12-31 12:34:56', 2024, 'medium', 'red,blue', b'1010101010', 'alpha'),
  (2, -0.99, '2001-01-02', '01:02:03.000004', '2001-01-02 03:04:05.000006',
   NULL, 1999, 'small', 'green', b'0000000001', 'beta');
SQL
log_verbose "INSERT INTO $TABLE_NAME VALUES (1, 1234.56, '2024-12-31', ...), (2, -0.99, '2001-01-02', ...)"

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
  > "$OUT_DIR/ib_parser.log"

echo "==> Comparing against MySQL output"
mysql_out="$OUT_DIR/mysql.jsonl"
"${MYSQL[@]}" -N -B \
  -e "SET time_zone='${MYSQL_TZ}'; \
      SELECT JSON_OBJECT( \
        'id', id, \
        'amount', amount, \
        'd', DATE_FORMAT(d, '%Y-%m-%d'), \
        't', DATE_FORMAT(t, '%H:%i:%s.%f'), \
        'dt', DATE_FORMAT(dt, '%Y-%m-%d %H:%i:%s.%f'), \
        'ts', IF(ts IS NULL, NULL, DATE_FORMAT(ts, '%Y-%m-%d %H:%i:%s')), \
        'y', LPAD(y, 4, '0'), \
        'e', e, \
        's', s, \
        'b', CAST(b AS UNSIGNED), \
        'note', note \
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

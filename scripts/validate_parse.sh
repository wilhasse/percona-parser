#!/usr/bin/env bash
set -euo pipefail

IB_PARSER=${IB_PARSER:-./build/ib_parser}
IBD=${IBD:-tests/compressed_test_rebuilt.ibd}
SDI=${SDI:-tests/compressed_test_sdi.json}
OUT_DIR=${OUT_DIR:-/tmp/ibd-validate}
FORMAT=${FORMAT:-jsonl}
TZ_NAME=${IB_PARSER_TZ:-America/Sao_Paulo}

# Optional MySQL compare
MYSQL_DB=${MYSQL_DB:-}
MYSQL_TABLE=${MYSQL_TABLE:-}
MYSQL_OPTS=${MYSQL_OPTS:-}

# Optional undrop-for-innodb compare
UNDROP_BIN=${UNDROP_BIN:-}
UNDROP_INPUT=${UNDROP_INPUT:-}
UNDROP_TABLE_SQL=${UNDROP_TABLE_SQL:-}
UNDROP_EXTRA_OPTS=${UNDROP_EXTRA_OPTS:-}

mkdir -p "$OUT_DIR"

parser_out="$OUT_DIR/ib_parser.${FORMAT}"
IB_PARSER_TZ="$TZ_NAME" "$IB_PARSER" 3 "$IBD" "$SDI" \
  --format="$FORMAT" \
  --output="$parser_out" \
  > "$OUT_DIR/ib_parser.log"

if [ "$FORMAT" = "jsonl" ]; then
  parser_compare="$parser_out"
else
  parser_compare="$OUT_DIR/ib_parser.jsonl"
  IB_PARSER_TZ="$TZ_NAME" "$IB_PARSER" 3 "$IBD" "$SDI" \
    --format=jsonl \
    --output="$parser_compare" \
    > "$OUT_DIR/ib_parser_jsonl.log"
fi

if [ "$FORMAT" = "jsonl" ]; then
  parser_count=$(wc -l < "$parser_out")
else
  parser_count=$(tail -n +2 "$parser_out" | wc -l)
fi
echo "ib_parser rows: $parser_count"

if [ -n "$MYSQL_DB" ] && [ -n "$MYSQL_TABLE" ]; then
  if command -v mysql >/dev/null 2>&1; then
    mysql_out="$OUT_DIR/mysql.jsonl"
    mysql $MYSQL_OPTS -N -B \
      -e "SET time_zone='${TZ_NAME}'; SELECT JSON_OBJECT('id',id,'name',name,'description',description,'created_at',DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s')) FROM ${MYSQL_DB}.${MYSQL_TABLE} ORDER BY id;" \
      > "$mysql_out"

    mysql_count=$(wc -l < "$mysql_out")
    echo "mysql rows: $mysql_count"

    sort "$parser_compare" > "$OUT_DIR/ib_parser.sorted"
    sort "$mysql_out" > "$OUT_DIR/mysql.sorted"
    if ! diff -u "$OUT_DIR/mysql.sorted" "$OUT_DIR/ib_parser.sorted"; then
      echo "Row diff detected (see sorted outputs in $OUT_DIR)."
      exit 1
    fi
  else
    echo "mysql client not found; skipping MySQL compare."
  fi
else
  echo "MYSQL_DB or MYSQL_TABLE not set; skipping MySQL compare."
fi

if [ -n "$UNDROP_BIN" ] && [ -n "$UNDROP_INPUT" ] && [ -n "$UNDROP_TABLE_SQL" ]; then
  undrop_out="$OUT_DIR/undrop.out"
  "$UNDROP_BIN" -6 -U -f "$UNDROP_INPUT" -t "$UNDROP_TABLE_SQL" \
    -o "$undrop_out" $UNDROP_EXTRA_OPTS
  echo "undrop output written to $undrop_out"
else
  echo "UNDROP_BIN/UNDROP_INPUT/UNDROP_TABLE_SQL not set; skipping undrop compare."
fi

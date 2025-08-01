#!/usr/bin/env bash
# Tags: long, no-async-insert
# no-async-insert: https://github.com/ClickHouse/ClickHouse/issues/80105

set -e

CLICKHOUSE_CLIENT_SERVER_LOGS_LEVEL=none

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} --query="DROP TABLE IF EXISTS t"
${CLICKHOUSE_CLIENT} --query="CREATE TABLE t (x Int8) ENGINE = MergeTree ORDER BY ()"


function thread_optimize()
{
    local TIMELIMIT=$((SECONDS+TIMEOUT))
    while [ $SECONDS -lt "$TIMELIMIT" ]
    do
        ${CLICKHOUSE_CLIENT} --query="OPTIMIZE TABLE t FINAL;" 2>&1 | tr -d '\n' | rg -v 'Cancelled merging parts' ||:
    done
}

TIMEOUT=15
thread_optimize 2> /dev/null &

for i in {1..100};
do
    echo "
        INSERT INTO t VALUES (0);
        INSERT INTO t VALUES (0);
        TRUNCATE TABLE t;
        SELECT count() FROM t HAVING count() > 0;
        SELECT ${i};
        "
done | ${CLICKHOUSE_CLIENT}

wait

$CLICKHOUSE_CLIENT -q "DROP TABLE t"

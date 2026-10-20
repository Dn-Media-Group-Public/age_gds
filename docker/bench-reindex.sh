#!/bin/bash
# Throw-away benchmark: no volumes, nothing persists.
#   docker/bench-reindex.sh [nv ne [qmode [weighted]]]   (defaults 300000 750000 omitted false)
#   qmode: omitted (whole graph via SQL) | cypher (explicit whole-graph queries)
# Needs the image from `docker build -t age_gds-test .`
set -euo pipefail
NV=${1:-300000}; NE=${2:-750000}; QMODE=${3:-omitted}; WT=${4:-false}
docker run --rm -v "$(cd "$(dirname "$0")" && pwd)/bench-reindex.sql:/bench.sql:ro" age_gds-test bash -c "
set -e
su postgres -c '/usr/lib/postgresql/16/bin/initdb -D /tmp/pgd' >/dev/null
su postgres -c '/usr/lib/postgresql/16/bin/pg_ctl -D /tmp/pgd -l /tmp/pg.log -w start' >/dev/null
su postgres -c 'createdb benchdb'
su postgres -c \"psql -d benchdb -f /bench.sql -v nv=$NV -v ne=$NE -v qmode=$QMODE -v wt=$WT\"
"

#!/bin/bash
# Throw-away validation of index/reindex content against SQL ground truth.
#   docker/validate-reindex.sh [image]    (default age_gds-test; baseline: age_gds-base)
set -euo pipefail
IMG=${1:-age_gds-test}
docker run --rm -v "$(cd "$(dirname "$0")" && pwd)/validate-reindex.sql:/v.sql:ro" "$IMG" bash -c "
set -e
su postgres -c '/usr/lib/postgresql/16/bin/initdb -D /tmp/pgd' >/dev/null 2>&1
su postgres -c '/usr/lib/postgresql/16/bin/pg_ctl -D /tmp/pgd -l /tmp/pg.log -w start' >/dev/null
su postgres -c 'createdb vdb'
su postgres -c 'psql -d vdb -f /v.sql' 2>&1
"

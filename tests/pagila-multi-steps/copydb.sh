#! /bin/bash

set -x
set -e

# This script expects the following environment variables to be set:
#
#  - PGCOPYDB_SOURCE_PGURI
#  - PGCOPYDB_TARGET_PGURI
#  - PGCOPYDB_TABLE_JOBS
#  - PGCOPYDB_INDEX_JOBS

#
# pgcopydb list extensions include a retry loop, so we use that as a proxy
# to depend on the source/target Postgres images to be ready
#
pgcopydb list extensions --source ${PGCOPYDB_SOURCE_PGURI}
pgcopydb list extensions --source ${PGCOPYDB_TARGET_PGURI}

psql -o /tmp/d.out -d ${PGCOPYDB_SOURCE_PGURI} -1 -f /usr/src/pagila/pagila-schema.sql
psql -o /tmp/s.out -d ${PGCOPYDB_SOURCE_PGURI} -1 -f /usr/src/pagila/pagila-data.sql

psql -d ${PGCOPYDB_TARGET_PGURI} <<EOF
alter database postgres connection limit 2;
EOF

#
# pgcopydb uses the environment variables
#
# we need to export a snapshot, and keep it while the indivual steps are
# running, one at a time

coproc ( pgcopydb snapshot -vv )

sleep 1

pgcopydb dump schema --resume -vv
pgcopydb restore pre-data --resume

pgcopydb copy table-data --resume
pgcopydb copy sequences --resume
pgcopydb copy blobs --resume
pgcopydb copy indexes --resume
pgcopydb copy constraints --resume

pgcopydb restore post-data --resume

kill -TERM ${COPROC_PID}
wait ${COPROC_PID}

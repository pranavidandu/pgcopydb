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
pgcopydb list extensions --source "${PGCOPYDB_SOURCE_PGURI}"

psql -a -d "${PGCOPYDB_SOURCE_PGURI}" -1 -f ./setup/setup.sql

# create the target needed collation manually for the test
psql -a -d "${PGCOPYDB_TARGET_PGURI}" -1 <<EOF
create collation if not exists mycol
 (
   locale = 'fr-FR-x-icu',
   provider = 'icu'
 );
EOF

pgcopydb list collations --source "${PGCOPYDB_SOURCE_PGURI}"

# pgcopydb fork uses the environment variables
pgcopydb fork --skip-collations --debug

# now compare the output of running the SQL command with what's expected
# as we're not root when running tests, can't write in /usr/src
mkdir -p /tmp/results

find .

pgopts="--single-transaction --no-psqlrc --expanded"

for f in ./sql/*.sql
do
    t=`basename $f .sql`
    r=/tmp/results/${t}.out
    e=./expected/${t}.out
    psql -d "${PGCOPYDB_TARGET_PGURI}" ${pgopts} --file ./sql/$t.sql &> $r
    test -f $e || cat $r
    diff $e $r || exit 1
done

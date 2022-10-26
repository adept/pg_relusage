#!/bin/bash
set -euxo pipefail

ver="$1"

docker build . --build-arg "PG_VER=${ver}" --tag pg_relusage_test:${ver}

docker run --name pg_relusage_test_${ver} -d --rm -p 5432:5432 pg_relusage_test:${ver}

sleep 2

PGPASSWORD=relusage psql -h localhost -d relusage -U relusage --echo-queries --quiet < sql/pg_relusage.sql > expected/pg_relusage_${ver}.out 2>&1

docker stop pg_relusage_test_${ver}

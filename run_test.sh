#!/bin/bash
# Run regression tests against the given PostgreSQL major version in Docker.
#
# Usage: ./run_test.sh <version>
#
# Actual output is saved to results/pg_relusage_<version>.out.
# Set ACCEPT=1 to copy it over expected/pg_relusage_<version>.out when tests fail.
# Set REBUILD=1 to rebuild the cached test image (e.g. to pick up new minor releases).
set -euo pipefail

ver="$1"
image="pg_relusage-test:${ver}"

build_opts=()
[ -n "${REBUILD:-}" ] && build_opts=(--pull --no-cache)
docker build --quiet "${build_opts[@]}" --build-arg PGVERSION="${ver}" -t "${image}" test/ >/dev/null

mkdir -p results

# Tests run on a copy of the source tree, so that several versions can be tested in parallel
status=0
docker run --rm -e AS_USER=worker -e LOCAL_UID="$(id -u)" \
    --volume "$PWD:/src" "${image}" bash -c "
      set -eu
      cp -r /src /tmp/build && cd /tmp/build
      make clean >/dev/null
      sudo pg_createcluster --start ${ver} test -p 5432 -- -A trust >/dev/null
      cp expected/pg_relusage_${ver}.out expected/pg_relusage.out
      status=0
      pg-build-test || status=\$?
      [ -f results/pg_relusage.out ] && cp results/pg_relusage.out /src/results/pg_relusage_${ver}.out
      exit \$status
    " || status=$?

if [ "${status}" -ne 0 ] && [ -n "${ACCEPT:-}" ] && [ -f "results/pg_relusage_${ver}.out" ]; then
    cp "results/pg_relusage_${ver}.out" "expected/pg_relusage_${ver}.out"
    echo "PostgreSQL ${ver}: expected output updated from results/pg_relusage_${ver}.out"
fi

exit "${status}"

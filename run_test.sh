#!/bin/bash
# Run regression tests against the given PostgreSQL major version in Docker.
#
# Usage: ./run_test.sh <version>
#
# Actual output is saved to results/<version>/.
# Set ACCEPT=1 to copy it over the files in expected/ when tests fail.
# Set REBUILD=1 to rebuild the cached test image (e.g. to pick up new minor releases).
set -euo pipefail

ver="$1"
[[ "${ver}" =~ ^[0-9]+$ ]] || { echo "Usage: $0 <PostgreSQL major version>" >&2; exit 2; }
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
      status=0
      pg-build-test || status=\$?
      rm -rf /src/results/${ver} && mkdir -p /src/results/${ver}
      cp results/*.out /src/results/${ver}/ 2>/dev/null || true
      exit \$status
    " || status=$?

if [ "${status}" -ne 0 ] && [ -n "${ACCEPT:-}" ]; then
    for f in results/"${ver}"/*.out; do
        [ -f "${f}" ] || continue
        cp "${f}" expected/
        echo "PostgreSQL ${ver}: expected/$(basename "${f}") updated from ${f}"
    done
fi

exit "${status}"

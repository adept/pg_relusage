#!/bin/bash
# Run regression tests against all supported PostgreSQL versions, see run_test.sh.
set -uo pipefail

failed=()
for ver in 10 11 12 13 14 15 16 17 18; do
    ./run_test.sh "${ver}" || failed+=("${ver}")
done

if [ ${#failed[@]} -ne 0 ]; then
    echo "FAILED on PostgreSQL: ${failed[*]}"
    exit 1
fi
echo "All versions passed"

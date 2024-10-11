#!/bin/bash
set -euxo pipefail

for ver in 10 11 12 13 14 15 16 17; do
    ./run_test.sh ${ver}
done

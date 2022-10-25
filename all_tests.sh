#!/bin/bash
setopt -e -o pipefail

for ver in 10 11 12 13 14 15 ; do
    ./run_test.sh ${ver}
done

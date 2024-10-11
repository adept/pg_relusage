#!/bin/bash
set -euxo pipefail

ver="$1"

rm -f *.o *.so *.bc

docker run -it --rm -w /repo -e AS_USER=worker -e LOCAL_UID=$(id -u) \
    --volume "$PWD:/repo" pgxn/pgxn-tools \
    sh -c "sudo pg-start ${ver} && cp expected/pg_relusage_${ver}.out expected/pg_relusage.out && (pg-build-test || mv results/pg_relusage.out expected/pg_relusage_${ver}.out)"

rm -f expected/pg_relusage.out

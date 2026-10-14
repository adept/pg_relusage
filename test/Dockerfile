# Test image with PostgreSQL server and dev packages preinstalled, so that test
# runs do not have to download them every time. Built by run_test.sh.
FROM pgxn/pgxn-tools
ARG PGVERSION
RUN test -n "${PGVERSION}" && NO_CLUSTER=1 pg-start "${PGVERSION}" && rm -rf /var/lib/apt/lists/*

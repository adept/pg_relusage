ARG PG_VER
FROM postgres:${PG_VER}-bullseye
ARG PG_VER

# Variables needed at runtime to configure postgres and run the initdb scripts
ENV POSTGRES_DB 'relusage'
ENV POSTGRES_USER 'relusage'
ENV POSTGRES_PASSWORD 'relusage'

# Copy in the extension source & build
RUN apt-get update && apt-get install -y make gcc postgresql-server-dev-${PG_VER} && rm -rf /var/lib/apt/lists
RUN mkdir /tmp/pg_relusage

COPY pg_relusage.c pg_relusage.control pg_relusage--0.0.1.sql Makefile META.json /tmp/pg_relusage/
RUN cd tmp/pg_relusage && make install


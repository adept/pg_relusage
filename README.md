# pg_relusage

pg_relusage is a PostgreSQL extension which allows one to discover and log the relations used in SQL statements. Supported PostgreSQL versions: 9.5 to 15

![regression tests](https://github.com/adept/pg_relusage/actions/workflows/ci.yml/badge.svg)

## Why?

This extension will be useful if you are dealing with (large?) legacy database and suspect that it contains plenty of unused objects.

This extension will allow you to quickly get an understanding of which clients use which objects / relations. Unlike statement log, which will only show you the views and tables explicitly referenced by the query, this extension will cut look at the result of view expansion, unused joins elimination etc etc and report the relations that were *actually* used by the statement.

## Installation

Extension hooks into PostgreSQL query executor and therefore needs to be compiled using PostgreSQL headers for the appropriate server version,
and then loaded into server process.

Extension uses standard PGXS build infrastructure and (provided that `pg_config` is somewhere in your PATH) could be built with `make install`.

## Usage

After installation, extension has to be either loaded into a single client process via [load](https://www.postgresql.org/docs/current/sql-load.html) statement or enabled for all sessions globally via [shared_preload_libraries](https://postgresqlco.nf/doc/en/param/shared_preload_libraries/) configuration parameter.

Once extension is loaded, each SQL statement will emit one extra log message which will list all the referenced relations (by name).

Statement:

```
select * from pg_stats limit 1;
```

should produce log message along the lines of:

```
relations used: pg_stats,pg_statistic,pg_class,pg_attribute,pg_namespace
```

## Configuration

Extension provides two user settings:

* pg_relusage.log_level (defaults to `LOG`), with the obvious meaning and the same set of values that similar configuration items (like `client_min_messages` support)
* pg_relusage.rel_kinds (defaults to `'riSvmfp'`) which specifies which relation kinds will be reported. This is a list of one-letter codes used in the `relkind` field in the `pg_class` (full list could be seen [here](https://www.postgresql.org/docs/current/catalog-pg-class.html)).

## Limitations

The earliest supported version is PostgreSQL v9.5.

## Development and testing

You can run `./all_tests.sh` to run tests for all supported PostgreSQL versions or `./run_test 11` to test just the specific version (11, in this case).

# pg_relusage

pg_relusage is a PostgreSQL extension which allows one to discover and log the relations used in SQL statements.

## Installation

Extension hooks into PostgreSQL query executor and therefore needs to be compiled using PostgreSQL headers for the appropriate server version,
and then loaded into server proces via [load](https://www.postgresql.org/docs/current/sql-load.html) statement or [shared_preload_libraries](https://postgresqlco.nf/doc/en/param/shared_preload_libraries/) configuration parameter.

Extension uses standard PGXS build infrastructure and (provided that `pg_config` is somewhere in your PATH) could be built with `make install`.

## Usage

Once extension is loaded, each SQL statement will emit one extra log message which will list all the referenced relations (by name).

Statement:

```
select * from pg_stats limit 1;
```

should produce log message along the lines of:

```
relations used: pg_statistic,pg_class,pg_attribute,pg_namespace
```

## Limitations

Not tested on PostgreSQL below v10 (though it should work).

## Development and testing

You can run `./all_tests.sh` to run tests for all supported PostgreSQL versions or `./run_test 11` to test just the specific version (11, in this case).

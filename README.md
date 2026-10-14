# pg_relusage

pg_relusage is a PostgreSQL extension which allows one to discover and log the relations used in SQL statements. Supported PostgreSQL versions: 10 to 18.

![regression tests](https://github.com/adept/pg_relusage/actions/workflows/ci.yml/badge.svg)

## Why?

This extension will be useful if you are dealing with (large?) legacy database and suspect that it contains plenty of unused objects.

This extension will allow you to quickly get an understanding of which clients use which objects / relations. Unlike statement log, which will only show you the views and tables explicitly referenced by the query, this extension will look at the result of view expansion, join elimination, partition pruning etc and report the relations that were *actually* used by the statement.

## Installation

Extension hooks into PostgreSQL query executor and therefore needs to be compiled using PostgreSQL headers for the appropriate server version,
and then loaded into server process.

Extension uses standard PGXS build infrastructure and (provided that `pg_config` is somewhere in your PATH) could be built with `make install`.

## Usage

After installation, extension has to be either loaded into a single client process via [load](https://www.postgresql.org/docs/current/sql-load.html) statement or enabled for all sessions globally via [shared_preload_libraries](https://postgresqlco.nf/doc/en/param/shared_preload_libraries/) configuration parameter.

Once extension is loaded, each SQL statement will emit one extra log message which will list all the referenced relations, as a comma-separated list of schema-qualified names. Relations in the `public` schema are reported without the schema name, to keep the logs smaller. Names are quoted the same way as in SQL when necessary (e.g. `"My Schema"."we,ird"`), and relations in temporary schemas are reported as `pg_temp.<name>`.

For example, given

```
create table owners(id int primary key, name text);
create table items(id int, owner_id int, val text);
create view vw_items as
  select i.id, i.val, o.name as owner from items i join owners o on o.id = i.owner_id;
```

statement

```
select * from vw_items where owner = 'alice';
```

should produce log message along the lines of:

```
relations used: vw_items,items,owners
```

### What counts as "used"

Tables, materialized views, foreign tables and partitions are reported when the final execution plan of the statement actually scans or modifies them. Relations that the planner eliminated are not reported, for example:

* relations removed by join elimination (e.g. a `left join` to a table via its primary key, when none of its columns are used);
* partitions removed by partition pruning, both at planning time and at executor startup (PostgreSQL 11+), but not partitions that are skipped later during execution;
* relations in a part of the query that the planner proved to be empty (e.g. `where false`, or all partitions pruned).

In addition, the following are reported:

* partitioned tables, when any of their partitions is scanned or modified via the partitioned table (they are not scanned themselves);
* indexes used by the plan (index scans, index-only scans and bitmap index scans);
* views referenced by the statement, together with the relations they expand to;
* relations referenced by name through a `regclass` constant, such as sequences used via `nextval('some_seq')` (including `serial`/`identity`-style column defaults: `insert into test ...` reports `test,test_id_seq`), `'some_table'::regclass` literals, and functions taking a `regclass` argument, such as `pg_relation_size('some_table')`. Such statements would break if the relation was dropped, so they are reported as users of it, even though its contents are never read.

System catalogs (relations in `pg_catalog`, `pg_toast` and `information_schema`) are never reported. They cannot be dropped anyway, and catalog queries issued by tools (psql, GUI clients, ORMs) would otherwise flood the log.

If the plan contains a node type that pg_relusage does not recognize (e.g. one introduced by a newer PostgreSQL version), it falls back to reporting all the relations that the plan depends on, without eliminating anything.

### Correlating log messages with clients and statements

Log messages do not include the text of the statement, to keep the log small. To find out which client or statement used the relations, include the relevant information in [log_line_prefix](https://www.postgresql.org/docs/current/runtime-config-logging.html#GUC-LOG-LINE-PREFIX), for example:

```
log_line_prefix = '%m [%p] %q%u@%d app=%a client=%h '
```

which would produce messages like

```
2025-01-01 12:00:00.000 UTC [1234] app_user@app_db app=billing client=10.0.0.1 LOG:  relations used: vw_items,items,owners
```

On PostgreSQL 14 and later, you can also add the query identifier (`%Q`, requires [compute_query_id](https://www.postgresql.org/docs/current/runtime-config-statistics.html#GUC-COMPUTE-QUERY-ID) to be enabled, which [pg_stat_statements](https://www.postgresql.org/docs/current/pgstatstatements.html) does automatically). It can be used to look up the normalized text of the statement in `pg_stat_statements`. Nested statements are logged with the query identifier of the top-level statement.

## Configuration

Extension provides the following settings (all of them can only be changed by superusers):

* pg_relusage.enabled (defaults to `on`) turns logging on and off.
* pg_relusage.nested_statements (defaults to `all`) controls how relations used by nested statements (statements executed inside functions, triggers, foreign key checks etc.) are reported:
  * `all`: every statement is reported in a separate log message, including the nested ones. For example, `insert` into a table with a trigger that writes into `audit` table would produce two messages: `relations used: test,test_id_seq` and `relations used: audit`.
  * `top`: only top-level statements are reported, nested statements are not processed at all. This has the lowest overhead, but relations used only by functions and triggers will never be reported, so they would look unused.
  * `fold`: relations used by nested statements are reported together with the top-level statement, in one log message emitted when the top-level statement finishes (`relations used: test,test_id_seq,audit` in the example above). This keeps the log volume close to `top` (e.g. a function that executes a query in a loop million times produces just one message), but has the same processing overhead as `all`. If the statement fails, relations it used before the failure are reported at the end of the transaction.

  Note that statements in `DO` blocks and procedures invoked by `CALL` are top-level statements, as `DO` and `CALL` are not executed by the query executor themselves.
* pg_relusage.log_level (defaults to `LOG`), with the obvious meaning and the same set of values that similar configuration items (like `client_min_messages` support)
* pg_relusage.rel_kinds (defaults to `'riSvmfp'`) which specifies which relation kinds will be reported. This is a list of one-letter codes used in the `relkind` field in the `pg_class` (full list could be seen [here](https://www.postgresql.org/docs/current/catalog-pg-class.html)).

## Limitations

Supported PostgreSQL versions are 10 to 18.

## Development and testing

In addition to `make install && make installcheck`, you can run `./all_tests.sh` to run tests in Docker containers (which will test all supported PostgreSQL versions) or `./run_test.sh 11` to test just the specific version (11, in this case).

Docker images with PostgreSQL preinstalled (`pg_relusage-test:<version>`, see `test/Dockerfile`) are built on the first run and reused afterwards. Use `REBUILD=1 ./run_test.sh 11` to rebuild the image and pick up new PostgreSQL minor releases.

Actual test output is saved to `results/pg_relusage_<version>.out`. If changes in the output are expected, use `ACCEPT=1 ./run_test.sh 11` (or `ACCEPT=1 ./all_tests.sh`) to update the expected output, and review the changes with `git diff expected/`.

C code follows the PostgreSQL coding style. Run `tools/pgindent.sh` to reformat it with PostgreSQL's pgindent (in the same Docker image used for tests), or `tools/pgindent.sh --check` to check it without making changes.

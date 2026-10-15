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

After installation, the library has to be loaded into the server processes, in one of the following ways:

* [shared_preload_libraries](https://www.postgresql.org/docs/current/runtime-config-client.html#GUC-SHARED-PRELOAD-LIBRARIES) in `postgresql.conf` enables it for all sessions in all databases. Changing it requires a server restart.
* [session_preload_libraries](https://www.postgresql.org/docs/current/runtime-config-client.html#GUC-SESSION-PRELOAD-LIBRARIES) loads it at the start of each new session, without a restart. It can be set for all sessions in `postgresql.conf`, or only for some databases or roles, e.g. `ALTER DATABASE app SET session_preload_libraries = 'pg_relusage'`. This requires superuser.
* [LOAD](https://www.postgresql.org/docs/current/sql-load.html) `'pg_relusage'` loads it into the current session only. This requires superuser, unless the library is installed into `$libdir/plugins/` (then use `LOAD '$libdir/plugins/pg_relusage'`).

The extension does not define any SQL objects, so `CREATE EXTENSION pg_relusage` is optional: it only records the extension in `pg_extension` (which could be useful to keep track of where it is installed), and does not load or enable anything by itself.

Once the library is loaded, each SQL statement that uses any relations will emit a log message which will list them, as a comma-separated list of schema-qualified names. Relations in the `public` schema are reported without the schema name, to keep the logs smaller. Names are quoted the same way as in SQL when necessary (e.g. `"My Schema"."we,ird"`), and relations in temporary schemas are reported as `pg_temp.<name>`.

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

Utility statements that access data without running a query are reported as well: `COPY` to or from a table, `TRUNCATE`, `LOCK` and `REFRESH MATERIALIZED VIEW` (which also reports the relations used by the query of the materialized view, in a separate message). Utility statements that run a query, such as `COPY (select ...) TO`, `CREATE TABLE AS` or `EXPLAIN ANALYZE`, report the relations used by that query. Maintenance (`VACUUM`, `ANALYZE`, `CLUSTER`, `REINDEX`) and DDL statements (`ALTER TABLE`, `CREATE INDEX`, `GRANT`, etc.) are not reported: they are typically run for all relations by maintenance scripts and migrations, which would make every relation look used.

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
  * `fold`: relations used by nested statements are reported together with the top-level statement, in one log message emitted when the top-level statement finishes (`relations used: test,test_id_seq,audit` in the example above). This keeps the log volume close to `top` (e.g. a function that executes a query in a loop million times produces just one message), but has the same processing overhead as `all`. If the statement fails, relations it used before the failure are still reported, when its transaction (or subtransaction, e.g. a PL/pgSQL block with an exception handler) is aborted.

  Note that statements in `DO` blocks and procedures invoked by `CALL` are top-level statements, as `DO` and `CALL` are not executed by the query executor themselves.
* pg_relusage.log_level (defaults to `log`) is the level of the log messages: one of `debug5`, `debug4`, `debug3`, `debug2`, `debug1` (`debug` is an alias for `debug2`), `info`, `notice`, `warning` and `log`. Whether a message is written to the server log and/or sent to the client is decided by [log_min_messages and client_min_messages](https://www.postgresql.org/docs/current/runtime-config-logging.html#RUNTIME-CONFIG-SEVERITY-LEVELS), as for any other message. With default settings, `log` messages are written to the server log only, `info` and `notice` messages are sent to the client only (`info` messages are always sent to the client), and `warning` messages go to both. If the message would not be written or sent anywhere, pg_relusage skips all the work of collecting the relations, so it has no overhead in that case.
* pg_relusage.rel_kinds (defaults to `'riSvmfp'`) specifies which relation kinds will be reported. This is a list of one-letter codes used in the `relkind` field in the `pg_class` (full list could be seen [here](https://www.postgresql.org/docs/current/catalog-pg-class.html)). The default reports tables (`r`), indexes (`i`), sequences (`S`), views (`v`), materialized views (`m`), foreign tables (`f`) and partitioned tables (`p`). For example, use `'rSvmfp'` to stop reporting indexes.

## Performance

The numbers below were measured with pgbench (single client, prepared statements) on PostgreSQL 17. Absolute numbers depend on hardware and workload, but they show what the overhead depends on:

* When pg_relusage is disabled (`pg_relusage.enabled = off`), or its messages would be discarded because of `log_min_messages` and `client_min_messages`, the overhead is too small to measure.
* When every statement is logged, a short statement (a query on a view joining three tables, taking 51 µs without pg_relusage) took 66 µs, i.e. about 15 µs longer. Most of this is the cost of writing a log line for every statement, so for workloads with many short statements, consider logging only for some databases or roles (see `session_preload_libraries` above), or for a limited period of time.
* The cost grows with the number of relations a statement uses. On a table with 2000 partitions, a statement that scans all of them took 1.7 ms longer (26.8 ms instead of 25.2 ms), most of it for formatting the 2000 relation names in the log message. A prepared statement with a generic plan, which scans a single partition after pruning at executor startup, took 0.35 ms longer (1.2 ms instead of 0.9 ms).
* With `pg_relusage.nested_statements = 'top'`, nested statements are not processed at all. With `fold`, they are processed as with `all`, but fewer log lines are written.

## Limitations

Supported PostgreSQL versions are 10 to 18.

* Statements that are executed when no other statement is running are top-level statements, even if they are not issued by the client directly. This includes deferred constraint triggers fired at `COMMIT` (e.g. deferred foreign key checks), triggers fired by `COPY FROM`, and functions evaluated by the planner. They are reported as separate messages, also when `pg_relusage.nested_statements` is `top` or `fold`.
* `INSERT` into a partitioned table reports only the partitioned table, as rows are routed to partitions during execution.
* Partitions that are skipped only during execution (by pruning based on values that are not known at executor startup, e.g. from a subquery or the outer side of a join) are reported.
* In the server log, messages from nested statements are followed by `CONTEXT` lines, which show the function and the statement within it that used the relations. This adds to the log volume; set [log_error_verbosity](https://www.postgresql.org/docs/current/runtime-config-logging.html#GUC-LOG-ERROR-VERBOSITY) to `terse` to omit them (this affects all messages).

## Development and testing

In addition to `make install && make installcheck`, you can run `./all_tests.sh` to run tests in Docker containers (which will test all supported PostgreSQL versions) or `./run_test.sh 11` to test just the specific version (11, in this case).

Docker images with PostgreSQL preinstalled (`pg_relusage-test:<version>`, see `test/Dockerfile`) are built on the first run and reused afterwards. Use `REBUILD=1 ./run_test.sh 11` to rebuild the image and pick up new PostgreSQL minor releases.

Expected output is the same for all PostgreSQL versions (tests that depend on features missing in older versions are skipped there, see `Makefile`). Actual test output is saved to `results/<version>/`. If changes in the output are expected, use `ACCEPT=1 ./run_test.sh 11` to update the expected output, and review the changes with `git diff expected/`.

C code follows the PostgreSQL coding style. Run `tools/pgindent.sh` to reformat it with PostgreSQL's pgindent (in the same Docker image used for tests), or `tools/pgindent.sh --check` to check it without making changes.

## Releasing

Releases are published on [PGXN](https://pgxn.org/dist/pg_relusage/) by the `Release` GitHub workflow, which runs all the tests and then uploads the distribution (using `PGXN_USERNAME` and `PGXN_PASSWORD` repository secrets). To make a release:

1. Update the version in `META.json` (in both places) and `default_version` in `pg_relusage.control`.
2. Add `pg_relusage--<new version>.sql` and an upgrade script `pg_relusage--<old version>--<new version>.sql` (both are empty, as the extension does not define any SQL objects).
3. Commit, tag the commit with `v<new version>` (e.g. `v1.0.1`) and push the tag.

The workflow checks that the tag matches the version in `META.json` and `pg_relusage.control`.

# AGENTS.md

Notes for coding agents (and humans) working on pg_relusage. See `README.md` for what the extension does from a user's point of view.

## What it is

A PostgreSQL extension in a single C file, `pg_relusage.c`. It logs `relations used: a,b,c` for every statement that uses relations. Its purpose is to find unused database objects, so a relation that a statement depends on must never be missed. When in doubt, report rather than omit.

It supports PostgreSQL 10 to 18. That range is stated in `README.md`, `META.json` (`>= 10.0.0`), the CI matrix, `all_tests.sh` and an `#error` guard in `pg_relusage.c`, so keep them in sync.

## Layout

- `pg_relusage.c`: all the code.
- `pg_relusage.control`, `pg_relusage--*.sql`: extension packaging. The extension defines no SQL objects, so the scripts are empty and `CREATE EXTENSION` is optional.
- `sql/`, `expected/`: pg_regress tests. `runtime_pruning` is skipped on PG 10 (see `Makefile`).
- `run_test.sh`, `all_tests.sh`, `test/Dockerfile`: run the tests in Docker.
- `tools/pgindent.sh`: run PostgreSQL's pgindent in Docker.
- `.github/workflows/ci.yml`: tests on PG 10–18 plus a pgindent check, on every push and PR. `release.yml`: publishes to PGXN on `vX.Y.Z` tags.

## Build and test

Nothing needs to be installed locally except Docker.

```
./run_test.sh 18            # one version; output in results/18/
./all_tests.sh              # all supported versions
ACCEPT=1 ./run_test.sh 18   # on failure, copy actual output over expected/
tools/pgindent.sh           # reformat; --check / --diff to only check
```

- The first run per version builds a cached image `pg_relusage-test:<ver>`; use `REBUILD=1` to refresh it.
- Tests compile with `-Werror` (`pg-build-test` sets `PROFILE=-Werror`), so any compiler warning fails the build.
- There is **one expected output for all versions**. Never add per-version expected files or pg_regress alternative files: a broken feature on a newer version could then match an older version's output and pass. If output differs between versions, make the test version-independent instead:
  - hide output that depends on the version (`\o /dev/null`);
  - catch version-specific error messages in a `DO` block and print a fixed notice;
  - order queries so that the relation order is stable;
  - move tests of features that are missing in old versions to a separate test file, excluded in the `Makefile` by `MAJORVERSION`.
- After `ACCEPT=1`, always review `git diff expected/`: the expected output must only change in the intended places.
- Don't make test output depend on the order in which messages reach the client after an ERROR. psql prints notices that arrive together with an error inconsistently, which made a test flaky. Catch the error in a `DO` block instead.
- Before relying on a new test, check that it actually fails without the fix. Twice in this project a test first passed both with and without the bug.

## Code conventions

- PostgreSQL style, enforced by `tools/pgindent.sh` and CI. Use `/* */` comments only: pgindent mangles `//`. New typedefs in `.c` files are picked up automatically by the script.
- Version differences go into `#if PG_VERSION_NUM >= ...` blocks. Check the headers of every supported version, which are available in the test images (`/usr/include/postgresql/<ver>/server`). Differences hit so far:
  - `ExecutorRun` lost `execute_once` in PG 18;
  - `ProcessUtility` has three signatures (10–12, 13, 14+);
  - `ForeignScan.fs_base_relids` exists from PG 16;
  - `RelationGetPartitionDesc` is a macro in `utils/rel.h` up to 12, a function in 13, and takes `omit_detached` from 14;
  - `relation_open` is in `access/heapam.h` before 12 and `access/relation.h` from 12;
  - `get_partition_ancestors` is not available in PG 10;
  - `IsSystemNamespace` was renamed (compare with `PG_CATALOG_NAMESPACE` instead);
  - `GETSTRUCT` needs `access/htup_details.h` on PG 10/11;
  - `MarkGUCPrefixReserved` exists from PG 15 (before that, `EmitWarningsOnPlaceholders`);
  - `message_level_is_interesting` exists from PG 14 (a simplified copy is used before that).
- Statements can use thousands of relations (partitioned tables). Never use linear list searches (`list_member_oid`, `list_append_unique_oid`) on lists that grow with the number of relations; use the sorted OID sets (`pg_relusage_oidset`, `pg_relusage_unique_oids`). Avoid per-relation catalog scans.
- Keep the early exit in `pg_relusage_should_report()` cheap and first. When the extension is disabled or the message would be discarded, the overhead must stay unmeasurable.

## How it works (key design decisions)

- **What is reported.** Relations come from the plan's dependency list (`plannedstmt->relationOids`). Range-table relations that the initialized plan state tree does not scan or modify are dropped: join removal, partition pruning at plan time and at executor startup, and empty query parts.
  - Views and `regclass` references are always kept: they are not in the range table as scanned relations.
  - Indexes come from index scan nodes.
  - Partitioned tables are kept when any of their partitions is scanned (checked top-down via cached partition descriptors).
  - An unknown plan node type makes the walker give up on elimination and report the whole dependency list. That's the safety net for new PostgreSQL versions: add new node types to the switch in `pg_relusage_plan_walker`.
- **When it is logged.** After `ExecutorStart` succeeds, so statements that fail permission checks are not reported. Never for `EXPLAIN` without `ANALYZE`, and never in parallel workers (the leader reports the whole plan).
- **Utility statements.** Only those that access data without the executor are reported: `COPY` to or from a table, `TRUNCATE`, `LOCK` and `REFRESH MATERIALIZED VIEW`. Maintenance and DDL commands are deliberately excluded, because scripts and migrations touch every table. Utility commands do not change the nesting level, so statements in `DO` and `CALL` are top-level.
- **Names.** Schema-qualified and quoted with `quote_qualified_identifier`, except for `public`. Temporary schemas are shown as `pg_temp`. Relations in `pg_catalog`, `pg_toast` and `information_schema` are never reported.
- **Order.** Relations are logged in dependency-list order, not sorted (a deliberate decision: it is deterministic for a given plan, and sorting costs time).
- **Nesting** (`pg_relusage.nested_statements`).
  - The nesting level is tracked in the `ExecutorRun`/`ExecutorFinish` hooks; AFTER triggers and FK checks run in `ExecutorFinish`.
  - In `fold` mode, a top-level statement gets a pending entry that is logged at `ExecutorEnd`. Names are formatted when relations are added, because the entry may be logged from a transaction callback, where catalog access is not allowed.
  - Entries remember their subtransaction: they are flushed when it aborts and handed to the parent when it commits. Without this, a failed statement's freed QueryDesc address can be reused, and relations get attributed to the wrong statement.
- The statement text is not logged (`errhidestmt`). The README recommends a `log_line_prefix`, including `%Q` (query ID, PG 14+) for joining with `pg_stat_statements`.

## Gotchas

- **Makefile:** custom targets (`release-zip`) must come after `include $(PGXS)`. Otherwise they become the default target and a plain `make` stops building the extension.
- **Release check in the workflow:** shell steps run with `sh -e`. A failing command at the start of an `a && b` chain does not stop the script, so use explicit `if ...; then exit 1; fi`.
- **META.json:** contains a third `"version": "1.0.0"`, the META spec version. Don't change it when bumping the release version.
- **`.git-blame-ignore-revs`:** lists the pgindent commit by hash. Don't rebase published history; merge instead.
- **Release zip contents:** `.gitattributes` keeps development-only files out of the zip (`git archive` reads it from the archived commit).
- **Profiling in Docker:**
  - Use timers around phases first, in a throwaway build.
  - `perf` from the host needs `perf_event_paranoid <= 1`, `yama.ptrace_scope = 0`, and the backend running with the same uid and gid as the host user (`docker exec -u 1000:1000`).
  - Big scans may be dominated by JIT; benchmark with `jit=off` to see the extension's share.
  - Disable parallel query (`max_parallel_workers_per_gather=0`) to get stable numbers.

## Making changes

1. Change the code, then add or adjust tests in `sql/pg_relusage.sql`.
2. Run `./all_tests.sh` and `tools/pgindent.sh --check`.
3. Update `README.md` when behaviour, settings or limitations change. The README documents user-visible semantics in detail ("What counts as used", "Limitations", "Performance"), and they must stay accurate. Verify claims by experiment before documenting them.

## Releasing

Follow "Releasing" in `README.md`:
1. Bump the version in `META.json` (2 places) and `pg_relusage.control`.
2. Rename the base script to `pg_relusage--<new>.sql` and add an empty `pg_relusage--<old>--<new>.sql`.
3. Accept the new version in the extension test's expected output.
4. Push and wait for CI.
5. Tag `v<new>` and push the tag. The workflow re-runs CI, checks the versions, and publishes with the `PGXN_USERNAME`/`PGXN_PASSWORD` secrets.

PGXN does not replace an already published version: always release a new version number.

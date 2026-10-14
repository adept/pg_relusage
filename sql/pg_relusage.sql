load 'pg_relusage';
set client_min_messages to LOG;
---------
-- TABLES
---------
create table test(id serial, t text);

-- we detect 'test' in a simple query
\copy (select * from test) to '/dev/null';

-- multiple references reported only once
\copy (select * from test t1 join test t2 using (id)) to '/dev/null';

-- system catalogs are not reported
\copy (select * from pg_class c1 join pg_class c2 using (relname) limit 1) to '/dev/null';
\copy (select * from information_schema.tables limit 1) to '/dev/null';
\copy (select * from test, pg_namespace) to '/dev/null';

-- insert, update, delete statements all covered
insert into test(t) values ('test');
update test set t = t||t;
delete from test;

--------
-- VIEWS
--------
create view vw_test as select * from test;

-- we detect 'test' as the only relation used here
\copy (select * from vw_test t1 join test t2 using (id)) to '/dev/null';

-- more complex view
create table owners(id int primary key, name text);
create table items(id int, owner_id int, val text);
create view vw_items as
  select i.id, i.val, o.name as owner from items i join owners o on o.id = i.owner_id;
\copy (select * from vw_items limit 1) to '/dev/null';

-------
-- CTEs
-------
\copy (with x as (select * from vw_items limit 1) select * from x) to '/dev/null';

------------------------
-- TEMP TABLES AND VIEWS
------------------------
create temp table temp_test(t text);
create temp view vw_temp_test as select * from temp_test;
\copy (select * from vw_temp_test) to '/dev/null';

----------------
-- SQL functions
----------------
create function test_sql_f(i int) returns text
language sql
as $_$
select t from test where id = i
$_$;

select * from test_sql_f(42);

-----------
-- PL/pgSQL
-----------
create function test_pgsql_f(p_owner text) returns text
language plpgsql
as $_$
declare
  res text;
begin
select val into res from vw_items where owner = p_owner;
return res;
end;
$_$;

select * from test_pgsql_f('foobar');

-- dynamic queries
create function test_pgsql_dyn_f(p_owner text) returns text
language plpgsql
as $_$
declare
  res text;
begin
execute $$ select val from vw_items where owner = $$ || quote_literal(p_owner) into res;
return res;
end;
$_$;

select * from test_pgsql_dyn_f('foobar');

----------
-- EXPLAIN
----------
-- plain EXPLAIN does not execute the query, so nothing is reported
explain (costs off) select * from test;

-- EXPLAIN ANALYZE executes the query, so relations are reported
explain (analyze, costs off, timing off, summary off, buffers off) select * from test;

--------------
-- PERMISSIONS
--------------
-- statements that fail the permission check are not reported
drop role if exists regress_relusage_user;
create role regress_relusage_user;
set role regress_relusage_user;
select * from test;
reset role;
drop role regress_relusage_user;

-----------------------
-- REGCLASS REFERENCES
-----------------------
-- relations referenced via regclass constants are reported, as the statement depends on them
select 'test'::regclass;

---------------------------------------
-- ONLY RELATIONS USED BY THE FINAL PLAN
---------------------------------------
create table small(id int primary key, x int);

-- left join to small is removed by the planner, so small is not reported
\copy (select t.id from test t left join small s on s.id = t.id) to '/dev/null';

-- indexes used by the plan are reported too
set enable_seqscan = off;
\copy (select * from small where id = 1) to '/dev/null';
reset enable_seqscan;

-- relation is not scanned at all
\copy (select * from test where false) to '/dev/null';

-- partitioned tables are reported along with partitions that are scanned
create table parted(k int) partition by list (k);
create table parted_1 partition of parted for values in (1);
create table parted_2 partition of parted for values in (2);
\copy (select * from parted) to '/dev/null';

-- partitions pruned by the planner are not reported
\copy (select * from parted where k = 1) to '/dev/null';

-- partitions pruned at executor startup are not reported
set test.k = '2';
\copy (select * from parted where k = current_setting('test.k')::int) to '/dev/null';

-- all partitions are pruned, so nothing is scanned
\copy (select * from parted where k = 3) to '/dev/null';

-- target of insert is reported
insert into parted values (1);

-- parent is not reported when the partition is queried directly
\copy (select * from parted_1) to '/dev/null';

---------------
-- NAME QUOTING
---------------
-- names are schema-qualified (except for public) and quoted where necessary
create schema "Other Schema";
create table "Other Schema".small(a int);
create table "we,ird"(a int);
\copy (select * from "Other Schema".small, public.small, "we,ird") to '/dev/null';

--------------------
-- NESTED STATEMENTS
--------------------
create function count_items() returns bigint
language plpgsql
as $_$
begin
  return (select count(*) from items);
end;
$_$;

create table audit(t text);
create function audit_trg() returns trigger
language plpgsql
as $_$
begin
  insert into audit values (new.t);
  return new;
end;
$_$;
create trigger test_audit after insert on test for each row execute procedure audit_trg();

create function fail_after_read() returns bigint
language plpgsql
as $_$
begin
  perform count(*) from items;
  raise exception 'failed after reading items';
end;
$_$;

-- all: every statement is reported separately (default)
\copy (select count_items()) to '/dev/null';
insert into test(t) values ('nested');

-- top: only top-level statements are reported
set pg_relusage.nested_statements = 'top';
\copy (select count_items()) to '/dev/null';
insert into test(t) values ('nested');
-- statements in DO blocks are top-level, as DO is not executed by the executor
do $_$ begin perform count(*) from items; end $_$;

-- fold: relations used by nested statements are reported with the top-level statement
set pg_relusage.nested_statements = 'fold';
\copy (select count_items()) to '/dev/null';
insert into test(t) values ('nested');
\copy (select count_items() from vw_items) to '/dev/null';

-- fold: statement that failed still reports relations used before the failure, at
-- the end of transaction (error is caught, so that the order of the output is stable)
do $_$ begin perform fail_after_read(); exception when others then null; end $_$;

reset pg_relusage.nested_statements;
drop trigger test_audit on test;

-----------
-- ENABLED
-----------
set pg_relusage.enabled = off;
\copy (select * from test) to '/dev/null';
reset pg_relusage.enabled;
\copy (select * from test) to '/dev/null';

-----------
-- PARALLEL
-----------
-- parallel workers do not report relations, the leader reports them for the whole plan
create table big as select g id from generate_series(1, 10000) g;
create index big_id on big(id);
analyze big;
set parallel_setup_cost = 0;
set parallel_tuple_cost = 0;
set min_parallel_table_scan_size = 0;
set min_parallel_index_scan_size = 0;
set enable_seqscan = off;
set enable_bitmapscan = off;
set max_parallel_workers_per_gather = 2;
explain (costs off) select count(*) from big where id < 5000;
select count(*) from big where id < 5000;
reset parallel_setup_cost;
reset parallel_tuple_cost;
reset min_parallel_table_scan_size;
reset min_parallel_index_scan_size;
reset enable_seqscan;
reset enable_bitmapscan;
reset max_parallel_workers_per_gather;

---------------------
-- UTILITY STATEMENTS
---------------------
-- utility statements that access data are reported
\copy test to '/dev/null'
\copy small from '/dev/null'
truncate audit, small;
begin;
lock table test in share mode;
commit;

-- materialized view is reported by REFRESH, in addition to relations used by its query
create materialized view mv_items as select * from vw_items;
refresh materialized view mv_items;

-- maintenance and DDL statements are not reported
vacuum test;
analyze test;
create index test_t on test(t);

-- statements that fail are not reported
truncate test, small_nonexistent;

-- nested utility statements follow pg_relusage.nested_statements
create function truncate_audit() returns void
language plpgsql
as $_$
begin
  truncate audit;
end;
$_$;
set pg_relusage.nested_statements = 'top';
select truncate_audit();
set pg_relusage.nested_statements = 'fold';
select truncate_audit();
reset pg_relusage.nested_statements;
select truncate_audit();

------------
-- EXTENSION
------------
-- CREATE EXTENSION is optional, as the extension does not define any objects
create extension pg_relusage;
select extversion from pg_extension where extname = 'pg_relusage';
drop extension pg_relusage;

-- upgrade from 0.0.1
create extension pg_relusage version '0.0.1';
alter extension pg_relusage update;
select extversion from pg_extension where extname = 'pg_relusage';
drop extension pg_relusage;

------
-- GUC
------
-- unknown relation kinds are rejected
set pg_relusage.rel_kinds = 'rx';

set pg_relusage.log_level = 'INFO';
set pg_relusage.rel_kinds = 'r';
\copy (select * from vw_items limit 1) to '/dev/null';

set pg_relusage.log_level = 'NOTICE';
set pg_relusage.rel_kinds = 'v';
\copy (select * from vw_items limit 1) to '/dev/null';

-- nothing is reported when the message would be discarded
set pg_relusage.log_level = 'DEBUG1';
set pg_relusage.rel_kinds = 'riSvmfp';
\copy (select * from vw_items limit 1) to '/dev/null';

-- nothing is reported with empty rel_kinds
set pg_relusage.log_level = 'NOTICE';
set pg_relusage.rel_kinds = '';
\copy (select * from vw_items limit 1) to '/dev/null';

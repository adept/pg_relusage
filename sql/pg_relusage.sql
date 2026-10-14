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

-- this works with built-in tables as well
\copy (select * from pg_class c1 join pg_class c2 using (relname) limit 1) to '/dev/null';

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

-- more complex built-in view
\copy (select * from pg_stats limit 1) to '/dev/null';

-------
-- CTEs
-------
\copy (with stats as (select * from pg_stats limit 1) select * from stats) to '/dev/null';

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
create function test_pgsql_f(t text) returns text
language plpgsql
as $_$
declare
  res text;
begin
select t into res from pg_stats where tablename = t;
return res;
end;
$_$;

select * from test_pgsql_f('foobar');

-- dynamic queries
create function test_pgsql_dyn_f(t text) returns text
language plpgsql
as $_$
declare
  res text;
begin
execute $$ select schemaname from pg_stats where tablename = $$ || quote_literal(t) into res;
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

------
-- GUC
------
set pg_relusage.log_level = 'INFO';
set pg_relusage.rel_kinds = 'r';
\copy (select * from pg_stats limit 1) to '/dev/null';

set pg_relusage.log_level = 'NOTICE';
set pg_relusage.rel_kinds = 'v';
\copy (select * from pg_stats limit 1) to '/dev/null';


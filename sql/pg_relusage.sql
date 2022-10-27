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

------
-- GUC
------
set pg_relusage.log_level = 'INFO';
set pg_relusage.rel_kinds = 'r';
\copy (select * from pg_stats limit 1) to '/dev/null';

set pg_relusage.log_level = 'NOTICE';
set pg_relusage.rel_kinds = 'v';
\copy (select * from pg_stats limit 1) to '/dev/null';


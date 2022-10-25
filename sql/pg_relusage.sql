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

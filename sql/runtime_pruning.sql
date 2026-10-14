-- Partition pruning at executor startup, not supported by PostgreSQL 10
load 'pg_relusage';
set client_min_messages to LOG;

create table rp_parted(k int) partition by list (k);
create table rp_parted_1 partition of rp_parted for values in (1);
create table rp_parted_2 partition of rp_parted for values in (2);

-- partitions pruned at executor startup are not reported
set test.k = '2';
\copy (select * from rp_parted where k = current_setting('test.k')::int) to '/dev/null';

from new_test_framework.utils import tdLog, tdSql, sc, clusterComCheck, tdCom


class TestExplain:

    def setup_class(cls):
        tdLog.debug(f"start to execute {__file__}")

    def test_explain(self):
        """explain

        1. 

        Catalog:
            - Query:Explain

        Since: v3.0.0.0

        Labels: common,ci

        Jira: None

        History:
            - 2025-4-28 Simon Guan Migrated from tsim/query/explain.sim

        """

        tdLog.info(f'======== step1')
        tdSql.execute(f"create database db1 vgroups 3 cachesize 10 cachemodel 'both';")
        tdSql.execute(f"use db1;")
        tdSql.query(f"select * from information_schema.ins_databases;")
        tdSql.execute(f"create stable st1 (ts timestamp, f1 int, f2 binary(200)) tags(t1 int);")
        tdSql.execute(f"create stable st2 (ts timestamp, f1 int, f2 binary(200)) tags(t1 int);")
        tdSql.execute(f"create table tb1 using st1 tags(1);")
        tdSql.execute(f'insert into tb1 values (now, 1, "Hash Join  (cost=230.47..713.98 rows=101 width=488) (actual time=0.711..7.427 rows=100 loops=1)");')

        tdSql.execute(f"create table tb2 using st1 tags(2);")
        tdSql.execute(f'insert into tb2 values (now, 2, "Seq Scan on tenk2 t2  (cost=0.00..445.00 rows=10000 width=244) (actual time=0.007..2.583 rows=10000 loops=1)");')
        tdSql.execute(f"create table tb3 using st1 tags(3);")
        tdSql.execute(f'insert into tb3 values (now, 3, "Hash  (cost=229.20..229.20 rows=101 width=244) (actual time=0.659..0.659 rows=100 loops=1)");')
        tdSql.execute(f"create table tb4 using st1 tags(4);")
        tdSql.execute(f'insert into tb4 values (now, 4, "Bitmap Heap Scan on tenk1 t1  (cost=5.07..229.20 rows=101 width=244) (actual time=0.080..0.526 rows=100 loops=1)");')

#sql create table tb1 using st2 tags(1);
#sql insert into tb1 values (now, 1, "Hash Join  (cost=230.47..713.98 rows=101 width=488) (actual time=0.711..7.427 rows=100 loops=1)");

#sql create table tb2 using st2 tags(2);
#sql insert into tb2 values (now, 2, "Seq Scan on tenk2 t2  (cost=0.00..445.00 rows=10000 width=244) (actual time=0.007..2.583 rows=10000 loops=1)");
#sql create table tb3 using st2 tags(3);
#sql insert into tb3 values (now, 3, "Hash  (cost=229.20..229.20 rows=101 width=244) (actual time=0.659..0.659 rows=100 loops=1)");
#sql create table tb4 using st2 tags(4);
#sql insert into tb4 values (now, 4, "Bitmap Heap Scan on tenk1 t1  (cost=5.07..229.20 rows=101 width=244) (actual time=0.080..0.526 rows=100 loops=1)");

# for explain insert into select
        tdSql.execute(f"create table t1 (ts timestamp, f1 int, f2 binary(200), t1 int);")

        tdLog.info(f'======== step2')
        tdSql.query(f"explain select * from st1 where -2;")
        tdSql.query(f"explain insert into t1 select * from st1 where -2;")
        tdSql.query(f"explain select ts from tb1;")
        tdSql.query(f"explain insert into t1(ts) select ts from tb1;")
        tdSql.query(f"explain select * from st1;")
        tdSql.query(f"explain insert into t1 select * from st1;")
        tdSql.query(f"explain select * from st1 order by ts;")
        tdSql.query(f"explain insert into t1 select * from st1 order by ts;")
        tdSql.query(f"explain select * from information_schema.ins_stables;")
        tdSql.query(f"explain select count(*),sum(f1) from tb1;")
        tdSql.query(f"explain select count(*),sum(f1) from st1;")
        tdSql.query(f"explain select count(*),sum(f1) from st1 group by f1;")
#sql explain select count(f1) from tb1 interval(10s, 2s) sliding(3s) fill(prev);
        tdSql.query(f"explain insert into t1(ts, t1) select _wstart, count(*) from st1 interval(10s);")

        tdLog.info(f'======== step3')
        tdSql.query(f"explain verbose true select * from st1 where -2;")
        tdSql.query(f"explain verbose true insert into t1 select * from st1 where -2;")
        tdSql.query(f"explain verbose true select ts from tb1 where f1 > 0;")
        tdSql.query(f"explain verbose true insert into t1(ts) select ts from tb1 where f1 > 0;")
        tdSql.query(f"explain verbose true select * from st1 where f1 > 0 and ts > '2020-10-31 00:00:00' and ts < '2021-10-31 00:00:00';")
        tdSql.query(f"explain verbose true insert into t1 select * from st1 where f1 > 0 and ts > '2020-10-31 00:00:00' and ts < '2021-10-31 00:00:00';")
        tdSql.query(f"explain verbose true select count(*) from st1 partition by tbname slimit 1 soffset 2 limit 2 offset 1;")
        tdSql.query(f"explain verbose true select * from information_schema.ins_stables where db_name='db2';")
        tdSql.query(f"explain verbose true select st1.f1 from st1 join st2 on st1.ts=st2.ts and st1.f1 > 0;")
        tdSql.query(f"explain verbose true insert into t1(ts) select st1.f1 from st1 join st2 on st1.ts=st2.ts and st1.f1 > 0;")
        tdSql.query(f"explain verbose true insert into t1(ts, t1) select _wstart, count(*) from st1 interval(10s);")
        tdSql.query(f"explain verbose true select distinct tbname, table_name from information_schema.ins_tables;")
        tdSql.query(f"explain verbose true select diff(f1) as f11 from tb1 order by f11;")
        tdSql.query(f"explain verbose true select count(*) from st1 where ts > now - 3m and ts < now interval(10s) fill(linear);")
        tdSql.query(f"explain verbose true select count(*) from st1 partition by tbname;")
        tdSql.query(f"explain verbose true select count(*) from information_schema.ins_tables group by stable_name;")
        tdSql.query(f"explain verbose true select last(*) from st1;")
        tdSql.query(f"explain verbose true select last_row(*) from st1;")
        tdSql.query(f"explain verbose true select interp(f1) from tb1 where ts > now - 3m and ts < now range(now-3m,now) every(1m) fill(prev);")
        tdSql.query(f"explain verbose true select _wstart, _wend, count(*) from tb1 EVENT_WINDOW start with f1 > 0 end with f1 < 10;")

        tdLog.info(f'======== step4')
        tdSql.query(f"explain analyze select ts from st1 where -2;")
        tdSql.query(f"explain analyze insert into t1(ts) select ts from st1 where -2;")
        tdSql.query(f"explain analyze select ts from tb1;")
        tdSql.query(f"explain analyze insert into t1(ts) select ts from tb1;")
        tdSql.query(f"explain analyze select ts from st1;")
        tdSql.query(f"explain analyze insert into t1(ts) select ts from st1;")
        tdSql.query(f"explain analyze select ts from st1 order by ts;")
        tdSql.query(f"explain analyze insert into t1(ts) select ts from st1 order by ts;")
        tdSql.query(f"explain analyze select * from information_schema.ins_stables;")
        tdSql.query(f"explain analyze select count(*),sum(f1) from tb1;")
        tdSql.query(f"explain analyze select count(*),sum(f1) from st1;")
        tdSql.query(f"explain analyze select count(*),sum(f1) from st1 group by f1;")
        tdSql.query(f"explain analyze insert into t1(ts, t1) select _wstart, count(*) from st1 interval(10s);")

        tdLog.info(f'======== step5')
        tdSql.query(f"explain analyze verbose true select ts from st1 where -2;")
        tdSql.query(f"explain analyze verbose true insert into t1(ts) select ts from st1 where -2;")
        tdSql.query(f"explain analyze verbose true select ts from tb1;")
        tdSql.query(f"explain analyze verbose true insert into t1(ts) select ts from tb1;")
        tdSql.query(f"explain analyze verbose true select ts from st1;")
        tdSql.query(f"explain analyze verbose true insert into t1(ts) select ts from st1;")
        tdSql.query(f"explain analyze verbose true select ts from st1 order by ts;")
        tdSql.query(f"explain analyze verbose true insert into t1(ts) select ts from st1 order by ts;")
        tdSql.query(f"explain analyze verbose true select * from information_schema.ins_stables;")
        tdSql.query(f"explain analyze verbose true select count(*),sum(f1) from tb1;")
        tdSql.query(f"explain analyze verbose true select count(*),sum(f1) from st1;")
        tdSql.query(f"explain analyze verbose true select count(*),sum(f1) from st1 group by f1;")
#sql explain analyze verbose true select count(f1) from tb1 interval(10s, 2s) sliding(3s) fill(prev);
        tdSql.query(f"explain analyze verbose true select ts from tb1 where f1 > 0;")
        tdSql.query(f"explain analyze verbose true select f1 from st1 where f1 > 0 and ts > '2020-10-31 00:00:00' and ts < '2021-10-31 00:00:00';")
        tdSql.query(f"explain analyze verbose true select * from information_schema.ins_stables where db_name='db2';")
        tdSql.query(f"explain analyze verbose true select * from (select min(f1),count(*) a from st1 where f1 > 0) where a < 0;")
        tdSql.query(f"explain analyze verbose true select count(f1) from st1 group by tbname;")
        tdSql.query(f"explain analyze verbose true select st1.f1 from st1 join st2 on st1.ts=st2.ts and st1.f1 > 0;")
        tdSql.query(f"explain analyze verbose true select diff(f1) as f11 from tb1 order by f11;")
        tdSql.query(f"explain analyze verbose true select count(*) from st1 where ts > now - 3m and ts < now interval(10s) fill(linear);")
        tdSql.query(f"explain analyze verbose true select count(*) from information_schema.ins_tables group by stable_name;")
        tdSql.query(f"explain analyze verbose true select last(*) from st1;")
        tdSql.query(f"explain analyze verbose true select last_row(*) from st1;")
        tdSql.query(f"explain analyze verbose true select interp(f1) from tb1 where ts > now - 3m and ts < now range(now-3m,now) every(1m) fill(prev);")
        tdSql.query(f"explain analyze verbose true select _wstart, _wend, count(*) from tb1 EVENT_WINDOW start with f1 > 0 end with f1 < 10;")

#not pass case
#sql explain verbose true select count(*),sum(f1) as aa from tb1 where (f1 > 0 or f1 < -1) and ts > '2020-10-31 00:00:00' and ts < '2021-10-31 00:00:00' order by aa;
#sql explain verbose true select * from st1 where (f1 > 0 or f1 < -1) and ts > '2020-10-31 00:00:00' and ts < '2021-10-31 00:00:00' order by ts;
#sql explain verbose true select count(*),sum(f1) from st1 where (f1 > 0 or f1 < -1) and ts > '2020-10-31 00:00:00' and ts < '2021-10-31 00:00:00' order by ts;
#sql explain verbose true select count(f1) from tb1 where (f1 > 0 or f1 < -1) and ts > '2020-10-31 00:00:00' and ts < '2021-10-31 00:00:00' interval(10s, 2s) sliding(3s) order by ts;
#sql explain verbose true select min(f1) from st1 where (f1 > 0 or f1 < -1) and ts > '2020-10-31 00:00:00' and ts < '2021-10-31 00:00:00' interval(1m, 2a) sliding(30s)  fill(linear) order by ts;
#sql explain select max(f1) from tb1 SESSION(ts, 1s);
#sql explain select max(f1) from st1 SESSION(ts, 1s);
#sql explain select * from tb1, tb2 where tb1.ts=tb2.ts;
#sql explain select * from st1, st2 where tb1.ts=tb2.ts;
#sql explain analyze verbose true select sum(a+ str(b)) from (select _rowts, min(f1) b,count(*) a from st1 where f1 > 0 interval(1a)) where a < 0 interval(1s);
#sql explain select min(f1) from st1 interval(1m, 2a) sliding(30s);
#sql explain verbose true select count(*),sum(f1) from st1 where f1 > 0 and ts > '2021-10-31 00:00:00' group by f1 having sum(f1) > 0;
#sql explain analyze select min(f1) from st1 interval(3m, 2a) sliding(1m);
#sql explain analyze select count(f1) from tb1 interval(10s, 2s) sliding(3s) fill(prev);
#sql explain analyze verbose true select count(*),sum(f1) from st1 where f1 > 0 and ts > '2021-10-31 00:00:00' group by f1 having sum(f1) > 0;
#sql explain analyze verbose true select min(f1) from st1 interval(3m, 2a) sliding(1m);

#system sh/exec.sh -n dnode1 -s stop -x SIGINT

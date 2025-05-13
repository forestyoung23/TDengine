import time
from new_test_framework.utils import tdLog, tdSql, sc, clusterComCheck


class TestStreamSubqueryBasic:

    def setup_class(cls):
        tdLog.debug(f"start to execute {__file__}")

    def test_stream_subquery_basic(self):
        """As SubQuery basic test

        1. -

        Catalog:
            - Streams:SubQuery

        Since: v3.0.0.0

        Labels: common,ci

        Jira: None

        History:
            - 2025-5-13 Simon Guan Create Case

        """

        self.init_variables()
        self.create_snode()
        self.create_db()
        self.create_stb()
        self.create_stream()
        self.write_data()
        self.wait_stream_run_finish()
        self.check_result()

    def init_variables(self):
        tdLog.info("init variables")

        self.child_tb_num = 10
        self.batch_per_tb = 3
        self.rows_per_batch = 40
        self.rows_per_tb = self.batch_per_tb * self.rows_per_batch
        self.ts_start = 1702425600000
        self.ts_interval = 30 * 1000

    def create_snode(self):
        tdLog.info("create snode")

        tdSql.execute(f"create snode on dnode 1")
        tdSql.query(f"show snodes")
        tdSql.checkRows(1)
        tdSql.checkData(0, 0, 1)

    def create_db(self):
        tdLog.info(f"create database")

        tdSql.prepare("qdb", vgroups=1)
        tdSql.prepare("rdb", vgroups=1)
        tdSql.prepare("qdb2", vgroups=1)
        clusterComCheck.checkDbReady("qdb")
        clusterComCheck.checkDbReady("rdb")
        clusterComCheck.checkDbReady("qdb2")

    def create_stb(self):
        tdLog.info(f"create super table")

        tdSql.execute(
            f"create stable qdb.meters (ts timestamp, current float, voltage int, phase float) TAGS (location varchar(64), group_id int)"
        )

        for table in range(self.child_tb_num):
            if table % 2 == 1:
                group_id = 1
                location = "California.SanFrancisco"
            else:
                group_id = 2
                location = "California.LosAngeles"

            tdSql.execute(
                f"create table qdb.d{table} using qdb.meters tags('{location}', {group_id})"
            )

        tdSql.query("show qdb.tables")
        tdSql.checkRows(self.child_tb_num)

    def write_data(self):
        tdLog.info(f"write data")

        for batch in range(self.batch_per_tb):
            for table in range(self.child_tb_num):
                insert_sql = f"insert into qdb.d{table} values"
                for row in range(self.rows_per_batch):
                    rows = row + batch * self.rows_per_batch
                    ts = self.ts_start + self.ts_interval * rows
                    insert_sql += f"({ts}, {batch}, {row}, {rows})"
                tdSql.execute(insert_sql)

        tdSql.query(f"select count(*) from qdb.meters")
        tdSql.checkData(0, 0, self.rows_per_tb * self.child_tb_num)
        time.sleep(1000)

    def create_stream(self):
        tdLog.info(f"create stream")

    def wait_stream_run_finish(self):
        tdLog.info(f"check_result")

    def check_result(self):
        tdLog.info(f"check_result")

class TestStreamSubqueryBaiscItem():
    def __init__(self, index, trigger, subquery, checkquery): 
        self.index = index
        self.trigger = trigger
        self.subquery = subquery
        self.checkquery = checkquery
        
    def create_stream(self):
        sql = f"create stream s{self.index} {self.trigger} from qdb.meters into rdb.rs{self.index} as {self.subquery}"
        tdLog.info(sql)
        
    def wait_stream_run_finish(self):
        sql = "select * from information_schema.ins_stream_tasks where status <> 'ready';"
        # tdSql.query("select * from information_schema.ins_stream_tasks where status <> 'ready';")

        tdLog.info(sql)
        time.sleep(100)
    
    def check_result(self):
        expect_result = tdSql.query(self.checkquery)
        tdSql.printResult(expect_result)
        stream_result = tdSql.query(f"select * from rdb.rs{self.index}")
        tdSql.printResult(stream_result)
        
        tdSql.check_rows_loop
        
        
###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-
import os
import frame
import frame.etool
from frame.log import *
from frame.cases import *
from frame.sql import *
from frame.caseBase import *
from frame import *


class TDTestCase(TBase):
    def caseDescription(self):
        """
        [TD-23292] taosBenchmark test cases
        """

    def run(self):
        tdSql.query("select client_version()")
        client_ver = "".join(tdSql.res[0])
        major_ver = client_ver.split(".")[0]

        binPath = etool.benchMarkFile()
        cmd = (
            "%s -f ./tools/benchmark/basic/json/sml_json_insert_alltypes-same-min-max.json"
            % binPath
        )
        tdLog.info("%s" % cmd)
        os.system("%s" % cmd)
        tdSql.execute("reset query cache")
        tdSql.query("select count(*) from db.stb")
        rows = tdSql.res[0]
        tdSql.query("select * from db.stb")
        for row in range(rows[0]):
            if major_ver == "3":
                tdSql.checkData(row, 1, 1.0)
                tdSql.checkData(row, 2, 1.0)
                tdSql.checkData(row, 4, 1.0)
                tdSql.checkData(row, 5, 1.0)
                tdSql.checkData(row, 6, 1.0)
                tdSql.checkData(row, 7, 1.0)
                tdSql.checkData(row, 11, 30.0)
                tdSql.checkData(row, 12, 60000.0)
            else:
                tdSql.checkData(row, 1, 1.0)
                tdSql.checkData(row, 2, 1.0)
                tdSql.checkData(row, 3, 60000.0)
                tdSql.checkData(row, 5, 1.0)
                tdSql.checkData(row, 6, 1.0)
                tdSql.checkData(row, 7, 1.0)
                tdSql.checkData(row, 8, 1.0)
                tdSql.checkData(row, 12, 30.0)


    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)


tdCases.addWindows(__file__, TDTestCase())
tdCases.addLinux(__file__, TDTestCase())

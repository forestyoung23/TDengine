"""
TDengine Python Connector
"""
import shutil
import os
import re
import time
from threading import Thread

from ..util.file import dict2file
from ..util.remote import Remote


class TaosPy:
    def __init__(self, remote: Remote):
        self._remote = remote

    def check_install(self):
        pass

    def install(self, host, version, client_version):
        '''
        1. only support linux
        2. if not installed and check version if not match ,install taospy using "pip3 install taospy" 
        3. do not need to specify a package version 
        '''
        # server from env
        server = "192.168.1.131"
        if "PACKAGE_SERVER" in os.environ:
            server = os.environ["PACKAGE_SERVER"]
        # password from env
        password = "tbase125!"
        if "PACKAGE_SERVER_PASSWORD" in os.environ:
            password = os.environ["PACKAGE_SERVER_PASSWORD"]
        # root path from env
        rootPath = "/nas/TDengine"
        if "PACKAGE_SERVER_ROOT" in os.environ:
            rootPath = os.environ["PACKAGE_SERVER_ROOT"]
        installFlag = False
        result = self._remote.cmd(host, ["pip3 list installed|grep taospy"])
        print(result)
        if result == "":
            installFlag = True
        else:
            if not result is None:
                result = result.strip()
                if re.match(f'.* {version}$', result) == None:
                    installFlag = True
            else:
                installFlag = True
        if installFlag == True:
            # get package from server
            filePath = f"{rootPath}/v{client_version}/enterprise"
            fileName = f"TDengine-enterprise-client-{client_version}-Linux-x64.tar.gz"
            self._remote.get(server, f"{filePath}/{fileName}", "/tmp/", password)
            # push package to host
            filePath = time.strftime("%Y-%m-%d-%H-%M-%S", time.localtime())
            filePath = f"taostest-install-{filePath}"
            pkgDir = f"TDengine-enterprise-client-{client_version}"
            self._remote.put(host, f"/tmp/{fileName}", f"/tmp/{filePath}")
            # extract package && install
            cmds = [f"pip3 install taospy=={version}"]
            result = self._remote.cmd(host, cmds)
            # remove local file
            result = self._remote.cmd(host, [f"rm -rf /tmp/{filePath}"])

    def install_pkg(self, host, package):
        # push package to host
        filePath = time.strftime("%Y-%m-%d-%H-%M-%S", time.localtime())
        filePath = f"taostest-install-{filePath}"
        self._remote.put(host, package, f"/tmp/{filePath}")
        # extract package && install
        fileName = os.path.basename(package)
        cmds = [f"cd /tmp/{filePath}", f"tar xzf {fileName}", f"rm -f {fileName}", f"ls | tail -n1"]
        pkgDir = self._remote.cmd(host, cmds)
        if not pkgDir is None:
            pkgDir = pkgDir.strip()
            cmds = [f"cd /tmp/{filePath}/{pkgDir}", f"pip3 install connector/python", f"./install_client.sh"]
            result = self._remote.cmd(host, cmds)
        # remove local file
        result = self._remote.cmd(host, [f"rm -rf /tmp/{filePath}"])

    def configure(self, run_log_dir, nodeDict):
        dict2file(run_log_dir, "taos.cfg", nodeDict["spec"]["config"])

    def uninstall(self):
        pass

    def reset(self):
        pass

    def destroy(self, nodeDict):
        config = nodeDict["spec"]["config"]
        if "logDir" in config:
            shutil.rmtree(config["logDir"], ignore_errors=True)

    def _install(self, host, version, client_version, pkg):
        if pkg is None:
            #self.install(host, version, client_version)
            self.logger.info("_install ignore ")
        else:
            # if package specified, install the package without checking version
            self.install_pkg(host, pkg)

    def setup(self, run_log_dir, nodeDict):
        '''
        1. if system is not window,  install taospy
        2. else  skip setup taospy
        '''
        hosts = nodeDict["fqdn"]
        version = nodeDict["spec"]["version"]
        client_version = nodeDict["spec"]["client_version"]
        pkg = nodeDict["client_pkg"]
        if "dnodes" in nodeDict["spec"]:
            nodeDictList = [nodeDict["spec"]["dnodes"], nodeDict["spec"]["reserve_dnodes"]] if "reserve_dnodes" in nodeDict["spec"] else [nodeDict["spec"]["dnodes"]]
            for dnodeList in nodeDictList:
                for i in dnodeList:
                    fqdn, _ = i["endpoint"].split(":")
                    if "system" in i.keys() and i["system"].lower() == "windows":
                        self._remote._logger.debug(f"windows system can't install taospy")
                    else:
                        threads = []
                        for host in hosts:
                            t = Thread(target = self._install, args = (host, version, client_version, pkg))
                            t.start()
                            threads.append(t)
                        for thread in threads:
                            thread.join()
        else :       
            threads = []
            for host in hosts:
                t = Thread(target = self._install, args = (host, version, client_version, pkg))
                t.start()
                threads.append(t)
            for thread in threads:
                thread.join()
        self.configure(run_log_dir, nodeDict)

    def update_cfg(self, run_log_dir, tmp_dict, cfg_dict):
        tmp_dict["spec"]["config"].update(cfg_dict)
        dict2file(run_log_dir, "taos.cfg", tmp_dict["spec"]["config"])

    def get_cfg(self, tmp_dict, cfg_dict):
        if cfg_dict:
            for i in cfg_dict.keys():
                for j in tmp_dict["spec"].keys():
                    if i == j:
                        cfg_dict[i] = tmp_dict["spec"][i]
                        continue
                for j in tmp_dict["spec"]["config"].keys():
                    if i == j:
                        cfg_dict[i] = tmp_dict["spec"]["config"][i]
                        continue
            return cfg_dict
        else:
            return tmp_dict["spec"]

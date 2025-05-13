#!/bin/bash

##################################################
# 
# Do simulation test 
#
##################################################

set -e
#set -x

CMD_NAME=
LOOP_TIMES=5
SLEEP_TIME=0

while getopts "f:" arg
do
  case $arg in
    f)
      CMD_NAME=$OPTARG
      ;;
    ?)
      echo "unknow argument"
      ;;
  esac
done

../tests/script/sh/stop_dnodes.sh; 
rm -rf ~/TDinternal/sim/*; 
pytest $CMD_NAME


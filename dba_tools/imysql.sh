#!/bin/sh

if [ $# -lt 1 ];then
   echo "usage: $0 port {sql}"
   echo "arg num: $#"
   exit
fi

cmd="e"
if [ $# -ge 2 ];then
    cmd=$2
fi

echo "cmd: ${cmd}"

port=$1
base_dir=`dirname "$PWD"`
etcfile=${base_dir}/etc/my_${port}.cnf
sockfile=` grep "^socket *=" ${etcfile}  | tail -n1| awk -F"=" '{print $2}'`

echo ${sockfile}
echo ${base_dir}

if [ "$cmd" != "e" ];then
    cd ${base_dir} ; ./bin/mysql -uroot -proot -S${sockfile} -A -e "$cmd"
else
    cd ${base_dir} ; ./bin/mysql -uroot -proot -S${sockfile} -A
fi


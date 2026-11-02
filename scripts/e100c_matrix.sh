#!/bin/bash
# E100c 读写组合扫描：先单口逐对跑，再 4 口并发。每组跑完追加一行到 <输出目录>/summary.csv，原始日志在 <输出目录>/raw。
# 已有结果的组会跳过，中断后重跑同一命令即可续跑。
#
#   ./scripts/e100c_matrix.sh <输出目录> [每组秒数，默认 30]
#
#   读：1~4 拍 × 每帧 1~4 个事务，16 组
#   写：每帧事务数 × 拍数 ≤ 8，16 组（1 拍 1~8 个，2 拍 1~4 个，3 拍、4 拍 1~2 个）
#   每组：每对 4 流 × 200，-s，--id-seq，反射端用 e100c.sh 默认核数
#   4 对口 × 32 组 + 4 口并发 32 组，共 160 组；每组 30 秒时约 1 小时 50 分
#
# 后台跑：setsid nohup ./scripts/e100c_matrix.sh /home/mx/mengx/axi_pcap/matrix > matrix.out 2>&1 < /dev/null &
# 别放 /tmp，重启会丢。
cd "$(dirname "$0")/.." || exit 1
L=${1:?用法: $0 <输出目录> [每组秒数]}; T=${2:-30}
mkdir -p "$L/raw"
IFS_ALL="ens108np0 ens103np0 ens119np0 ens111np0 ens106np0 ens101np0 ens114np0 ens110np0"
READ="1:1 1:2 1:3 1:4 2:1 2:2 2:3 2:4 3:1 3:2 3:3 3:4 4:1 4:2 4:3 4:4"
WRITE="1:1 1:2 1:3 1:4 1:5 1:6 1:7 1:8 2:1 2:2 2:3 2:4 3:1 3:2 4:1 4:2"

# 8 个网口的丢包 + 缓冲不足 + CRC 计数
errs(){ for i in $IFS_ALL; do sudo -n ethtool -S $i | awk '/^ *(rx_discards_phy|rx_out_of_buffer|rx_crc_errors_phy):/{s+=$2}END{printf "%d ", s+0}'; done; }
# 结束本组的 axiperf（方括号避免 pkill 匹配到执行它的 shell）
stop(){ sudo pkill -INT -f '[a]xiperf --file-prefix axi'; sleep 3; sudo pkill -KILL -f '[a]xiperf --file-prefix axi' 2>/dev/null; sleep 1; }
[ -f "$L/summary.csv" ] || echo "time,scope,id,op,beats,pack,txL2G,rxL2G,dataG,txnM,p50us,p99us,p999us,maxus,err,lost,nic_err_delta" > "$L/summary.csv"

one(){ # $1=scope（single / four）$2=op $3=拍数 $4=每帧事务数 $5...=对口 id
	local scope=$1 op=$2 b=$3 k=$4; shift 4; local ids="$*" tag="${1}_${2}_b${3}_p${4}"
	tag="${scope}_${op}_b${b}_p${k}"
	if grep -qE "^[^,]*,$scope,${ids%% *},$op,$b,$k," "$L/summary.csv"; then echo "skip $tag $ids"; return; fi
	stop
	for i in $ids; do ./scripts/e100c.sh $i refl > "$L/raw/${tag}_r$i.log" 2>&1 < /dev/null & done
	sleep 5
	local a z d pk pids=()
	a=$(errs)
	[ $op = read ] && pk="--rpack $k" || pk="--pack $k"
	for i in $ids; do ( ./scripts/e100c.sh $i snd $op 200 -s -t $T --beats $b $pk --id-seq > "$L/raw/${tag}_s$i.log" 2>&1 < /dev/null ) & pids+=($!); done
	wait "${pids[@]}"                            # 只等发送端；反射端由 stop 结束
	z=$(errs)
	d=$(paste <(echo $a | tr ' ' '\n') <(echo $z | tr ' ' '\n') | awk '{s+=$2-$1}END{print s+0}')
	for i in $ids; do
		local line; line=$(grep -E '^all ' "$L/raw/${tag}_s$i.log" | tail -1)
		[ -n "$line" ] || line="all - - - - - - - - - - - - -"
		echo "$(date +%T),$scope,$i,$op,$b,$k,$(echo $line | awk '{print $2","$3","$4","$5","$6","$7","$8","$9","$10","$11}'),$d" >> "$L/summary.csv"
	done
	stop
}

date >> "$L/runs"
for i in 0 1 2 3; do
	for c in $READ;  do one single read  ${c%:*} ${c#*:} $i; done
	for c in $WRITE; do one single write ${c%:*} ${c#*:} $i; done
done
for c in $READ;  do one four read  ${c%:*} ${c#*:} 0 1 2 3; done
for c in $WRITE; do one four write ${c%:*} ${c#*:} 0 1 2 3; done
date > "$L/end"

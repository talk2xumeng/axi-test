#!/bin/bash
# axiperf 单机两口测试：一个口发送、一个口反射，两个进程开两个窗口。每条 axiperf 命令执行前完整打印。
#
#   ./scripts/e100c.sh setup                准备环境（重启后要重做）
#   ./scripts/e100c.sh check                看链路、TC、pause、大页
#   ./scripts/e100c.sh refl [rw]            窗口 1：反射端（单写 / 单读不带 rw，读写并发带 rw）
#   ./scripts/e100c.sh ping                 窗口 2：低速打通（每流在途 4，每包 1 事务 2 拍，打印前 8 帧）
#   ./scripts/e100c.sh snd write|read|rw W  窗口 2：跑测试，每流在途 W，4 条流，每方向总在途 4W
#   ./scripts/e100c.sh cnt snd|refl         窗口 3：每秒看网卡计数
#   ./scripts/e100c.sh cap                  抓进反射端网口的帧（先停反射端），存 /tmp/refl_in_<PAIR>.pcap
#
# 环境变量：
#   PAIR=e100c（默认）ens106np0 发送 → E100c → ens101np0 反射
#   PAIR=e100c_r      ens101np0 发送 → E100c → ens106np0 反射（e100c 的反方向，user.port 默认 0，b-port 默认 2）
#   PAIR=e100c2       ens111np0 发送 → E100c → ens119np0 反射（可与 e100c 同时跑，各开自己的窗口）
#   PAIR=direct       ens102np0 发送 → 直连 → ens110np0 反射
#   COUNT=10 ping 只发 10 帧（1~511）；T=20 运行秒数；BRIEF=0 完整输出（看每流 MAC、ign、xmac、pcpx）
#   HDR=sue 发送端改用 SUEv1-标准ETH 头（事务头在第 20 字节；反射端只有单流，本端 GPU ID 取 SUE_DST）
#     SUE_DST=0xc0c0 目的 GPU ID；SUE_SRC=1 源 GPU ID；SUE_ET=0x88ee EtherType；SUE_FMT=0 Format；SUE_PT=0 PktType
#   命令行末尾的参数原样传给 axiperf，如 ./scripts/e100c.sh snd write 16 --pcap /tmp/snd.pcap
#
# 接 E100c 前改下面的值（也可用同名环境变量覆盖）：
VID=${VID:-0}                   # 两端相同，默认 0（帧里仍带 802.1Q 标签，PCP 用来区分 VC）
E100C_MAC1=${E100C_MAC1:-c0:c0:00:a0:00:00}   # 请求 DMAC（E100c 样例值，10-30 实测不校验）
E100C_PORT1=${E100C_PORT1:-3}  # ens106np0 发出帧的 user.port（10-31 实测 3 转发到 ens101np0；0 = 不传 --e100c-port）
E100C_MAC2=${E100C_MAC2:-}     # E100c 上接 ens111np0 那个口的 MAC
E100C_PORT2=${E100C_PORT2:-}   # ens111np0 所接 E100c 的 ETH 口号 1~4
E100C_SMAC=${E100C_SMAC:-}     # 发送端 SMAC，E100c 要求固定地址时填（只用一条流时加 FLOWS=1）；不填用网卡 MAC

PAIR=${PAIR:-e100c}
case "$PAIR" in
e100c)   # 两口都在 NUMA0
	SND_IF=ens106np0  SND_BDF=0000:71:00.0  SND_MAIN=8   SND_TX=9   SND_RX=137
	REFL_IF=ens101np0 REFL_BDF=0000:60:00.0 REFL_MAIN=24 REFL_C0=25
	E100C_MAC=$E100C_MAC1 E100C_PORT=$E100C_PORT1
	;;
e100c_r)  # e100c 的反方向：两口都在 NUMA0
	SND_IF=ens101np0  SND_BDF=0000:60:00.0  SND_MAIN=24  SND_TX=25  SND_RX=153
	REFL_IF=ens106np0 REFL_BDF=0000:71:00.0 REFL_MAIN=8  REFL_C0=9
	E100C_MAC=$E100C_MAC1 E100C_PORT=${E100C_PORT_R:-0} B_PORT=${B_PORT:-2}
	;;
e100c2)  # 两口都在 NUMA1
	SND_IF=ens111np0  SND_BDF=0000:dd:00.0  SND_MAIN=88  SND_TX=89  SND_RX=217
	REFL_IF=ens119np0 REFL_BDF=0000:f1:00.0 REFL_MAIN=72 REFL_C0=73
	E100C_MAC=$E100C_MAC2 E100C_PORT=$E100C_PORT2
	;;
direct)  # 发送端 NUMA0，反射端 NUMA1
	SND_IF=ens102np0  SND_BDF=0000:61:00.0  SND_MAIN=40  SND_TX=41  SND_RX=169
	REFL_IF=ens110np0 REFL_BDF=0000:dc:00.0 REFL_MAIN=80 REFL_C0=81
	;;
*) echo "PAIR 只能是 e100c、e100c_r、e100c2 或 direct" >&2; exit 1 ;;
esac

# E100c 格式：EtherType 0x88ee + 0000，事务头从第 16 字节开始；响应不能按 DMAC 导流，只用 1 条流
if [ "$PAIR" = direct ]; then FLOWS=${FLOWS:-4}; NOLEN_ARG=
elif [ "${HDR:-eth}" = sue ]; then
	FLOWS=${FLOWS:-1}
	NOLEN_ARG="--hdr sue --gpu-id ${SUE_SRC:-1} --peer-gpu-id ${SUE_DST:-0xc0c0} --sue-ethertype ${SUE_ET:-0x88ee} --sue-format ${SUE_FMT:-0} --sue-pkttype ${SUE_PT:-0}"
else FLOWS=${FLOWS:-1}; NOLEN_ARG="--ethertype 0x88ee --min-frame ${MIN_FRAME:-0}"; fi
# E100c 的 ID 只在收到响应后释放：不做超时回收重发，否则响应一停，发出的请求会超过 511 把 ID 耗尽
TMO_ARG=; [ "$PAIR" = direct ] || TMO_ARG="--timeout-us 0"
# 反射端回的写响应 B 的 port id（ETHI1 的 port 0 经 UHUBT1 到 die1 ETHT3）
BPORT_ARG=; [ "$PAIR" = direct ] || BPORT_ARG="--b-port ${B_PORT:-0} --b-tag ${B_TAG:-0} --b-pack 1"   # OISA 头只有一个 tag_id：每帧 1 个 B
SND_DA=txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0
REFL_DA=dv_flow_en=2,$SND_DA
AXI=$(cd "$(dirname "$0")/.." && pwd)/axiperf
BRIEF_ARG=--brief; [ "${BRIEF:-1}" = 0 ] && BRIEF_ARG=

run() { echo "+ $*"; "$@"; }

need() {
	local miss=0
	if [ "$PAIR" != direct ] && [ "$1" = snd ]; then
		[ -n "$E100C_MAC" ]  || { echo "未设 $PAIR 的 E100C_MAC" >&2; miss=1; }
		[ -n "$E100C_PORT" ] || { echo "未设 $PAIR 的 E100C_PORT" >&2; miss=1; }
	fi
	[ "$miss" = 0 ] || { echo "改 $0 开头的值，或写成 VID=1 E100C_MAC1=xx E100C_PORT1=1 $0 ..." >&2; exit 1; }
}

# 发送端（$1=op，$2=每流在途，$3=时长，其余传给 axiperf）：上下文 k 的发送线程在 SND_TX+k，接收线程在其超线程 SND_RX+k
sender() {
	local op=$1 w=$2 t=$3 n=$FLOWS k lc dst; shift 3
	need snd
	[ "$op" = rw ] && n=$((FLOWS * 2))
	lc=0@$SND_MAIN
	for k in $(seq 0 $((n - 1))); do lc+=",$((2*k+1))@$((SND_TX+k)),$((2*k+2))@$((SND_RX+k))"; done
	if [ "$PAIR" != direct ]; then dst="--dmac 0,$E100C_MAC --e100c-port 0,$E100C_PORT"
	else dst="--dmac 0,$(cat /sys/class/net/$REFL_IF/address)"; fi
	[ "$E100C_PORT" = 0 ] && dst="${dst/ --e100c-port 0,0/}"   # 不传该参数时 user.port 就是 0
	local rp=; [ "$op" = write ] || rp="--rpack 4"
	[ -n "$E100C_SMAC" ] && dst="$dst --smac $E100C_SMAC"
	[ "${HDR:-eth}" = sue ] && [ "$PAIR" != direct ] && { dst=; [ "$E100C_PORT" = 0 ] || dst="--e100c-port 0,$E100C_PORT"; }   # 地址由 GPU ID 决定
	run sudo "$AXI" --file-prefix "axisnd_$PAIR" --huge-unlink --lcores "$lc" -a "$SND_BDF,$SND_DA" -- \
		--mode sender --vid "$VID" --flows $FLOWS --burst 16 --window "$w" --time "$t" $dst --one-dmac $NOLEN_ARG $TMO_ARG --op "$op" $rp "$@"
}

cmd=$1; shift
case "$cmd" in
setup)
	for i in $SND_IF $REFL_IF; do
		if [ "$PAIR" != direct ]; then   # 内核的 mDNS / DHCP / IPv6 帧也会进 E100c，被当成请求，先让测试口静默（重启后恢复）
			run sudo nmcli device set "$i" managed no
			run sudo sysctl -q "net.ipv6.conf.$i.disable_ipv6=1"
		fi
		run sudo ip link set "$i" up
		run sudo mlnx_qos -i "$i" --prio_tc=0,0,0,0,0,0,0,0 >/dev/null
		run sudo ethtool -A "$i" rx off tx off
	done
	for n in 0 1; do
		echo 4096 | sudo tee /sys/devices/system/node/node$n/hugepages/hugepages-2048kB/nr_hugepages >/dev/null
	done
	"$0" check
	;;
check)
	for i in $SND_IF $REFL_IF; do
		echo "== $i  MAC $(cat /sys/class/net/$i/address)  carrier $(cat /sys/class/net/$i/carrier 2>/dev/null)  $(cat /sys/class/net/$i/speed 2>/dev/null) Mb/s  NUMA $(cat /sys/class/net/$i/device/numa_node)  $(basename "$(readlink /sys/class/net/$i/device)")"
		echo "   TC 数 $(sudo mlnx_qos -i "$i" | grep -c '^tc:')；pause $(sudo ethtool -a "$i" | awk '/^RX:|^TX:/{printf "%s %s ", $1, $2}')"
	done
	echo "大页（2MB）：node0 $(cat /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages)  node1 $(cat /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages)"
	;;
refl)
	need refl
	if [ "${HDR:-eth}" = sue ]; then   # --rx-split 只支持 eth 头：单流单队列；本端 GPU ID 是发送端的目的 GPU ID
		[ "$1" != rw ] || { echo "HDR=sue 的反射端只有单流，不支持 rw" >&2; exit 1; }
		run sudo "$AXI" --file-prefix "axirefl_$PAIR" --huge-unlink -l "$REFL_MAIN,$REFL_C0" -a "$REFL_BDF,$SND_DA" -- \
			--mode reflector --vid "$VID" --flows 1 --rxd 4096 --burst 16 --b-pack 16 $NOLEN_ARG --gpu-id "${SUE_DST:-0xc0c0}" "$@"
		exit
	fi
	split=4; rw=
	if [ "$1" = rw ]; then split=2; rw="--op rw"; shift; fi
	run sudo "$AXI" --file-prefix "axirefl_$PAIR" --huge-unlink -l "$REFL_MAIN,$REFL_C0-$((REFL_C0 + 3))" -a "$REFL_BDF,$REFL_DA" -- \
		--mode reflector --vid "$VID" --rx-split $split $rw --rxd 4096 --burst 16 --b-pack 16 $NOLEN_ARG $BPORT_ARG $BRIEF_ARG "$@"
	;;
ping)
	# 与 E100c 样例相同：每包 1 个事务、2 拍。COUNT=N：只发 N 帧，窗口也取 N，没有响应也能发满
	if [ -n "$COUNT" ]; then sender write "$COUNT" "${T:-3}" --pack 1 --beats 2 --dump 8 --count "$COUNT" "$@"
	else sender write 4 "${T:-5}" --pack 1 --beats 2 --dump 8 "$@"; fi
	;;
snd)
	op=$1; w=$2
	case "$op" in write|read|rw) ;; *) echo "用法: $0 snd write|read|rw <每流在途>" >&2; exit 1 ;; esac
	[ -n "$w" ] || { echo "用法: $0 snd write|read|rw <每流在途>" >&2; exit 1; }
	# 一个请求包占多个 ID（写 --pack 2，读 --rpack 4），每流在途小于它时一帧也发不出去
	min=2; [ "$op" = write ] || min=4
	[ "$w" -ge "$min" ] || { echo "$op 的每流在途至少为 $min，否则发不出包" >&2; exit 1; }
	shift 2
	sender "$op" "$w" "${T:-20}" $BRIEF_ARG "$@"
	;;
cnt)
	case "$1" in snd) i=$SND_IF ;; refl) i=$REFL_IF ;; *) echo "用法: $0 cnt snd|refl" >&2; exit 1 ;; esac
	exec watch -n1 "sudo ethtool -S $i | grep -E '^ *(tx_packets_phy|rx_packets_phy|rx_discards_phy|rx_crc_errors_phy|rx_out_of_buffer):|(rx|tx)_prio[0-7]_(packets|pause):' | grep -v ': 0$'"
	;;
cap)
	run sudo tcpdump -i "$REFL_IF" -e -nn -c "${1:-200}" -w "/tmp/refl_in_$PAIR.pcap"
	echo "查看: sudo tcpdump -r /tmp/refl_in_$PAIR.pcap -e -nn -xx | head -80"
	;;
*)
	sed -n '2,10p' "$0"; exit 1
	;;
esac

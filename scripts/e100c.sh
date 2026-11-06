#!/bin/bash
# axiperf 单机两口测试：一个口发送、一个口反射，两个进程开两个窗口。每条 axiperf 命令执行前完整打印。
#
#   ./scripts/e100c.sh setup                      准备所有对口的环境（重启后要重做）
#   ./scripts/e100c.sh check                      看所有对口的链路、TC、pause、大页
#   ./scripts/e100c.sh <id> refl [rw] [选项]      窗口 1：反射端（单写 / 单读不带 rw，读写并发带 rw）
#   ./scripts/e100c.sh <id> ping [选项]           窗口 2：低速打通（每流在途 4，每包 1 事务 2 拍，打印前 8 帧）
#   ./scripts/e100c.sh <id> snd write|read|rw|mix W [选项]  窗口 2：跑测试，每流在途 W（E100c 读写并发用 mix：偶数流写、奇数流读，拍数相同）
#   ./scripts/e100c.sh <id> cnt snd|refl          窗口 3：每秒看网卡计数
#   ./scripts/e100c.sh <id> cap [帧数]            抓进反射端网口的帧（先停反射端），存 /tmp/refl_in_<名字>.pcap
#
# <id> 是请求里的 user.port（E100c 的 port id），每个 id 对应一对口：
#   0  ens108np0 → ens110np0（die1 chip0 → die0 chip2）
#   1  ens111np0 → ens106np0（die1 chip1 → die0 chip3）
#   2  ens103np0 → ens101np0（die1 chip2 → die0 chip0；已做双向写和小包验证）
#   3  ens114np0 → ens119np0（die1 chip3 → die0 chip1）
# 其他对口用名字代替 id：
#   r0~r3    id 0~3 的反方向（如 r0：ens110np0 → ens108np0），和正方向一起跑时两边都加 --iso；最多 -f 4、-r 4
#   b2       id 2 的反方向，供单对口双向 8 流、8 核反射使用；不能与 id 0、1 并发（借用其 NUMA0 核）
#   b1       id 1 的反方向，8 流、8 核反射（两口分在交换芯片 6e、da 下）；不能与 id 2、3、b2 并发
#   y0、y3、w2  id 0、3、2 的别名（报告里用过的名字）
#   z1、v3   交叉路由 ens111 → ens110 和它的反方向（z1 与直连 d1 网卡、核相同，用来比时延）；不与 id 0、1、3 并发
#   e100c_r  是 b2 的兼容别名；e100c_r1、e100c2、x1、xa、xb 是旧接线专用，已停用
#   direct   ens102np0 发送 → 直连 → ens110np0 反射
#   d0~d2    网线直连：d0 = ens108 → ens103，d1 = ens111 → ens110，d2 = ens106 → ens101（拔掉 E100c 的线，两口直接对接）
#
# 选项（可放在命令后任意位置，其余参数原样传给 axiperf）：
#   -f N     发送端流数，N 为 2 的幂 [snd 4，ping 1，--sue 1]。mix 时是读写合计：偶数号流写、奇数号流读，-f 8 即 4 流写 + 4 流读
#   -t SEC   运行秒数 [snd 20，ping 5，ping 带 -n 时 3；0 = 一直跑]
#   -n N     ping 只发 N 帧（1~511）
#   -r N     反射端核数 1、2、4、8、16 [4]。1 = 单队列单核，响应严格按收到的顺序发出。
#            读写混跑时 8 核比 4 核高约 16%（110G → 128G）；16 核反而更低。只读、只写 4 核和 8 核相同。
#            旧接线下曾观察到两对同时用 -r 8 掉速；新接线尚未验证
#   -s       接收核不与发送核共用物理核（读的性能配置要加）
#   -v       完整输出（每流结果、MAC、ign、xmac、pcpx）；默认精简输出
#   --txc N  发送端每上下文 N 个发送核 [1]
#   --sue    发送端改用 SUEv1-标准ETH 头（事务头在第 20 字节；反射端只有单流，本端 GPU ID 取 SUE_DST）
#   --iso    发送端、反射端都加 --isolate：同一网口可同时跑本方向的发送端和反方向的反射端（双向打流）；反射端多核按请求 tag 低位分队列
#   --dry-run 只打印 ping / snd / refl 的命令，不运行 sudo 或 axiperf
#
# 常用的 axiperf 参数（直接加在命令后面）：
#   发送端：--beats N  每事务拍数      --pack N  写每帧事务数      --rpack N  读每帧事务数（最多 4）
#           --count N  只发 N 帧       --gap-us N  每隔 N 微秒发 1 帧   --id-seq  ID 按顺序分配（读的性能更高）
#           --dump N   打印前 N 帧     --pcap F --pcap-count N  抓包存到文件 F，最多 N 帧
#           --ar-user-low X  读请求 aruser[29:0]。默认按 --beats 自动填 sect_mask（bit 27:20，地址 0 时 1~4 拍为 0x03、0x0F、0x3F、0xFF）；
#                            填 0 可复现多拍读卡死（累计几十个响应后卡死，要给 E100c 重新上下电才能恢复）
#   反射端：读响应拍数默认取自请求；--r-beats N  固定回 N 拍
#   调试读响应的填法：--r-user X  --r-len-data  --r-pack-beats  --r-echo-aruser N  --r-split N
#   全部参数看 ./axiperf --help
#
# 先准备环境（主机重启后做一次）：
#   ./scripts/e100c.sh setup
#   ./scripts/e100c.sh check
#
# 写，单对口，性能配置（窗口 1 跑反射端，窗口 2 跑发送端，下同）：
#   ./scripts/e100c.sh 1 refl
#   ./scripts/e100c.sh 1 snd write 200 --pack 2 --beats 4
#
# 写，低速打通，只发 10 帧并打印：
#   ./scripts/e100c.sh 1 refl -r 1 -v
#   ./scripts/e100c.sh 1 ping -n 10
#
# 读 1 拍，单对口，性能配置：
#   ./scripts/e100c.sh 1 refl
#   ./scripts/e100c.sh 1 snd read 200 -s -t 30 --rpack 4 --beats 1 --id-seq
#
# 读 1 拍，单流慢速，发 100 帧，看每流结果并抓包：
#   ./scripts/e100c.sh 1 refl -r 1 -v --pcap /tmp/r.pcap --pcap-count 2000
#   ./scripts/e100c.sh 1 snd read 4 -f 1 -v -t 5 --rpack 1 --beats 1 --count 100 --gap-us 1000 --id-seq --pcap /tmp/s.pcap --pcap-count 2000
#
# 读 2 拍，单对口，性能配置：
#   ./scripts/e100c.sh 1 refl
#   ./scripts/e100c.sh 1 snd read 200 -s -t 30 --rpack 1 --beats 2 --id-seq
#
# 读 4 拍，单对口，性能配置：
#   ./scripts/e100c.sh 1 refl
#   ./scripts/e100c.sh 1 snd read 200 -s -t 30 --rpack 1 --beats 4 --id-seq
#
# 读 4 拍，单流慢速，每 100ms 发 1 帧（不会拼包），发 80 帧并抓包：
#   ./scripts/e100c.sh 1 refl -r 1 -v --pcap /tmp/r.pcap --pcap-count 4000
#   ./scripts/e100c.sh 1 snd read 4 -f 1 -v -t 14 --rpack 1 --beats 4 --count 80 --gap-us 100000 --id-seq --pcap /tmp/s.pcap --pcap-count 4000
#
# 四对口并发（各对口的反射端都起来后再起发送端；反射端用默认 4 核）：
#   for i in 0 1 2 3; do ./scripts/e100c.sh $i refl --time 50 & done; sleep 6
#   写 4 拍：  for i in 0 1 2 3; do ./scripts/e100c.sh $i snd write 200 -t 30 --pack 2 --beats 4 --id-seq & done; wait
#   读 4 拍：  for i in 0 1 2 3; do ./scripts/e100c.sh $i snd read 200 -s -t 30 --rpack 2 --beats 4 --id-seq & done; wait
#   读 1 拍：  for i in 0 1 2 3; do ./scripts/e100c.sh $i snd read 200 -s -t 30 --rpack 4 --beats 1 --id-seq & done; wait
#   读写混跑：for i in 0 1 2 3; do ./scripts/e100c.sh $i snd mix 200 -f 8 -s -t 30 --pack 2 --rpack 2 --beats 4 --id-seq & done; wait
#   新接线下的四对口并发性能尚未验证；以上示例使用 4 核反射端
#
# 测试方法：
#   - 单对口每次只改一个变量（拍数、每帧事务数、流数、窗口、反射端核数），其他保持“4 流 × 200，-s，--id-seq”。
#   - 看吞吐时同时看 p50：在途翻倍时延也翻倍、吞吐不变，说明是在前面排队，加窗口和流数没用。
#   - 反射端的 busy% 会低估负载（显示 70% 时可能已是瓶颈），判断是否 CPU 瓶颈要加核对比，不要只看 busy%。
#   - 慢速抓包用 -f 1 -r 1 --gap-us 100000，避免 E100c 拼包；拼包时按帧数推算会出错。
#   - 停反射端只发一次 Ctrl-C；连按两次会强制退出，抓包文件写不完整。
#   - 日志和抓包放 /home/mx/mengx，不要放 /tmp（主机重启会清空）。
#   - 每次测完看 err、lost 和网口计数（cnt）：都为 0 才算有效。
#
# 读写组合扫描（单口逐对 + 四口并发，每组 30 秒约 1 小时 50 分；中断后重跑同一命令会续跑，结果在 summary.csv）：
#   setsid nohup ./scripts/e100c_matrix.sh /home/mx/mengx/axi_pcap/matrix 30 > /home/mx/mengx/axi_pcap/matrix.out 2>&1 < /dev/null &
#
# 读写并发，单对口，4 拍，2 流写 + 2 流读（读写各 400 在途）：
#   ./scripts/e100c.sh 1 refl
#   ./scripts/e100c.sh 1 snd mix 200 -s -t 30 --pack 2 --rpack 1 --beats 4 --id-seq
#
# 读写并发，单对口，4 拍，4 流写 + 4 流读（读写各 800 在途，和单独只读、只写的资源相同；反射端 8 核约 128G，4 核约 110G）：
#   ./scripts/e100c.sh 1 refl -r 8
#   ./scripts/e100c.sh 1 snd mix 200 -f 8 -s -t 30 --pack 2 --rpack 2 --beats 4 --id-seq
#
# 双向打写示例（id 0 和 r0，四个进程分开窗口）：
#   ./scripts/e100c.sh 0 refl --iso -r 4
#   ./scripts/e100c.sh r0 refl --iso -r 4
#   ./scripts/e100c.sh 0 snd write 200 -f 4 -t 30 --pack 2 --beats 4 --id-seq --iso
#   ./scripts/e100c.sh r0 snd write 200 -f 4 -t 30 --pack 2 --beats 4 --id-seq --iso
#
# 单对口双向打写（ens103 ↔ ens101，每方向 8 流 × 100、反射 8 核；b2 借用其他对口的核）：
#   ./scripts/e100c.sh 2 refl --iso -r 8
#   ./scripts/e100c.sh b2 refl --iso -r 8
#   ./scripts/e100c.sh 2 snd write 100 -f 8 -t 30 --pack 2 --beats 4 --id-seq --iso
#   ./scripts/e100c.sh b2 snd write 100 -f 8 -t 30 --pack 2 --beats 4 --id-seq --iso
#
# 4 对口双向打写：8 个方向同时跑（合计约 512G，受主机核数限制只能 4 流 + 4 核）：
#   for i in 0 1 2 3 r0 r1 r2 r3; do ./scripts/e100c.sh $i refl --iso -r 4 --time 50 & done; sleep 10
#   for i in 0 1 2 3 r0 r1 r2 r3; do ./scripts/e100c.sh $i snd write 200 -f 4 -t 30 --pack 2 --beats 4 --id-seq --iso & done; wait
#
# 看网卡计数（另开窗口）：
#   ./scripts/e100c.sh 1 cnt snd
#   ./scripts/e100c.sh 1 cnt refl
#
# 看帮助：./scripts/e100c.sh help
#
# 改环境时改下面的值：
VID=0                           # 两端相同（帧里仍带 802.1Q 标签，PCP 用来区分 VC）
E100C_MAC1=c0:c0:00:a0:00:00    # 请求 DMAC（E100c 样例值，10-30 实测不校验）
E100C_SMAC=00:00:00:00:04:01    # 发送端 SMAC，也是 OISA 头第 6~11 字节的初值
E100C_MAC2=                     # e100c2：E100c 上接 ens111np0 那个口的 MAC
E100C_PORT2=                    # e100c2：ens111np0 所接 E100c 的 ETH 口号 1~4
SUE_DST=0xc0c0 SUE_SRC=1 SUE_ET=0x88ee SUE_FMT=0 SUE_PT=0   # --sue：目的 / 源 GPU ID、EtherType、Format、PktType
MIN_FRAME=0 B_PORT=0 B_TAG=0 R_USER=0   # 反射端默认值，命令行加 --min-frame / --b-port / --b-tag / --r-user 可覆盖

usage() { sed -n '2,/^# 改环境时/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; }

ID=$1; [ $# -gt 0 ] && shift
DIRECT=0   # 1 = 网口直连，不经过 E100c（标准帧格式，DMAC 用反射口 MAC）
case "$ID" in
""|-h|--help|help) usage; exit 0 ;;
setup|check) for i in 0 1 2 3; do "$0" "$i" "$ID" || exit; done; exit 0 ;;
0|e100c_b|y0)  # die1 chip0 → die0 chip2；发送 NUMA0，反射 NUMA1
	PAIR=e100c_b
	SND_IF=ens108np0  SND_BDF=0000:73:00.0  SND_MAIN=27  SND_TX=28  SND_RX=156
	REFL_IF=ens110np0 REFL_BDF=0000:dc:00.0 REFL_MAIN=108 REFL_C0=109
	E100C_MAC=$E100C_MAC1 E100C_PORT=0
	;;
1|e100c_d)  # die1 chip1 → die0 chip3；发送 NUMA1，反射 NUMA0
	PAIR=e100c_d
	SND_IF=ens111np0  SND_BDF=0000:dd:00.0  SND_MAIN=65  SND_TX=66  SND_RX=194
	REFL_IF=ens106np0 REFL_BDF=0000:71:00.0 REFL_MAIN=44 REFL_C0=45
	E100C_MAC=$E100C_MAC1 E100C_PORT=1
	;;
2|e100c|w2)   # die1 chip2 → die0 chip0
	PAIR=e100c
	SND_IF=ens103np0  SND_BDF=0000:62:00.0  SND_MAIN=1   SND_TX=2   SND_RX=130
	REFL_IF=ens101np0 REFL_BDF=0000:60:00.0 REFL_MAIN=18 REFL_C0=19
	E100C_MAC=$E100C_MAC1 E100C_PORT=2
	;;
3|e100c_c|y3)  # die1 chip3 → die0 chip1；两端 NUMA1
	PAIR=e100c_c
	SND_IF=ens114np0  SND_BDF=0000:e0:00.0  SND_MAIN=91  SND_TX=92  SND_RX=220
	REFL_IF=ens119np0 REFL_BDF=0000:f1:00.0 REFL_MAIN=82 REFL_C0=83
	E100C_MAC=$E100C_MAC1 E100C_PORT=3
	;;
b2|e100c_r)  # id 2 的反方向，8 流 + 8 核反射；借用 id 0、1 在 NUMA0 的核
	PAIR=e100c_r
	SND_IF=ens101np0  SND_BDF=0000:60:00.0  SND_MAIN=27  SND_TX=28  SND_RX=156
	REFL_IF=ens103np0 REFL_BDF=0000:62:00.0 REFL_MAIN=44 REFL_C0=45
	E100C_MAC=$E100C_MAC1 E100C_PORT=0 B_PORT=2
	;;
b1)  # id 1 的反方向，8 流 + 8 核反射：ens106（die0 chip3，6e）→ port 3 → ens111（die1 chip1，da）；可与 id 1 的 -f 8、-r 8 同时跑，两对口分在两个交换芯片下
	PAIR=e100c_b1
	SND_IF=ens106np0  SND_BDF=0000:71:00.0  SND_MAIN=35  SND_TX=36  SND_RX=164
	REFL_IF=ens111np0 REFL_BDF=0000:dd:00.0 REFL_MAIN=82 REFL_C0=83
	E100C_MAC=$E100C_MAC1 E100C_PORT=3 B_PORT=1
	;;
r0|r1|r2|r3)  # id 0~3 的反方向：port = id xor 2，b-port = id；与正方向并发最多 -f 4、-r 4
	N=${ID#r} PAIR=e100c_$ID
	case $N in
	0) SND_IF=ens110np0 SND_BDF=0000:dc:00.0 SND_TX=100 SND_MAIN=251 REFL_IF=ens108np0 REFL_BDF=0000:73:00.0 REFL_MAIN=53 REFL_C0=49 ;;
	1) SND_IF=ens106np0 SND_BDF=0000:71:00.0 SND_TX=36  SND_MAIN=187 REFL_IF=ens111np0 REFL_BDF=0000:dd:00.0 REFL_MAIN=118 REFL_C0=113 ;;
	2) SND_IF=ens101np0 SND_BDF=0000:60:00.0 SND_TX=10  SND_MAIN=188 REFL_IF=ens103np0 REFL_BDF=0000:62:00.0 REFL_MAIN=54 REFL_C0=23 ;;
	3) SND_IF=ens119np0 SND_BDF=0000:f1:00.0 SND_TX=74  SND_MAIN=252 REFL_IF=ens114np0 REFL_BDF=0000:e0:00.0 REFL_MAIN=117 REFL_C0=87 ;;
	esac
	SND_RX=$((SND_TX + 128))   # 反向主核单独指定，避免与其他对口工作核共用物理核
	E100C_MAC=$E100C_MAC1 E100C_PORT=$((N ^ 2)) B_PORT=$N
	;;
e100c_r1)  # id 1 的反方向，NUMA1：借用 id 3 的核（发送 91~107，反射 108~116），不能和 id 3 同时跑；可与 id 1 同时跑（双向打流加 --iso）
	PAIR=e100c_r1
	SND_IF=ens111np0  SND_BDF=0000:dd:00.0  SND_MAIN=91  SND_TX=92  SND_RX=220
	REFL_IF=ens119np0 REFL_BDF=0000:f1:00.0 REFL_MAIN=108 REFL_C0=109
	E100C_MAC=$E100C_MAC1 E100C_PORT=3 B_PORT=1
	;;
x1)  # 交叉路由：ens119（NUMA1）→ ens103（NUMA0），die1 chip1 → die0 chip2；发送端用 id 1 的核，反射端借 id 0 的核，不能和 id 0、id 1 同时跑
	PAIR=e100c_x1
	SND_IF=ens119np0  SND_BDF=0000:f1:00.0  SND_MAIN=65  SND_TX=66  SND_RX=194
	REFL_IF=ens103np0 REFL_BDF=0000:62:00.0 REFL_MAIN=44 REFL_C0=45
	E100C_MAC=$E100C_MAC1 E100C_PORT=0
	;;
xa)  # 换线后用（ens101 接 die0 chip1，ens110 接 die0 chip0）：ens119 → ens101，die1 chip1 → die0 chip1；和 id 0 共用反射端交换芯片、不共用 chip 组
	PAIR=e100c_xa
	SND_IF=ens119np0  SND_BDF=0000:f1:00.0  SND_MAIN=65  SND_TX=66  SND_RX=194
	REFL_IF=ens101np0 REFL_BDF=0000:60:00.0 REFL_MAIN=18 REFL_C0=19
	E100C_MAC=$E100C_MAC1 E100C_PORT=3
	;;
xb)  # 换线后用：ens119 → ens110，die1 chip1 → die0 chip0；和 id 0 只共用 die0 的 chip 0、2 一组，主机侧什么都不共用
	PAIR=e100c_xb
	SND_IF=ens119np0  SND_BDF=0000:f1:00.0  SND_MAIN=65  SND_TX=66  SND_RX=194
	REFL_IF=ens110np0 REFL_BDF=0000:dc:00.0 REFL_MAIN=108 REFL_C0=109
	E100C_MAC=$E100C_MAC1 E100C_PORT=2
	;;
e100c2)  # 两口都在 NUMA1
	PAIR=e100c2
	SND_IF=ens111np0  SND_BDF=0000:dd:00.0  SND_MAIN=88  SND_TX=89  SND_RX=217
	REFL_IF=ens119np0 REFL_BDF=0000:f1:00.0 REFL_MAIN=72 REFL_C0=73
	E100C_MAC=$E100C_MAC2 E100C_PORT=$E100C_PORT2
	;;
d0)  # 直连：ens108 ↔ ens103 用网线对接（不经过 E100c），核同 id 0
	PAIR=direct_d0 DIRECT=1
	SND_IF=ens108np0  SND_BDF=0000:73:00.0  SND_MAIN=27  SND_TX=28  SND_RX=156
	REFL_IF=ens103np0 REFL_BDF=0000:62:00.0 REFL_MAIN=44 REFL_C0=45
	;;
v3)  # z1 的反方向：ens110（die0 chip2）→ port 3 → ens111（die1 chip1）
	PAIR=e100c_v3
	SND_IF=ens110np0  SND_BDF=0000:dc:00.0  SND_MAIN=74  SND_TX=75  SND_RX=203
	REFL_IF=ens111np0 REFL_BDF=0000:dd:00.0 REFL_MAIN=82 REFL_C0=83
	E100C_MAC=$E100C_MAC1 E100C_PORT=3 B_PORT=0
	;;
z1)  # 11-04 接线：ens111（die1）→ port 0 → ens110（die0 chip2）；网卡和核与直连 d1 相同，用来和 d1 对比时延
	PAIR=e100c_z1
	SND_IF=ens111np0  SND_BDF=0000:dd:00.0  SND_MAIN=65  SND_TX=66  SND_RX=194
	REFL_IF=ens110np0 REFL_BDF=0000:dc:00.0 REFL_MAIN=108 REFL_C0=109
	E100C_MAC=$E100C_MAC1 E100C_PORT=0
	;;
d1)  # 直连：ens111 ↔ ens110 用网线对接（不经过 E100c），发送端借 id 1 的核，反射端借 id 3 的核
	PAIR=direct_d1 DIRECT=1
	SND_IF=ens111np0  SND_BDF=0000:dd:00.0  SND_MAIN=65  SND_TX=66  SND_RX=194
	REFL_IF=ens110np0 REFL_BDF=0000:dc:00.0 REFL_MAIN=108 REFL_C0=109
	;;
d2)  # 直连：ens106 ↔ ens101 用网线对接（不经过 E100c），核同 id 2
	PAIR=direct_d2 DIRECT=1
	SND_IF=ens106np0  SND_BDF=0000:71:00.0  SND_MAIN=1   SND_TX=2   SND_RX=130
	REFL_IF=ens101np0 REFL_BDF=0000:60:00.0 REFL_MAIN=18 REFL_C0=19
	;;
direct)  # 发送端 NUMA0，反射端 NUMA1
	PAIR=direct DIRECT=1
	SND_IF=ens102np0  SND_BDF=0000:61:00.0  SND_MAIN=40  SND_TX=41  SND_RX=169
	REFL_IF=ens110np0 REFL_BDF=0000:dc:00.0 REFL_MAIN=80 REFL_C0=81
	;;
*) echo "第一个参数须为 id（0~3）、r0~r3、b1、b2、y0、y3、w2、z1、v3、d0、d1、d2、e100c_r 或 direct；用法看 $0 help" >&2; exit 1 ;;
esac

case "$ID" in
e100c_r1|e100c2|x1|xa|xb)
	echo "对口 $ID 属于旧接线，已停用；请使用当前接线的 0~3、r0~r3、z1 或 v3" >&2; exit 1 ;;
esac
cmd=$1; [ $# -gt 0 ] && shift

# 取出脚本自己的选项，其余留给 axiperf
FLOWS= T= COUNT= REFL_CORES=4 SEP_RX=0 BRIEF=1 TXC=1 HDR=eth ISO_ARG= DRY_RUN=0 rest=()
while [ $# -gt 0 ]; do
	case "$1" in
	-f) FLOWS=$2; shift 2 ;;
	-t) T=$2; shift 2 ;;
	-n) COUNT=$2; shift 2 ;;
	-r) REFL_CORES=$2; shift 2 ;;
	-s) SEP_RX=1; shift ;;
	-v) BRIEF=0; shift ;;
	--txc) TXC=$2; shift 2 ;;
	--sue) HDR=sue; shift ;;
	--iso) ISO_ARG=--isolate; shift ;;
	--dry-run) DRY_RUN=1; shift ;;
	*) rest+=("$1"); shift ;;
	esac
done
set -- "${rest[@]}"
if [ "$DRY_RUN" = 1 ]; then
	case "$cmd" in ping|snd|refl) ;; *) echo "--dry-run 仅支持 ping、snd、refl" >&2; exit 1 ;; esac
fi
case "$ID" in
r0|r1|r2|r3)
	[ "$REFL_CORES" -le 4 ] && [ "${FLOWS:-4}" -le 4 ] && [ "$TXC" = 1 ] || {
		echo "$ID 的并发核布局最多支持 4 流、4 核反射、每流 1 个发送核；id 2 的 8 流双向请使用 b2" >&2; exit 1; }
	;;
esac

# E100c 格式：EtherType 0x88ee + 0000，事务头从第 16 字节开始；多流时响应按 dest_id 低位导流
if [ "$DIRECT" = 1 ]; then NOLEN_ARG=
elif [ "$HDR" = sue ]; then
	FLOWS=${FLOWS:-1}
	NOLEN_ARG="--hdr sue --gpu-id $SUE_SRC --peer-gpu-id $SUE_DST --sue-ethertype $SUE_ET --sue-format $SUE_FMT --sue-pkttype $SUE_PT"
else NOLEN_ARG="--ethertype 0x88ee --min-frame $MIN_FRAME"; fi
# E100c 的 ID 只在收到响应后释放：不做超时回收重发，否则响应一停，发出的请求会超过 511 把 ID 耗尽
TMO_ARG=; [ "$DIRECT" = 1 ] || TMO_ARG="--timeout-us 0"
# 反射端回的写响应 B 的 port id（ETHI1 的 port 0 经 UHUBT1 到 die1 ETHT3）
BPORT_ARG=; [ "$DIRECT" = 1 ] || BPORT_ARG="--b-port $B_PORT --b-tag $B_TAG --r-user $R_USER --b-pack 1"   # OISA 头只有一个 tag_id：每帧 1 个 B
SND_DA=txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0
REFL_DA=dv_flow_en=2,txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0
AXI=$(cd "$(dirname "$0")/.." && pwd)/axiperf
BRIEF_ARG=--brief; [ "$BRIEF" = 0 ] && BRIEF_ARG=

run() { echo "+ $*"; [ "$DRY_RUN" = 1 ] || "$@"; }

need() {
	local miss=0
	if [ "$DIRECT" != 1 ] && [ "$1" = snd ]; then
		[ -n "$E100C_MAC" ]  || { echo "未设 $PAIR 的 E100C_MAC" >&2; miss=1; }
		[ -n "$E100C_PORT" ] || { echo "未设 $PAIR 的 E100C_PORT" >&2; miss=1; }
	fi
	[ "$miss" = 0 ] || { echo "改 $0 开头的值" >&2; exit 1; }
}

# 发送端（$1=op，$2=每流在途，$3=时长，其余传给 axiperf）：上下文 k 的发送线程在 SND_TX+k，接收线程在其超线程 SND_RX+k
sender() {
	local op=$1 w=$2 t=$3 n=$FLOWS k lc dst; shift 3
	need snd
	[ "$op" = rw ] && n=$((FLOWS * 2))
	lc=0@$SND_MAIN
	# --txc=每个上下文的发送核数；-s 时接收线程不用发送核的超线程，放在下一个物理核
	local id=1 cur=$SND_TX txc=$TXC j
	for k in $(seq 0 $((n - 1))); do
		for j in $(seq 1 "$txc"); do lc+=",$id@$cur"; id=$((id + 1)); cur=$((cur + 1)); done
		if [ "$SEP_RX" = 1 ]; then lc+=",$id@$cur"; cur=$((cur + 1)); else lc+=",$id@$((SND_RX + k))"; fi
		id=$((id + 1))
	done
	[ "$txc" -gt 1 ] && set -- --tx-cores "$txc" "$@"
	if [ "$DIRECT" != 1 ]; then dst="--dmac 0,$E100C_MAC --e100c-port 0,$E100C_PORT"
	else dst="--dmac 0,$(cat /sys/class/net/$REFL_IF/address)"; fi
	[ "$E100C_PORT" = 0 ] && dst="${dst/ --e100c-port 0,0/}"   # 不传该参数时 user.port 就是 0
	local rp=; [ "$op" = write ] || rp="--rpack 4"
	[ "$DIRECT" != 1 ] && dst="$dst --smac $E100C_SMAC"
	[ "$HDR" = sue ] && [ "$DIRECT" != 1 ] && { dst=; [ "$E100C_PORT" = 0 ] || dst="--e100c-port 0,$E100C_PORT"; }   # 地址由 GPU ID 决定
	run sudo "$AXI" --file-prefix "axisnd_$PAIR" --huge-unlink --lcores "$lc" -a "$SND_BDF,$SND_DA" -- \
		--mode sender --vid "$VID" --flows $FLOWS --burst 16 --window "$w" --time "$t" $dst --one-dmac $NOLEN_ARG $TMO_ARG --op "$op" $rp $ISO_ARG "$@"
}

case "$cmd" in
setup)
	for i in $SND_IF $REFL_IF; do
		[ -e "/sys/class/net/$i" ] || { echo "跳过 $i：接口不存在"; continue; }
		if [ "$DIRECT" != 1 ]; then   # 内核的 mDNS / DHCP / IPv6 帧也会进 E100c，被当成请求，先让测试口静默（重启后恢复）
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
	"$0" "$ID" check
	;;
check)
	for i in $SND_IF $REFL_IF; do
		[ -e "/sys/class/net/$i" ] || { echo "== $i  接口不存在"; continue; }
		echo "== $i MAC $(cat /sys/class/net/$i/address)  carrier $(cat /sys/class/net/$i/carrier 2>/dev/null)  $(cat /sys/class/net/$i/speed 2>/dev/null) Mb/s  NUMA $(cat /sys/class/net/$i/device/numa_node)  $(basename "$(readlink /sys/class/net/$i/device)")"
		echo "   TC 数 $(sudo mlnx_qos -i "$i" | grep -c '^tc:')；pause $(sudo ethtool -a "$i" | awk '/^RX:|^TX:/{printf "%s %s ", $1, $2}')"
	done
	echo "大页（2MB）：node0 $(cat /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages)  node1 $(cat /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages)"
	;;
refl)
	need refl
	if [ "$HDR" = sue ]; then   # --rx-split 只支持 eth 头：单流单队列；本端 GPU ID 是发送端的目的 GPU ID
		[ "$1" != rw ] || { echo "--sue 的反射端只有单流，不支持 rw" >&2; exit 1; }
		run sudo "$AXI" --file-prefix "axirefl_$PAIR" --huge-unlink -l "$REFL_MAIN,$REFL_C0" -a "$REFL_BDF,$SND_DA" -- \
			--mode reflector --vid "$VID" --flows 1 --rxd 4096 --burst 16 --b-pack 16 $NOLEN_ARG --gpu-id "$SUE_DST" "$@"
		exit
	fi
	case "$REFL_CORES" in 1|2|4|8|16) ;; *) echo "-r 只能是 1、2、4、8、16" >&2; exit 1 ;; esac
	if [ "$REFL_CORES" = 1 ]; then   # 单队列单核：响应严格按收到的顺序发出
		run sudo "$AXI" --file-prefix "axirefl_$PAIR" --huge-unlink -l "$REFL_MAIN,$REFL_C0" -a "$REFL_BDF,$SND_DA" -- \
			--mode reflector --vid "$VID" --flows 1 --rxd 4096 --burst 16 --b-pack 16 $NOLEN_ARG $BPORT_ARG $BRIEF_ARG $ISO_ARG "$@"
		exit
	fi
	cores="$REFL_MAIN,$REFL_C0-$((REFL_C0 + REFL_CORES - 1))"
	if [ -n "$ISO_ARG" ]; then   # --iso 多核：不用 --rx-split，按请求 tag 低位分到 N 个队列
		run sudo "$AXI" --file-prefix "axirefl_$PAIR" --huge-unlink -l "$cores" -a "$REFL_BDF,$SND_DA" -- \
			--mode reflector --vid "$VID" --flows $REFL_CORES --rxd 4096 --burst 16 --b-pack 16 $NOLEN_ARG $BPORT_ARG $BRIEF_ARG $ISO_ARG "$@"
		exit
	fi
	split=$REFL_CORES; rw=
	if [ "$1" = rw ]; then split=$((REFL_CORES / 2)); rw="--op rw"; shift; fi
	run sudo "$AXI" --file-prefix "axirefl_$PAIR" --huge-unlink -l "$cores" -a "$REFL_BDF,$REFL_DA" -- \
		--mode reflector --vid "$VID" --rx-split $split $rw --rxd 4096 --burst 16 --b-pack 16 $NOLEN_ARG $BPORT_ARG $BRIEF_ARG "$@"
	;;
ping)
	# 与 E100c 样例相同：每包 1 个事务、2 拍。-n N：只发 N 帧，窗口也取 N，没有响应也能发满
	FLOWS=${FLOWS:-1}
	if [ -n "$COUNT" ]; then sender write "$COUNT" "${T:-3}" --pack 1 --beats 2 --dump 8 --count "$COUNT" "$@"
	else sender write 4 "${T:-5}" --pack 1 --beats 2 --dump 8 "$@"; fi
	;;
snd)
	op=$1; w=$2
	case "$op" in write|read|rw|mix) ;; *) echo "用法: $0 <id> snd write|read|rw|mix <每流在途>" >&2; exit 1 ;; esac
	[ -n "$w" ] || { echo "用法: $0 <id> snd write|read|rw|mix <每流在途>" >&2; exit 1; }
	# 一个请求包占多个 ID（写 --pack 2，读 --rpack 4），每流在途小于它时一帧也发不出去
	min=2; [ "$op" = write ] || min=4
	[ "$w" -ge "$min" ] || { echo "$op 的每流在途至少为 $min，否则发不出包" >&2; exit 1; }
	shift 2
	FLOWS=${FLOWS:-4}
	sender "$op" "$w" "${T:-20}" $BRIEF_ARG "$@"
	;;
cnt)
	case "$1" in snd) i=$SND_IF ;; refl) i=$REFL_IF ;; *) echo "用法: $0 <id> cnt snd|refl" >&2; exit 1 ;; esac
	exec watch -n1 "sudo ethtool -S $i | grep -E '^ *(tx_packets_phy|rx_packets_phy|rx_discards_phy|rx_crc_errors_phy|rx_out_of_buffer):|(rx|tx)_prio[0-7]_(packets|pause):' | grep -v ': 0$'"
	;;
cap)
	run sudo tcpdump -i "$REFL_IF" -e -nn -c "${1:-200}" -w "/tmp/refl_in_$PAIR.pcap"
	echo "查看: sudo tcpdump -r /tmp/refl_in_$PAIR.pcap -e -nn -xx | head -80"
	;;
-h|--help|help|"")
	usage
	;;
*)
	echo "未知命令 $cmd；用法看 $0 help" >&2; exit 1
	;;
esac

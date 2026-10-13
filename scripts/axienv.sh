# axiperf 测试环境：两端都 source 本文件，用预设名选 devargs，命令由函数拼出并先打印。
#   反射端: refl <预设> [额外参数]        例: refl SINGLE
#   发送端: snd  <预设> <窗口> [额外参数]  例: snd SINGLE 511
#   改核/时长: SL=23,24-27 snd ... / RL=23,24,25 refl ... / T=30 snd ...
#   单流 2 发送核: SL=23,24-26 snd SINGLE 511 --tx-cores 2（每条流 1 + N 个核）
#
# 多流（不带 --hwts：它会让驱动关 CQE 压缩，多流包率掉到约 45 Mpps）：
#   反射端: reflm <卡数 1|2> [额外参数]   例: reflm 1 / reflm 1 --op rw / reflm 2
#   发送端: sndm  <卡数 1|2> [额外参数]   例: sndm 1 --op write / sndm 1 --op read --rpack 4 / sndm 2 --op write
#   卡数 1 = 33:00.0；2 = 12:00.0 + 33:00.0。每块网卡 F 条流（默认 8），--op rw 时每条流写、读 2 个上下文。
#   核自动分配：发送端上下文 k 的发送线程在 CPU 24+k、接收线程在其超线程 104+k；反射端每个上下文 1 核（24 起）。
#   上下文最多 16 个（CPU 24-39）。两端的卡数、F、--op rw 必须一致。
#
# 多源 MAC → 单一目的 MAC（模拟经 E100c 后反射端只有一对 MAC）：
#   反射端: refl SINGLE                                  （--flows 1：一个接收队列、1 个核）
#           RL=23,24,25 refl SINGLE --op rw              （读写并发：写、读按 PCP 分两个队列）
#   发送端: sndm 1 --op write --one-dmac --window 64     （8 流 × 64 ≈ E100c 共享的 511 个 ID）
#   发送端各流 SMAC 仍不同，请求的 DMAC 都是反射端网卡 MAC；反射端交换 MAC 回给各流，
#   写响应只合并同一源 MAC 的请求。
#
# 同上，反射端由网卡按随机值把这一对 MAC 的请求均分到 N 个核（--rx-split，HW steering）：
#   反射端: refls <N> [额外参数]                         例: refls 4 / refls 8 / refls 4 --op rw（写、读各 N 核）
#   发送端: sndm 1 --op write --one-dmac --window 64
#   反射端用 SPLIT 预设（多流 devargs + dv_flow_en=2；关 CQE 压缩时网卡只能收约 46 Mpps；不开 MPRQ 时 RTT 约高 2.9 µs），
#   接收环 --rxd 4096（RXD= 可改；1024 + MPRQ 每队列只有 2 个 WQE，满载会 imissed）。
#   10-13 实测：refls 8、8 流 × 511 在途写 75.8 Mpps / 343 G，无丢包，各核约 72%。
#
# E100c 等效（总在途 512 ≈ 511 个共享 ID，包率约 20–30 Mpps）：两端都用低时延 devargs（关 CQE 压缩、关 MPRQ），
# 吞吐 = 512 ÷ RTT，RTT 每少 1 µs 吞吐约多 8%。10-13 实测（带 --hwts）：写 p50 11.5 µs / 双向 106 G，
# 读 11.2 µs / 104.5 G，读写并发 写 14.8、读 12.85 µs / 169.9 G。
#   反射端: D=LAT refls 8                      （读写并发：D=LAT refls 4 --op rw）
#   发送端: D=SINGLE sndm 1 --op write --one-dmac --window 64      （读：--op read --rpack 4；读写并发：--op rw --rpack 4）
#   不限在途、压反射端吞吐时改回默认（反射端 SPLIT、发送端 MULTI）：关压缩时网卡约 46 Mpps 即丢包。
BDF=${BDF:-0000:33:00.0}
PEER=${PEER:-0,a0:88:c2:76:cc:0c}
BDF12=${BDF12:-0000:12:00.0}                 # 多流双卡用
MAC12=${MAC12:-a0:88:c2:76:bb:6c}            # 反射端 12:00.0 的 MAC
MAC33=${MAC33:-a0:88:c2:76:cc:0c}            # 反射端 33:00.0 的 MAC
VID=${VID:-1}
AXI=${AXI:-./axiperf}

# ---- devargs 预设（只在这里改）----
DA_SINGLE="txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0"                                                  # 单流基线，无 MPRQ
DA_A="txq_inline_mpw=1024,txq_inline_max=1024,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0"                                  # 实验 A：全内联
DA_B="txq_inline_mpw=128,txq_mpw_en=0,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0"                                          # 实验 B：关 eMPW
DA_MULTI="mprq_en=1,rxqs_min_mprq=1,mprq_log_stride_num=9,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=1"   # 多流

DA_SPLIT="dv_flow_en=2,$DA_MULTI"
DA_LAT="dv_flow_en=2,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0"                                    # 反射端 --rx-split 低时延（≤ 约 40 Mpps）                                                                                    # 反射端 --rx-split

da() { local v="DA_$1"; [ -n "$1" ] && [ -n "${!v}" ] || { echo "未知预设: '$1'（可用: SINGLE A B MULTI SPLIT LAT）" >&2; return 1; }; echo "${!v}"; }

refl() { local d; d=$(da "$1") || return 1; shift
  local c="$AXI -l ${RL:-23,24} -a $BDF,$d -- --mode reflector --vid $VID --b-pack 16 --hwts $*"
  echo "+ $c"; $c; }

snd() { local d; d=$(da "$1") || return 1; local w=$2; [ -n "$w" ] || { echo "用法: snd <预设> <窗口> [额外参数]" >&2; return 1; }; shift 2
  local c="$AXI -l ${SL:-23,24,25} -a $BDF,$d -- --mode sender --vid $VID --window $w --time ${T:-20} --hwts --dmac $PEER $*"
  echo "+ $c"; $c; }

# ---- 多流 ----
# 上下文数 = 卡数 × F × (--op rw ? 2 : 1)
nctx() { local n=$(( $1 * ${F:-8} )); case " ${*:2} " in *" --op rw "*) n=$((n * 2));; esac; echo $n; }

# -a 参数：卡数 1 → 33:00.0；2 → 12:00.0 + 33:00.0（DPDK 按 PCI 地址编号，12:00.0 为 port 0）
mdev() { local d; d=$(da "${D:-MULTI}") || return 1
  case "$1" in 1) echo "-a $BDF,$d";; 2) echo "-a $BDF12,$d -a $BDF,$d";; *) echo "卡数须为 1 或 2" >&2; return 1;; esac; }

reflm() { local nic=$1; shift; local dev n; dev=$(mdev "$nic") || return 1; n=$(nctx "$nic" "$@")
  [ "$n" -le 16 ] || { echo "上下文 $n 个超过 16" >&2; return 1; }
  local c="$AXI --huge-unlink -l 23,24-$((23 + n)) $dev -- --mode reflector --vid $VID --flows ${F:-8} --burst 16 --b-pack 16 $*"
  echo "+ $c"; $c; }

# 反射端 --rx-split：一对 MAC（33:00.0），N 个上下文（--op rw 时 2N），每个 1 核，从 CPU 24 起
refls() { local n=$1; shift; [ -n "$n" ] || { echo "用法: refls <N> [额外参数]" >&2; return 1; }
  local k=$n d; case " $* " in *" --op rw "*) k=$((n * 2));; esac
  [ "$k" -le 16 ] || { echo "上下文 $k 个超过 16" >&2; return 1; }
  d=$(da "${D:-SPLIT}") || return 1
  local c="$AXI --huge-unlink -l 23,24-$((23 + k)) -a $BDF,$d -- --mode reflector --vid $VID --rx-split $n --rxd ${RXD:-4096} --burst 16 --b-pack 16 $*"
  echo "+ $c"; $c; }

sndm() { local nic=$1; shift; local dev n lc k mac; dev=$(mdev "$nic") || return 1; n=$(nctx "$nic" "$@")
  [ "$n" -le 16 ] || { echo "上下文 $n 个超过 16" >&2; return 1; }
  lc=0@23; for k in $(seq 0 $((n - 1))); do lc+=",$((2*k+1))@$((24+k)),$((2*k+2))@$((104+k))"; done
  if [ "$nic" = 1 ]; then mac="--dmac 0,$MAC33"; else mac="--dmac 0,$MAC12 --dmac 1,$MAC33"; fi
  local c="$AXI --huge-unlink --lcores $lc $dev -- --mode sender --vid $VID --flows ${F:-8} --burst 16 --window 511 --time ${T:-20} $mac $*"
  echo "+ $c"; $c; }

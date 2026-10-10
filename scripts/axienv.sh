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

da() { local v="DA_$1"; [ -n "$1" ] && [ -n "${!v}" ] || { echo "未知预设: '$1'（可用: SINGLE A B MULTI）" >&2; return 1; }; echo "${!v}"; }

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

sndm() { local nic=$1; shift; local dev n lc k mac; dev=$(mdev "$nic") || return 1; n=$(nctx "$nic" "$@")
  [ "$n" -le 16 ] || { echo "上下文 $n 个超过 16" >&2; return 1; }
  lc=0@23; for k in $(seq 0 $((n - 1))); do lc+=",$((2*k+1))@$((24+k)),$((2*k+2))@$((104+k))"; done
  if [ "$nic" = 1 ]; then mac="--dmac 0,$MAC33"; else mac="--dmac 0,$MAC12 --dmac 1,$MAC33"; fi
  local c="$AXI --huge-unlink --lcores $lc $dev -- --mode sender --vid $VID --flows ${F:-8} --burst 16 --window 511 --time ${T:-20} $mac $*"
  echo "+ $c"; $c; }

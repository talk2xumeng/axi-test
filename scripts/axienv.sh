# axiperf 测试环境：两端都 source 本文件，用预设名选 devargs，命令由函数拼出并先打印。
#   反射端: refl <预设> [额外参数]        例: refl SINGLE
#   发送端: snd  <预设> <窗口> [额外参数]  例: snd SINGLE 511
#   改核/时长: SL=23,24-27 snd ... / RL=23,24,25 refl ... / T=30 snd ...
BDF=${BDF:-0000:33:00.0}
PEER=${PEER:-0,a0:88:c2:76:cc:0c}
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

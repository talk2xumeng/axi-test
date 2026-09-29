# axiperf —— AXI over Ethernet 性能验证程序

一个程序两种模式：`sender`（发请求、收响应、统计带宽与 RTT）和 `reflector`（收请求、回响应）。
每个 DPDK port 可承载多条流（`--flows`），每条流占一对队列、一个独立的 ID 空间。发送端默认每条流 2 个核（TX 核 + RX 核，共享在途 ID 表），反射端每条流 1 个核；另需 1 个主核做统计输出。
`-l` 的第一个核是统计核，其余按流分配：每条流（分核模式先 TX 后 RX）取与其网卡同一 NUMA 节点、编号最小的空闲核，不够时借用其它节点的核并告警；启动时逐条打印 `flow p.f -> lcore n`。每条流一条 rte_flow 规则按接收帧的 DMAC 字段导到对应队列；eth 头默认不开混杂，其余单播由网卡丢弃，软件再核对一次 DMAC。
多网卡：`-a` 给多个口，`--flows` 为每口流数，流与核按"口 0 的各流、口 1 的各流…"顺序展开；DPDK 端口号按 PCI 地址顺序，与 `-a` 书写顺序无关。

以太头格式可选（`--hdr`），AXI 事务、窗口、统计、抓包与头格式无关：

| `--hdr` | 帧格式 | 事务边界 | 状态 |
|---|---|---|---|
| `eth`（默认） | `DMAC SMAC 0x8100 PCP/DEI/VID Length \| AXI 事务 × k` | 按 Length 截断 | 当前测试格式 |
| `sue` | `DstGPU(2)+Rsv(4) SrcGPU(2)+Rsv(4) 0x8100 OPCP/OCFI/OVID EtherType RSV/PackNum/Format/PktType \| AXI 事务 × k` | 按 PackNum 解析 | 试验性，字段取值待与 IP 侧确认 |

两种格式 PCP = VC，头部字段 32 bit 大端，接收端按 frame_type 分发、PCP 只做核对。

## 代码结构

```
src/
  common.h       常量、运行配置、每流上下文、统计结构
  hdr.h / hdr.c  以太头抽象层：build / parse / reply 按 --hdr 分发（inline，无函数指针开销）
  hdr_eth.h      eth 头：802.1Q + 802.3 Length
  hdr_sue.h      sue 头：SUEv1-标准ETH（试验性）
  axi.h          AXI 事务编码与解析（与头格式无关）
  sender.c       发请求、收响应、窗口、RTT
  reflector.c    收请求、回响应（写 1:1 原地改写；读按 ≤8 拍拆包）
  port.c         端口、队列、mempool、预填模板、导流
  stats.c        每秒统计输出
  capture.c      异常帧打印、--dump、--pcap
  main.c         参数、核分配、主循环
```

新增一种头格式：新建 `src/hdr_xxx.h` 实现 `xxx_build / xxx_parse / xxx_update`，在 `enum hdr_kind` 加一项，在 `hdr.h` 的各 switch 与 `hdr.c`（名称、参数校验、流地址推导）中各加一个 case，其余文件不用改。

## 编译

```bash
# 使用 DOCA 自带的 DPDK（/opt/mellanox/dpdk，22.11.2410），pkg-config 可直接找到
make
# 若运行时报找不到 .so：
echo /opt/mellanox/dpdk/lib/x86_64-linux-gnu > /etc/ld.so.conf.d/dpdk.conf && ldconfig
```

## 运行：eth 头（单个 400G 口，多条流以 MAC 区分）

```bash
# 反射端（先启动、常开）：35 统计，36/37 各负责一条流
./axiperf -l 35,36,37 -a 0000:12:00.0 -- --mode reflector --vid 1 --flows 2

# 发送端：写测试，35 统计，36/37 流 0 的 TX/RX，38/39 流 1 的 TX/RX
./axiperf -l 35-39 -a 0000:12:00.0 -- --mode sender --op write --vid 1 --flows 2 \
    --time 10 --window 511 --dmac 0,<反射端网卡真实 MAC>

# 发送端：读测试（每包 2 个读）
./axiperf -l 35-39 -a 0000:12:00.0 -- --mode sender --op read --pack 2 --vid 1 --flows 2 \
    --time 10 --dmac 0,<反射端网卡真实 MAC>
```

- 两端 `--hdr`、`--flows`、`--vid` 必须一致；`--dmac` 只需给出反射端网卡真实 MAC，其余流的 MAC 自动派生。
- 流 f（f ≥ 1）的 MAC = 真实 MAC 首字节置本地管理位（清组播位）、末字节 +f；启动时逐流打印。
- 收发核应为 `isolcpus` 隔离核，且与网卡同一 NUMA 节点；DPDK 可用 lcore 编号须 < 128。
- 建议 EAL 加 `--huge-unlink`：进程被杀后不残留大页文件。Ctrl-C 一次正常退出，卡住时再按一次强制退出。
- 抓包：`--pcap /tmp/x.pcap --pcap-count 50` 配合小窗口（如 `--window 4`），性能测试时不要开。

## 运行：多网卡

```bash
# 两口各 8 条流，单核收发（16 个工作核 + 1 个统计核）
./axiperf -l 23,24-39 -a 0000:12:00.0,$DEV -a 0000:33:00.0,$DEV -- --mode reflector --vid 1 --flows 8 --burst 16
./axiperf -l 23,24-39 -a 0000:12:00.0,$DEV -a 0000:33:00.0,$DEV -- --mode sender --op write --vid 1 \
    --flows 8 --nosplit --burst 16 --time 10 --dmac 0,<对端口 0 MAC> --dmac 1,<对端口 1 MAC>
```

- 程序按网卡 NUMA 节点自动选核，`-l` 里给出各节点足够的隔离核即可；DPDK lcore 编号须 < 128，编号更大的 CPU 用 `--lcores` 映射。
- 同一台机器的多个口接同一交换机同一 VLAN 时，交换机泛洪的帧会同时到达本机其它口。非混杂模式下由网卡丢弃；若 `xmac` 持续增长，说明仍有泛洪副本进来（已丢弃，不影响结果）。

## 运行：sue 头（试验性）

```bash
# 反射端：本端 GPU ID 基址 0x20，流 k 用 0x20+k
./axiperf -l 35,36 -a 0000:12:00.0 -- --mode reflector --hdr sue --gpu-id 0x20 --vid 1

# 发送端：本端 0x10，对端 0x20
./axiperf -l 35-37 -a 0000:12:00.0 -- --mode sender --hdr sue --gpu-id 0x10 --peer-gpu-id 0x20 \
    --op write --vid 1 --time 10
```

- GPU ID 写在 MAC 字段的前 2 字节（其余保留字段填 0），流 k 用"基址 + k"。响应由反射端交换两个 GPU ID。
- GPU ID 高字节即 MAC 首字节，其最低位是组播位：置 1 时网卡 / 交换机会当作组播，程序会告警。
- EtherType / Format / PktType 尚未确定，默认 0x88B5 / 0 / 0，可用 `--sue-ethertype` 等参数改。
- sue 头的 MAC 不是网卡地址。过交换机时，交换机按这些"MAC"学习与转发；直连 E100c 时由 E100c 按 GPU ID 转发。

测试拓扑、用例与阶段性结果见 `docs/AXI环回测试_拓扑与用例_v0.8.md`。

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--mode` | sender | sender / reflector |
| `--op` | write | sender 用：write / read / mix（偶数号流写、奇数号流读）/ rw（同一 MAC 上同时读写：写、读各一个上下文，各自 511 窗口、队列与核，接收按 PCP=VC 导流；**反射端也要加 `--op rw`**，两端 `--flows` 相同）|
| `--window` | 511 | 每条流在途事务上限（1~511） |
| `--pack` | 2 | 每个请求包的事务数（写：pack × beats ≤ 8；读：≤ 4） |
| `--rpack` | 同 `--pack` | 读请求每包事务数（1~4）；mix / rw 时读、写的打包可分别设置 |
| `--beats` | 4 | 每事务拍数（awlen/arlen + 1，1~4） |
| `--burst` | 32 | 每次 tx/rx burst 包数（≤ 64）；反射端每攒够 burst 个响应即发出 |
| `--flows` | 1 | 每端口流数（1~16），每条流独立地址、队列对、ID 空间 |
| `--nosplit` | — | 发送端改为每条流单核收发 |
| `--time` | 0 | 运行秒数，0 为直到 Ctrl-C |
| `--drop-every` | 0 | reflector：每 N 个请求包丢 1 个，用于验证丢包处理 |
| `--b-pack` | 1 | reflector：同一批收到的写请求，其 B 合并进一个响应包，每包最多 N 个（1~16，VC2 上限 16），减少写方向反射端发包数与发送端收包数 |
| `--txq` | 1 | 每个上下文的发送队列数（1~4）。接收仍是一个队列，同一核把每批包均分到 N 个发送队列（起始队列每批轮转），用于测单队列排队对 RTT 的影响；两端可分别设置。多个发送队列在线上可能乱序（不同 ID 之间） |
| `--timeout-us` | 10000 | sender：事务超时回收并计入 lost；0 为不回收（丢一个包该流就会停住） |
| `--dump` | 0 | 打印前 N 个收 / 发的 AXI 帧十六进制 |
| `--pcap` | — | 抓包模式：把收 / 发的 AXI 帧写入 pcap 文件（纳秒时间戳）；有锁和文件写入，性能会下降 |
| `--pcap-count` | 1000 | 抓包模式最多写入的帧数 |
| `--hdr` | eth | 以太头格式：eth / sue |
| `--vid` | 0 | 802.1Q VID（过交换机时填交换机上的 VLAN） |
| `--promisc` | — | 强制混杂模式（默认 eth 头只收本端各流 MAC） |
| `--dmac` | — | eth：`PORT,xx:xx:xx:xx:xx:xx`，对端网卡真实 MAC，可重复 |
| `--gpu-id` | — | sue：本端 GPU ID 基址（必填） |
| `--peer-gpu-id` | — | sue：sender 的对端 GPU ID 基址（必填） |
| `--sue-ethertype` | 0x88B5 | sue：EtherType（须 > 1500） |
| `--sue-format` | 0 | sue：Format[2:0] |
| `--sue-pkttype` | 0 | sue：PktType[4:0] |

## 输出（每秒一行 / 每条流）

| 列 | 含义 |
|---|---|
| txMpps / rxMpps | 发 / 收包速率 |
| txWireG / rxWireG | 线上速率 = (帧长，不足 60 补 60) + FCS 4 + 前导码/IFG 20，单位 Gbps |
| txnM/s | 完成事务数（百万/秒，仅 sender） |
| dataG | AXI 数据 Gbps（wdata / rdata，仅 sender） |
| meanus / p50us / p99us / p999us / maxus | RTT（累计）；0~200 µs 为 50 ns 一档，200 µs~100 ms 为 100 µs 一档 |
| slow | RTT > 200 µs 的事务数 |
| busyT% / busyR% | 发送端 TX 核 / RX 核（反射端、单核模式只看 busyR%）忙碌占比，接近 100% 即该核为瓶颈 |
| cyc/pk | 每个包（收、发各算一次）消耗的 CPU 周期 |
| rxB | 每次非空 rx_burst 平均包数，接近 64 表示该端接收有积压 |
| err | ID 异常（重复响应、超时回收后迟到）、头或 payload 非法等（前 5 个异常帧会打印原因与十六进制） |
| lost | 超时（`--timeout-us`）未收到响应、被回收的事务数，即丢失 |
| ign / xmac / pcpx | 非 AXI 背景帧数 / DMAC 不是本流的 AXI 帧数（交换机泛洪副本等，已丢弃）/ PCP 与 VC 不一致的帧数 |
| imissed / nombuf | 网卡侧丢包（描述符 / mbuf 不足） |

理论值（每 100G）：写请求 21.3 Mpps / 数据 87.4G；读响应 22.2 Mpps / 数据 91.1G（eth 头；sue 头每帧多 2B，略低）。

## 建议调优（参考 NVIDIA DPDK 性能报告）

```bash
# devargs
-a <BDF>,mprq_en=1,rxqs_min_mprq=1,mprq_log_stride_num=9,txq_inline_mpw=128,rxq_pkt_pad_en=1
# 网卡：CQE 压缩保持关闭（开启实测吞吐下降约 14%）
mlxconfig -d <BDF> set CQE_COMPRESSION=0
# PCIe MaxReadReq 改为 4096（重启 / 网卡复位后恢复 256，需重设；须在 axiperf 停止时改）
cur=$(setpci -s <BDF> CAP_EXP+8.w); setpci -s <BDF> CAP_EXP+8.w=$(printf %04x $(( (0x$cur & 0x8fff) | (5 << 12) )))
# 内核参数：isolcpus / nohz_full / rcu_nocbs 覆盖收发核；收发核、大页与网卡在同一 NUMA 节点；停 irqbalance
```

## 已知限制

- 不做 PFC、不重传；超时事务只回收 ID 并计入 lost。9 bit ID 无法区分迟到响应与复用后的新响应，迟到响应计入 err。
- 启动时每条流发 4 个广播帧（EtherType 0x9000）让交换机学习地址，对端计入 ign。
- 读响应 rdata 为模板固定内容。
- RTT 为累计直方图，不按秒清零。
- eth 头默认非混杂；`--promisc` 可强制打开。sue 头仍用混杂 + allmulticast，靠软件核对 DMAC。
- eth 头已在 DOCA DPDK 22.11.2410 + CX7 实机验证（两网卡 16 条流：写 737G / 读 687G 线上速率，err = 0）；sue 头仅在 memif 上做过功能验证。

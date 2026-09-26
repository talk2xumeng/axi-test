# axiperf —— AXI over Ethernet 最小性能验证程序

一个程序两种模式：`sender`（发请求、收响应、统计带宽与 RTT）和 `reflector`（收请求、回响应）。
每个 DPDK port 可承载多条流（`--flows`），每条流占一对队列。发送端默认每条流 2 个核（TX 核 + RX 核，共享在途 ID 表），反射端每条流 1 个核；另需 1 个主核做统计输出。
`-l` 的第一个核是统计核，之后按端口顺序分配：发送端流 k（按端口、流序展开）用第 2k+1、2k+2 个核（先 TX 后 RX），反射端流 k 用第 k+1 个核。多流时靠 rte_flow 按 DMAC 把响应导到对应队列。

帧格式按文档 v0.5：`DMAC SMAC 0x8100 PCP/DEI/VID Length | AXI 事务 × k`，PCP = VC，头部字段 32bit 大端。
MAC 暂不处理：端口开混杂模式，DMAC 可用 `--dmac` 指定（默认 02:00:00:00:01:0<port>）。

## 编译

```bash
# 使用 DOCA 自带的 DPDK（/opt/mellanox/dpdk，22.11.2410），pkg-config 可直接找到
make
# 若运行时报找不到 .so：
echo /opt/mellanox/dpdk/lib/x86_64-linux-gnu > /etc/ld.so.conf.d/dpdk.conf && ldconfig
```

## 运行（单个 400G 口，多条流以 MAC 区分）

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

- 两端 `--flows`、`--vid` 必须一致；`--dmac` 只需给出反射端网卡真实 MAC，其余流的 MAC 自动派生。
- 流 f（f ≥ 1）的 MAC = 真实 MAC 首字节置本地管理位（清组播位）、末字节 +f；启动时逐流打印。
- 收发核应为 `isolcpus` 隔离核，且与网卡同一 NUMA 节点；DPDK 可用 lcore 编号须 < 128。
- 抓包：`--pcap /tmp/x.pcap --pcap-count 50` 配合小窗口（如 `--window 4`），性能测试时不要开。

测试拓扑、用例与阶段性结果见 `docs/AXI环回测试_拓扑与用例_v0.6.md`。

| 参数 | 默认 | 说明 |
|---|---|---|
| `--mode` | sender | sender / reflector |
| `--op` | write | sender 用：write / read |
| `--window` | 511 | 每条流在途事务上限（1~511） |
| `--pack` | 2 | 每个请求包的事务数（写：pack × beats ≤ 8；读：≤ 4） |
| `--beats` | 4 | 每事务拍数（awlen/arlen + 1，1~4） |
| `--burst` | 32 | 每次 tx/rx burst 包数（≤ 64） |
| `--time` | 0 | 运行秒数，0 为直到 Ctrl-C |
| `--dmac` | — | `PORT,xx:xx:xx:xx:xx:xx`，可重复 |
| `--vid` | 0 | VLAN ID（过交换机时填交换机上的 VLAN） |
| `--nosplit` | — | 发送端改为每条流单核收发 |
| `--flows` | 1 | 每端口流数（1~16）。每条流独立 MAC、队列对、ID 空间；流 0 用网卡真实 MAC，流 f>0 的 MAC = 真实 MAC 首字节置本地管理位、末字节 +f，两端按同一规则推导 |
| `--dump` | 0 | 打印前 N 个收 / 发的 AXI 帧十六进制 |
| `--pcap` | — | 抓包模式：把收 / 发的 AXI 帧写入 pcap 文件（纳秒时间戳，tcpdump / Wireshark 可直接打开）；有锁和文件写入，性能会下降 |
| `--pcap-count` | 1000 | 抓包模式最多写入的帧数 |

## 输出（每秒一行 / 每端口）

| 列 | 含义 |
|---|---|
| txMpps / rxMpps | 发 / 收包速率 |
| txWireG / rxWireG | 线上速率 = (帧长，不足 60 补 60) + FCS 4 + 前导码/IFG 20，单位 Gbps；100.0 即打满 |
| txn/sM | 完成事务数（百万/秒，仅 sender） |
| dataG | AXI 数据 Gbps（wdata / rdata，仅 sender） |
| p50/p99/p999/max us | RTT（累计），按 burst 取 TSC，50 ns 一档，上限 200 us |
| busyT% / busyR% | 发送端 TX 核 / RX 核（反射端、单核模式只看 busyR%）忙碌占比，接近 100% 即该核为瓶颈 |
| cyc/pk | 每个包（收、发各算一次）消耗的 CPU 周期 |
| rxB | 每次非空 rx_burst 平均包数，接近 64 表示该端接收有积压 |
| meanus / slow | 平均 RTT / RTT > 200 us 的事务数 |
| err | ID 异常、解析错误等 |
| ign / pcpx | 非 AXI 背景帧数 / PCP 与 VC 不一致的帧数 |
| imissed / nombuf | 网卡侧丢包（描述符 / mbuf 不足） |

理论值（每条流 100G）：写请求 21.3 Mpps / 线上 100G / 数据 87.4G；读响应 22.2 Mpps / 线上 100G / 数据 91.1G。

## 建议调优（参考 NVIDIA DPDK 性能报告）

```bash
# devargs
-a <BDF>,mprq_en=1,rxqs_min_mprq=1,mprq_log_stride_num=9,txq_inline_mpw=128,rxq_pkt_pad_en=1
# 网卡
mlxconfig -d <BDF> set CQE_COMPRESSION=1
# 内核参数：isolcpus / nohz_full / rcu_nocbs 覆盖收发核，1G 大页，关闭深度 C-state
# 收发核、大页与网卡在同一 NUMA 节点；停 irqbalance
```

## 已知限制（快速验证版）

- 不做 PFC / 超时处理；ID 超时不回收。
- 读响应 rdata 为模板固定内容。
- RTT 为累计直方图，不按秒清零。
- 已在 DPDK 23.11 编译、memif 功能自测，并在 DOCA DPDK 22.11.2410 + CX7 实机验证（2 条流合计 109G，err = 0）。

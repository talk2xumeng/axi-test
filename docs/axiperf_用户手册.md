# axiperf 用户手册

本文说明怎样在一台新服务器上编译、准备和运行 `axiperf`，以及测试拓扑、程序原理和全部参数。不针对某一台被测设备；对接具体设备时的固定参数和脚本，见该设备自己的测试文档。

## 目录

1. [axiperf 是什么](#1-axiperf-是什么)
2. [环境要求](#2-环境要求)
3. [编译](#3-编译)
4. [运行前的准备](#4-运行前的准备)
5. [测试拓扑](#5-测试拓扑)
6. [工作原理](#6-工作原理)
7. [必须设对的参数](#7-必须设对的参数)
8. [运行示例](#8-运行示例)
9. [参数](#9-参数)
10. [输出](#10-输出)
11. [调优](#11-调优)
12. [注意事项和常见问题](#12-注意事项和常见问题)

第一次上手按 2 → 3 → 4 → 7 → 8 的顺序看；第 5、6 章讲原理，第 9 章是参数手册，第 10~12 章用于看结果和排查问题。

---

## 1. axiperf 是什么

`axiperf` 是基于 DPDK 的 AXI over Ethernet 流量程序：把 AXI 写（AW + W）、读（AR）请求封装进以太网帧发出，对端回写响应（B）、读响应（R + 数据），用来测吞吐、事务往返时延（RTT）和错误。不走 IP / ARP，按二层 MAC 寻址。

同一个程序两种角色：

| 角色 | 做什么 |
|---|---|
| `sender`（发送端） | 按窗口发请求，收响应，统计吞吐、RTT 分布、丢失和错误 |
| `reflector`（反射端） | 收请求，原地改写成响应发回；写请求回 B，读请求回 R + 数据 |

被测对象（交换芯片、桥接芯片等）放在两者之间；没有被测对象时两端直连，用来测主机和网卡本身的基线。

---

## 2. 环境要求

### 2.1 硬件

| 项 | 要求 |
|---|---|
| 网卡 | NVIDIA / Mellanox ConnectX（mlx5 驱动）。程序用了 mlx5 的 rte_flow（按 MAC 导流、`--rx-split` 的随机值匹配、`--isolate`），其它网卡未测 |
| PCIe | 网卡插在 x16 槽，链路协商为 x16（宽度不够时带宽被限制） |
| CPU | 每条流 1~2 个专用核，另加 1 个统计核；核要和网卡在同一 NUMA 节点，最好是隔离核 |
| 内存 | 每个用到的 NUMA 节点配大页，参考 4096 个 2MB 页（8GB） |

### 2.2 软件

| 项 | 要求 |
|---|---|
| 操作系统 | Linux x86_64 |
| 网卡驱动 | 内核 `mlx5_core` + rdma-core（`libibverbs`、`libmlx5`）。mlx5 是分叉驱动，**网卡保持绑定 mlx5_core，不要绑 vfio-pci** |
| DPDK | 24.11，或 DOCA 自带的 22.11。需带 mlx5 PMD 和 pdump 库 |
| 编译工具 | gcc、make、pkg-config；`libbsd`、`libnl-3`、`libnl-route-3`（DPDK pkg-config 会带上） |
| 抓包解析（可选） | Wireshark，插件在 `wireshark/` |

**`--rx-split` 需要 DPDK 带 `RTE_FLOW_ITEM_TYPE_RANDOM`**：DOCA 22.11 和上游 24.03 起有，Ubuntu 24.04 自带的 23.11 没有。编译时自动检测，没有时其它功能照常，只有 `--rx-split` 报错。

---

## 3. 编译

### 3.1 普通编译

```bash
git clone <仓库地址> axi-test && cd axi-test
make -j$(nproc)
./axiperf --help          # 不需要 root、不碰网卡，能打印帮助就说明编译、链接都正常
```

Makefile 用 `pkg-config libdpdk` 找 DPDK。DPDK 不在系统默认路径时，指定它的 pkgconfig 目录：

```bash
export PKG_CONFIG_PATH=<DPDK 安装目录>/lib/x86_64-linux-gnu/pkgconfig
make -j$(nproc)
```

运行时报找不到 `librte_*.so` 时，把库目录加进动态库搜索路径：

```bash
echo <DPDK 安装目录>/lib/x86_64-linux-gnu | sudo tee /etc/ld.so.conf.d/dpdk.conf
sudo ldconfig
```

检查编译结果：

- 编译命令里有 `-DHAVE_FLOW_RANDOM` 说明 DPDK 带 RANDOM，`--rx-split` 可用。
- `ldd axiperf | grep rte_eal` 确认链接的是想要的那套 DPDK。

### 3.2 附带工具（可选）

```bash
make rndq       # 验证网卡能否按 RANDOM 把同一对 MAC 的包分到多个队列（--rx-split 的前提）
make pingpong   # 最小 DPDK 乒乓时延探针：单队列、无 rte_flow，用来量主机 + 网卡本身的时延下限
```

### 3.3 单文件静态版本（验证中）

目标是编出一个不依赖 DPDK / rdma-core `.so` 的 `axiperf-static`，拷到新服务器上直接运行，不用在每台机器上装 DPDK。做法是用脚本从源码构建固定版本（DPDK 24.11.4、rdma-core 50.0）的静态工具链，再 `make static`。

这个功能还在验证，正式发布前请用 3.1 节的普通编译。

静态版本仍需要运行机器有 mlx5 内核驱动、大页和网卡，这些不能打进二进制。

---

## 4. 运行前的准备

以下设置大多在**重启后失效**，建议写成开机后执行的脚本。

### 4.1 一次性配置（改内核参数，需要重启）

**隔离核。** 收发核要独占，不被内核调度和中断打扰。在 `/etc/default/grub` 的 `GRUB_CMDLINE_LINUX` 里加：

```text
isolcpus=managed_irq,domain,<工作核列表> nohz_full=<工作核列表> rcu_nocbs=<工作核列表>
```

每个 NUMA 节点留 1~2 个核（及其超线程）给系统和中断，其余隔离。`sudo update-grub` 后重启，用 `cat /sys/devices/system/cpu/isolated` 确认。

**确认拓扑。** 记下每块网卡的 PCI 地址、所在 NUMA 节点，以及各 NUMA 节点的核编号和超线程对应关系：

```bash
lspci -D | grep -i mellanox                          # PCI 地址（BDF）
cat /sys/bus/pci/devices/<BDF>/numa_node             # 网卡所在 NUMA
ls /sys/bus/pci/devices/<BDF>/net/                   # 对应的内核网口名
lscpu | grep -i numa                                 # 各 NUMA 的核
cat /sys/devices/system/cpu/cpu<N>/topology/thread_siblings_list   # 核 N 的超线程兄弟
```

### 4.2 每次开机后

```bash
# 1. 停 irqbalance，CPU 调频设为 performance
sudo systemctl stop irqbalance
sudo cpupower frequency-set -g performance

# 2. 大页：每个用到的 NUMA 节点 4096 个 2MB 页
for n in 0 1; do
  echo 4096 | sudo tee /sys/devices/system/node/node$n/hugepages/hugepages-2048kB/nr_hugepages
done

# 3. 每个测试网口（IF 为内核网口名）
sudo ip link set $IF up
sudo mlnx_qos -i $IF --prio_tc=0,0,0,0,0,0,0,0     # 所有优先级归一个 TC，见下
sudo ethtool -A $IF rx off tx off                  # 关 pause 流控

# 4. 网卡 PCIe MaxReadReq 改为 4096（须在 axiperf 未运行时改）
cur=$(sudo setpci -s $BDF CAP_EXP+8.w)
sudo setpci -s $BDF CAP_EXP+8.w=$(printf %04x $(( (0x$cur & 0x8fff) | (5 << 12) )))
```

为什么：

- **单 TC**：axiperf 用 802.1Q PCP 区分 AXI 通道：PCP 0 写请求（AW）、1 读请求（AR）、2 写响应（B）、3 读响应（R）。网卡默认把不同 PCP 分到不同 TC 并按严格优先级调度，读写并发时低 TC 的帧会被饿死几十毫秒，表现为 RTT 飙升、lost / err 增加。程序启动时会检查，发现这种配置会打印 `*** 警告：... 严格优先级`。
- **MaxReadReq**：默认 256 时网卡从内存取大包的效率低。网卡复位或重启后会变回 256。
- `mlnx_qos` 随 MLNX_OFED / DOCA 安装；`cpupower` 在 Ubuntu 上来自 `linux-tools-$(uname -r)`。

**测试口要静默（被测设备会把收到的所有帧当请求时）。** 内核会在网口上发 IPv6、mDNS、DHCP 等帧。直连测试时这些帧只计入 `ign` 列、不影响结果；但如果被测设备会把收到的任何帧当成 AXI 请求处理，就要先让内核在测试口上不发东西：

```bash
sudo nmcli device set $IF managed no
sudo sysctl -q net.ipv6.conf.$IF.disable_ipv6=1
```

### 4.3 检查清单

| 项 | 怎么看 | 期望 |
|---|---|---|
| 链路 | `cat /sys/class/net/$IF/carrier` | 1 |
| PCIe 宽度 | `sudo lspci -s $BDF -vv \| grep LnkSta:` | `Width x16` |
| MaxReadReq | `sudo lspci -s $BDF -vv \| grep MaxReadReq` | 4096 |
| TC | `sudo mlnx_qos -i $IF \| grep -c '^tc:'` | 1 |
| pause | `sudo ethtool -a $IF` | RX / TX 都是 off |
| 大页 | `grep -i huge /proc/meminfo`，或看各节点的 `nr_hugepages` | 每个节点都够 |
| 隔离核 | `cat /sys/devices/system/cpu/isolated` | 包含工作核 |
| 残留进程 | `pgrep -af '[a]xiperf'` | 没有 |

### 4.4 权限

axiperf 要访问大页和网卡，用 `sudo` 运行。同一台机器同时跑多个 axiperf 进程时，每个进程的 `--file-prefix` 必须不同，核不能重叠。

---

## 5. 测试拓扑

### 5.1 基本形态

```text
     sender host                     direct / switch / DUT                 reflector host
+-----------------+   request   +---------------------+   request   +-------------------+
| axiperf         | ----------> |                     | ----------> | axiperf           |
| --mode sender   |             |   DUT / switch /    |             | --mode reflector  |
|                 | <---------- |   direct cable      | <---------- |                   |
+-----------------+  response   +---------------------+  response   +-------------------+
```

发送端（sender）按窗口发请求、统计吞吐和 RTT；反射端（reflector）收请求、回响应；中间是直连线缆、交换机或被测设备（DUT）。两端可以在两台主机上，也可以在同一台主机的两个网口上（各自一个进程，`--file-prefix` 不同、核不重叠）。

| 中间是什么 | 用途 | 要点 |
|---|---|---|
| 直连（网线 / 光纤） | 测主机、网卡、软件本身的基线 | 发送端 `--dmac` 填反射端网卡的真实 MAC |
| 以太网交换机 | 经交换机的多口测试 | 两端 `--vid` 填交换机上的 VLAN；启动时每条流发 4 个广播帧让交换机学习 MAC |
| 被测设备 | 测设备的吞吐、时延、正确性 | 帧格式要和设备一致（`--hdr`、`--no-len`、`--ethertype`），地址字段按设备的转发规则填 |

### 5.2 一个网口上的多条流

一个物理口上可以跑多条流（`--flows N`，每口 1~16 条）。每条流有自己的 MAC 对、收发队列、核和 AXI ID 空间，流之间互不影响：

| 流 | 发送端源 MAC | 反射端 MAC（请求的目的 MAC） | 队列和核 |
|---|---|---|---|
| 0 | S0 = 发送端网卡真实 MAC | R0 = 反射端网卡真实 MAC（`--dmac` 给出） | 两端各一组 |
| 1 | S1，由 S0 派生 | R1，由 R0 派生 | 两端各一组 |
| … | … | … | … |

流 f（f ≥ 1）的 MAC 由真实 MAC 派生：首字节置本地管理位、清组播位，末字节加 f。启动时逐条打印。网卡按目的 MAC 用 rte_flow 把每条流导到它自己的接收队列。

被测设备会改写 MAC、或者只认一对 MAC 时（多流到达反射端时都是同一对 MAC），反射端用 `--rx-split N`：网卡按每包的随机值把请求均分到 N 个队列，每个队列一个核。发送端各流用 `--one-dmac` 都发往同一个目的 MAC。

### 5.3 多个网口

`-a` 给多个网卡，`--flows` 是每口的流数，流和核按"口 0 的各流、口 1 的各流……"顺序展开。DPDK 端口号按 PCI 地址排序，和 `-a` 的书写顺序无关；`--dmac`、`--e100c-port` 里的 PORT 是这个 DPDK 端口号（0、1……），不是 PCI 地址。

同一台机器的多个口接到同一交换机的同一 VLAN 时，交换机泛洪的帧会出现在本机的其它口上，计入 `xmac` 列并丢弃，不影响结果。

### 5.4 同一网口双向打流

两端都要发请求时（A 写 B、同时 B 写 A），一个网口上要同时跑发送端和反射端两个进程。这需要网卡能把收到的帧分给两个进程：两个进程都加 `--isolate`，用 rte_flow 隔离模式，发送端只收响应、反射端只收请求。目前只用于 `--ethertype` 格式（靠帧头里的请求 / 响应标志位区分）。

---

## 6. 工作原理

### 6.1 AXI 事务和帧

- 一帧里放 k 个 AXI 事务（`--pack`，读用 `--rpack`），每个事务 N 拍（`--beats`），每拍 64 字节数据。
- 写请求：每事务 16 字节头 + N × 64 字节数据；写帧总拍数 `pack × beats ≤ 8`。读请求：每事务 16 字节头，每帧最多 4 个事务。
- 写响应 B：每事务 12 字节，反射端可以把同一批收到的多个 B 合并进一帧（`--b-pack`）。读响应 R：每事务 4 字节头 + N × 64 字节数据，反射端每个读响应帧最多 8 拍，多了拆成多帧。
- 每个事务带 9 位 AXI ID。发送端按 ID 匹配响应、计算 RTT。

### 6.2 帧头格式

| 格式 | 参数 | 帧头 | 说明 |
|---|---|---|---|
| eth（默认） | `--hdr eth` | `DMAC SMAC 0x8100 PCP/VID Length` | 802.1Q + 802.3 Length，PCP 表示 AXI 通道（VC） |
| eth 无 Length | `--hdr eth --no-len` | `DMAC SMAC 0x8100 PCP/VID` | 事务头紧跟 VLAN 标签，按帧长解析，全 0 的事务头视为补齐 |
| OISA | `--hdr eth --ethertype X` | `OISA 头 12 字节, EtherType, 2 字节` | 无 VLAN、无 Length，字段见 [OISA 包头定义](E100c_OISA包头定义.md) |
| sue（试验性） | `--hdr sue` | SUEv1-标准 ETH | GPU ID 写在 MAC 字段，字段取值未最终确认 |

两端的帧头参数必须一致。`--min-frame N` 把短帧补 0 到 N 字节（被测设备有最小帧长要求时用）。

### 6.3 窗口和 ID

- **窗口**（`--window`）：每条流同时在途的事务数上限。窗口满了就等响应回来再发。
- **ID 分配**：默认用空闲 ID 池，收到响应的 ID 立刻放回池里复用，不会被最老的在途事务挡住；`--id-seq` 改为按顺序分配。
- **超时**（`--timeout-us`）：事务超过这个时间没回响应就回收 ID、计入 `lost`。之后再收到它的迟到响应，计入 `err`（9 位 ID 无法区分迟到的响应和复用后的新响应）。设为 0 不回收：丢一个包，这个 ID 就一直占着。

### 6.4 读写组合（`--op`）

| 值 | 含义 | 反射端 |
|---|---|---|
| `write` | 只写 | 不用设 `--op`，自动识别读写 |
| `read` | 只读 | 同上 |
| `mix` | 偶数号流写、奇数号流读，至少 2 条流 | 同上 |
| `rw` | 同一 MAC 上同时读写：读、写各一个上下文，各有自己的窗口、队列和核 | **也要加 `--op rw`**，两端 `--flows` 相同 |

### 6.5 核的分配

```text
上下文数 = 网口数 × flows ×（--op rw 时为 2，否则为 1）       反射端 --rx-split N 时 flows = N
每个上下文的核数 = 发送端默认（收发分核）：tx-cores + 1     发送端 --nosplit、反射端：1
需要的核数 = 1（统计核）+ 上下文数 × 每个上下文的核数
```

- `-l` 列表的第一个核是统计核，每秒打印一次结果。其余核按流的顺序分配：每条流取和其网卡同一 NUMA 节点、编号最小的空闲核。
- 核不够时启动就报错退出（`需要 N 个 lcore ...`），不会碰网卡。
- 本 NUMA 节点的核不够、但总数够时，会借用另一节点的核并打印 `警告：... NUMA ... 的核不够`。跨 NUMA 性能会下降，应调整 `-l`。
- 核给多了不报错，多出来的核不分配任务。实际用了哪些核看启动时的 `flow p.f TX/RX -> lcore n`（`--brief` 时不打印）。
- DPDK 的 lcore 编号须小于 128。物理核编号更大时用 `--lcores` 映射，例如 `--lcores 0@130,1@131,2@132`（lcore 0~2 跑在 CPU 130~132 上）。

### 6.6 收发分核和多发送核

发送端默认每条流 2 个核：发送核发请求，接收核收响应、算 RTT、把 ID 还回池里。`--nosplit` 改为一个核收发，省核但包率低。

单条流的包率被一个发送核限制时，`--tx-cores N` 给每条流 N 个发送核：各占一个发送队列和一段 ID（窗口均分），共用一个接收核。不同发送队列之间在线上可能乱序。

### 6.7 随机测试（数据校验）

`--random`（两端都加）时，发送端每帧的事务数、每个事务的拍数都随机变化，数据为自校验图样：每个 64 字节拍的首字低 8 位是"拍序号 × 16 + 本事务拍数"，其余 15 个字由首字用 xorshift32 推出。收端只看本拍就能判断数据是否被改、拍序是否错乱、拍数是否正确。反射端校验写数据，发送端校验读数据，不对的事务数在 `derr` 列。

启动时打印 `random: seed=...`，发送端加 `--seed <同一个值>` 可以复现同样的随机序列。随机模式逐帧现场构造，发送端 CPU 跑满，吞吐低于普通模式，只用来查正确性。

### 6.8 AXI user 域（tsn 和 port）

发送端在每个请求事务的 awuser / aruser 里填两个字段，响应（B / R）不带：

| 字段 | 写请求位置 | 读请求位置 | 填什么 |
|---|---|---|---|
| tsn（16 位） | awuser[48:33]（W0[4:0] + W1[31:21]） | aruser[45:30]（W0[3:0] + W1[31:20]） | 事务序号，自动填 |
| port（3 位） | awuser[51:49]（W0[7:5]） | aruser[48:46]（W0[6:4]） | `--e100c-port PORT,N` 给的 N，没设为 0 |

- **tsn**：每个 DPDK 端口写、读各一个计数，每个请求事务加 1，16 位循环。单流单发送核时线上严格连续；多流或多发送核时各队列交错，线上不保证单调。
- **port**：告诉被测设备这个事务要从它的哪个口出去。`--e100c-port PORT,N` 里 PORT 是本机 DPDK 端口号，N 是这个端口经被测设备后要去的口号；多个网卡可以各写一次。`--e100c-rport N` 只改读请求的 port，用于读、写从同一网口发出、走不同出口的场景。
- 参数名里的 `e100c` 是历史命名，任何按 user.port 转发的设备都用它。
- **被测设备按 user.port 转发时，port 必须和实际接线一致**，填错了请求会被送到别的口，表现为收不到响应。直连或经普通交换机时没人看这个字段，不用设。
- 其余 user 位、地址字段填 0（OISA 格式下读请求的 `sect_mask` 按拍数自动填，见包头文档）。Wireshark 插件显示为 `axiperf.user_tsn` / `axiperf.user_port`。

---

## 7. 必须设对的参数

大多数参数有可用的默认值，下面这些**不设或设错就打不通、打错地方，或者结果不可信**。启动前逐项核对。

**两端必须一致**（一端设了另一端没设，对端会把帧当成非法帧或解析错）：

| 参数 | 设错的后果 |
|---|---|
| `--hdr`、`--no-len`、`--ethertype` | 帧头格式不同，对端解析不出事务，计入 `err` 或 `ign`，收不到响应 |
| `--vid` | 过交换机时 VLAN 不通、收不到响应。程序收包时不检查 VID，所以直连时不一致也能跑，容易漏掉，换到交换机上才出问题 |
| `--flows` | eth 头按流派生 MAC 并按 MAC 导流，流数不同时多出来的流没有对端（反射端用 `--rx-split` 时除外） |
| `--op rw` | 只有一端设时，读写导流对不上 |
| `--random` | 只有一端设时，数据校验全错（`derr` 增长）或长度不符 |
| `--isolate` | 同一网口双向打流时两个进程都要加，否则一个进程收不到帧 |

**发送端必须设：**

| 参数 | 不设或设错的后果 |
|---|---|
| `--dmac PORT,MAC` | 默认值 `02:00:00:00:01:0PORT` 不是真实地址。直连时要填反射端网卡的真实 MAC，否则反射端网卡（非混杂）直接丢弃，`rx` 为 0。多网卡时每个端口各写一个 |
| `--e100c-port PORT,N` | **只在被测设备按 user.port 转发时必须设**（见 [6.8 节](#68-axi-user-域tsn-和-port)）。不设就填 0，请求会被送到错误的出口或被丢弃；直连不用设 |
| `--smac` | OISA 格式下它是帧头第 6~11 字节的初值（source_id 等），要填被测设备认可的值；eth 直连不用设 |

**EAL 参数必须设对：**

| 参数 | 设错的后果 |
|---|---|
| `-a <BDF>` | 用错网卡；DPDK 端口号按 PCI 地址排序，和 `--dmac`、`--e100c-port` 里的 PORT 对应 |
| `-l` / `--lcores` | 核不够直接报错；核和网卡不在同一 NUMA 时性能下降 |
| `--file-prefix` | 同一台机器跑多个进程时必须各不相同，否则后起的进程起不来 |
| devargs `dv_flow_en=2` | 反射端用 `--rx-split` 时必须加，否则启动报错 |

**为防止"看着在跑、其实打错了"**，每次换配置后先小流量确认：

1. 用 `--window 4 --time 5` 跑一次，看发送端 `rxL2G`（详细输出为 `rxMpps`）不为 0、`err` / `bad` 为 0、RTT 合理；
2. 有被测设备时，用 `--pcap` 或 `dpdk-dumpcap` 抓几帧，用 Wireshark 插件核对目的 MAC、VID、user.port；
3. 再加大窗口和流数测性能。

---

## 8. 运行示例

命令格式：

```bash
sudo ./axiperf <DPDK EAL 参数> -- <axiperf 参数>
```

以下示例为直连、标准 eth 头，反射端和发送端在两台主机上。先在两台主机上设好变量：

```bash
BDF=<本机测试网卡的 PCI 地址>        # lspci -D | grep -i mellanox
SINGLE=txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0
MULTI=mprq_en=1,rxqs_min_mprq=1,mprq_log_stride_num=9,txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=1
```

`SINGLE` 用于单流，`MULTI` 用于多流，见 [11.1 节](#111-网卡驱动参数devargs)。示例里的核号只是示意，换成本机和网卡同一 NUMA 的隔离核，个数按 [6.5 节](#65-核的分配) 算。

**先起反射端，再起发送端；停的时候先停发送端。**

### 8.1 单流写、读

```bash
# 反射端：1 个统计核 + 1 个收发核
sudo ./axiperf --file-prefix refl --huge-unlink -l 1,2 -a $BDF,$SINGLE --   --mode reflector --vid 1

# 发送端：1 个统计核 + 发、收各 1 个核；写 10 秒
sudo ./axiperf --file-prefix snd --huge-unlink -l 1-3 -a $BDF,$SINGLE --   --mode sender --op write --vid 1 --dmac 0,<反射端网卡 MAC> --time 10

# 读：发送端改 --op read
```

### 8.2 多流

```bash
# 反射端 8 流：1 + 8 核
sudo ./axiperf --file-prefix refl --huge-unlink -l 1,2-9 -a $BDF,$MULTI --   --mode reflector --vid 1 --flows 8 --burst 16

# 发送端 8 流、读写混跑：1 + 8 × 2 核
sudo ./axiperf --file-prefix snd --huge-unlink -l 1,2-17 -a $BDF,$MULTI --   --mode sender --op mix --vid 1 --flows 8 --burst 16 --time 30 --brief   --dmac 0,<反射端网卡 MAC>
```

### 8.3 多网卡

```bash
# 两口各 8 流，发送端每条流单核收发：1 + 16 核
sudo ./axiperf --file-prefix snd --huge-unlink -l 1,2-17   -a <网卡 0 的 PCI 地址>,$MULTI -a <网卡 1 的 PCI 地址>,$MULTI --   --mode sender --op write --vid 1 --flows 8 --nosplit --burst 16 --time 10   --dmac 0,<对端口 0 的 MAC> --dmac 1,<对端口 1 的 MAC>
```

两块网卡在不同 NUMA 节点时，`-l` 里两个节点的核都要给够，程序会按网卡所在节点选核。

### 8.4 多个流汇到反射端的一对 MAC（`--rx-split`）

```bash
# 反射端：网卡按随机值分到 4 个队列，1 + 4 核；devargs 须加 dv_flow_en=2
sudo ./axiperf --file-prefix refl --huge-unlink -l 1,2-5 -a $BDF,dv_flow_en=2,$SINGLE --   --mode reflector --vid 1 --rx-split 4 --rxd 4096 --burst 16

# 发送端：4 流都发往同一个目的 MAC，1 + 4 × 2 核
sudo ./axiperf --file-prefix snd --huge-unlink -l 1,2-9 -a $BDF,$MULTI --   --mode sender --op write --vid 1 --flows 4 --one-dmac --dmac 0,<反射端网卡 MAC> --time 30 --brief
```

### 8.5 随机测试

```bash
# 两端都加 --random；反射端其它参数不变
... --mode reflector --vid 1 --random
... --mode sender --op mix --flows 2 --vid 1 --dmac 0,<MAC> --pack 2 --rpack 4 --beats 4 --random --time 30
```

结果看 `derr` 和 `err` 都为 0。

### 8.6 抓包

```bash
# 程序自己写 pcap：小窗口、只抓前 50 帧（有锁和文件写入，性能会下降）
... --window 4 --pcap ~/x.pcap --pcap-count 50

# 不停流旁路抓包（程序启动时已初始化 pdump）
sudo dpdk-dumpcap --file-prefix snd -i 0 -w ~/x.pcapng -c 1000
```

用 Wireshark 打开时把 `wireshark/axi_over_eth.lua`（eth / sue 头）或 `wireshark/oisa_axi.lua`（OISA 头）放进 Wireshark 的插件目录。抓包文件不要放 `/tmp`，重启会清空。

---

## 9. 参数

`[ ]` 里是默认值。`./axiperf --help` 打印同样的内容。

### 9.1 DPDK EAL 参数（`--` 之前）

| 参数 | 含义 |
|---|---|
| `-l <核列表>` | 用的核，第一个是统计核。如 `-l 1,2-5` |
| `--lcores <映射>` | lcore 到物理核的映射，物理核编号 ≥ 128 时用。如 `--lcores 0@130,1@131` |
| `-a <BDF>[,devargs]` | 用的网卡及驱动参数，可写多个；devargs 见 [11.1 节](#111-网卡驱动参数devargs) |
| `--file-prefix <名字>` | 大页文件前缀；同时跑多个进程时必须不同，也是 `dpdk-dumpcap` 找进程用的名字 |
| `--huge-unlink` | 不留大页文件，进程被杀后不残留 |

### 9.2 角色与流量

| 参数 | 默认 | 说明 |
|---|---|---|
| `--mode sender\|reflector` | sender | 角色 |
| `--op write\|read\|mix\|rw` | write | 读写组合，见 [6.4 节](#64-读写组合--op)；rw 时两端都要设 |
| `--flows N` | 1 | 每口流数，1~16 |
| `--time SEC` | 0 | 运行秒数；0 = 直到 Ctrl-C |

### 9.3 发送端

| 参数 | 默认 | 说明 |
|---|---|---|
| `--window N` | 511 | 每个上下文的在途事务数，1~511（`--ethertype` 时可更大，见 `--help`） |
| `--pack N` | 2 | 每个请求帧的事务数；写要求 pack × beats ≤ 8 |
| `--rpack N` | 同 `--pack` | 读请求每帧事务数，1~4；mix / rw 时读、写可分别设 |
| `--beats N` | 4 | 每事务拍数，1~4（每拍 64 字节） |
| `--burst N` | 32 | 每次发送的包数，1~64；反射端为攒够多少个响应发一次 |
| `--tx-cores N` | 1 | 每条流的发送核数，1~4；不能与 `--nosplit`、`--id-seq` 同用 |
| `--nosplit` | 关 | 每条流一个核收发 |
| `--id-seq` | 关 | ID 按顺序分配；默认空闲 ID 池 |
| `--timeout-us N` | 10000 | 事务超时回收并计入 lost；0 = 不回收 |
| `--count N` | 0 | 每个上下文发满 N 个请求帧就停发（进程仍运行到 `--time`）；0 = 不限 |
| `--e100c-port PORT,N` | 0 | 端口 PORT 发出的请求，user.port 填 N（1~4），见 [6.8 节](#68-axi-user-域tsn-和-port)；可为各端口重复设置 |
| `--e100c-rport N` | 同 `--e100c-port` | 只改读请求的 user.port（0~7） |

### 9.4 反射端

| 参数 | 默认 | 说明 |
|---|---|---|
| `--b-pack N` | 1 | 同一批收到的写请求，其 B 合并进一个响应帧，每帧最多 N 个（1~16） |
| `--drop-every N` | 0 | 每 N 个请求帧丢 1 个，用来验证发送端的丢失处理 |
| `--rx-split N` | 关 | 同一对 MAC 的请求由网卡按随机值均分到 N 个队列（2/4/8/16），各一个核；不能与 `--flows` 同用；devargs 须加 `dv_flow_en=2` |
| `--rxd N` | 1024 | 每个接收队列的描述符数，2 的幂、64~32768；高包率时建议 4096 |

### 9.5 地址和帧格式

| 参数 | 默认 | 说明 |
|---|---|---|
| `--hdr eth\|sue` | eth | 帧头格式，见 [6.2 节](#62-帧头格式) |
| `--vid N` | 0 | 802.1Q VLAN ID，两端一致 |
| `--dmac PORT,MAC` | 02:00:00:00:01:0PORT | 发送端：DPDK 端口 PORT 的对端基准 MAC（对端网卡真实 MAC），可为各端口重复设置 |
| `--smac MAC` | 网卡真实 MAC | 发送端：流 0 的源 MAC，其余流由它派生 |
| `--one-dmac` | 关 | 发送端各流都发往 `--dmac` 给的同一个 MAC；反射端用 `--flows 1` 或 `--rx-split` |
| `--no-len` | 关 | eth 头不带 Length；两端都要设 |
| `--ethertype X` | 关 | OISA 格式：无 VLAN、无 Length；不核对 PCP 和 DMAC，两端都开混杂；发送端不支持 rw，`--flows` 须为 2 的幂 |
| `--min-frame N` | 0 | 请求、响应都补 0 到 N 字节（不含 FCS）；0 = 不补 |
| `--promisc` | 关 | 强制混杂模式；默认 eth 头只收本端各流的 MAC |

### 9.6 随机测试

| 参数 | 默认 | 说明 |
|---|---|---|
| `--random` | 关 | 两端都加，见 [6.7 节](#67-随机测试数据校验)；只用于 eth 头（含 `--ethertype`），不能与 `--tx-cores` > 1、`--r-split` 同用 |
| `--seed N` | 启动时的 TSC | 随机种子，启动时打印，用于复现 |

### 9.7 输出、诊断和抓包

| 参数 | 默认 | 说明 |
|---|---|---|
| `--brief` | 关 | 精简输出：每秒只打所有流合并的一行（mix / rw 时 w、r、all 三行），不打每流明细和绑核行 |
| `--hwts` | 关 | 开接收硬件时间戳，统计网卡收到到 CPU 取到的时延（`rxd50us` / `rxd99us` 列） |
| `--dump N` | 0 | 打印前 N 个收 / 发帧的十六进制 |
| `--pcap FILE` | 关 | 把收发的 AXI 帧写入 pcap 文件 |
| `--pcap-count N` | 1000 | `--pcap` 最多写入的帧数 |

### 9.8 SUE 头（试验性）

| 参数 | 默认 | 说明 |
|---|---|---|
| `--gpu-id N` | 必填 | 本端 GPU ID 基址，流 k 用 N+k |
| `--peer-gpu-id N` | 必填（发送端） | 对端 GPU ID 基址 |
| `--sue-ethertype X` | 0x88B5 | EtherType，须 > 1500 |
| `--sue-format N` | 0 | Format[2:0] |
| `--sue-pkttype N` | 0 | PktType[4:0] |

### 9.9 对接特定设备的参数

`--isolate`（见 [5.4 节](#54-同一网口双向打流)）、`--b-port`、`--b-tag`、`--r-user`，以及调试参数 `--ar-user-low`、`--req-b1`、`--req-tag`、`--gap-us`、`--r-beats`、`--r-echo-aruser`、`--r-len-data`、`--r-pack-beats`、`--r-split`，用于填写 OISA 头和 AXI user 域里的设备相关字段、排查设备的响应格式。直连测试不用。说明见 `./axiperf --help` 和设备的测试手册。

---

## 10. 输出

### 10.1 精简输出（`--brief`）

发送端每秒一行 `all`，mix / rw 时还有 `w`（写）和 `r`（读）两行：

```text
      txL2G   rxL2G   dataG  txnM/s   p50us   p99us  p999us    maxus    err   lost    bad    miss busy%
all     86.5    17.1    79.0   38.60   19.95   23.40    24.7    719.5      0      0      0       0    77
```

| 列 | 含义 |
|---|---|
| `txL2G` / `rxL2G` | 发 / 收二层速率（Gbit/s），按帧长算，不含 FCS、前导码和帧间隔 |
| `dataG` | AXI 数据吞吐（Gbit/s），只算数据。**比较性能看这一列** |
| `txnM/s` | 每秒完成的事务数（百万） |
| `p50us` / `p99us` / `p999us` | RTT 分位数（本秒；退出时为全程） |
| `maxus` | 最大 RTT（累计） |
| `err` | ID 异常（重复响应、超时后迟到的响应）、帧头或负载非法（累计） |
| `lost` | 超时回收的事务数（累计）；`--timeout-us 0` 时恒为 0 |
| `bad` | 目的 MAC 不是本流（`xmac`）或 PCP 和通道不符（`pcpx`）的帧（累计） |
| `miss` | 网卡侧丢包：imissed + rx_nombuf（累计，只在 `all` 行） |
| `busy%` | 最忙的工作核的忙碌比例 |
| `derr` | 只在 `--random` 时有：数据校验不对的事务数（累计） |

写时 `rxL2G` 是写响应（不带数据），所以收比发小很多；读时 `rxL2G` 是读响应（带数据）。

反射端只有 `txL2G`、`rxL2G`、`err`、`bad`、`miss`、`busy%` 等列。反射端 `busy%` 接近 100% 说明反射端核已跑满，要加核（加流或加大 `--rx-split`）。

### 10.2 详细输出（不加 `--brief`）

每秒每条流一行，额外的列：

| 列 | 含义 |
|---|---|
| `txMpps` / `rxMpps` | 发 / 收包速率 |
| `txWireG` / `rxWireG` | 线上速率：帧长（不足 60 按 60）+ FCS 4 + 前导码和帧间隔 20 |
| `meanus` | 平均 RTT（累计） |
| `slow` | RTT > 200 µs 的事务数 |
| `busyT%` / `busyR%` | 发送核 / 接收核的忙碌比例；接近 100% 即该核是瓶颈 |
| `cyc/pk` | 每个包消耗的 CPU 周期 |
| `rxB` | 每次非空接收平均取到的包数；接近 64 说明接收有积压 |
| `ign` | 非 AXI 的背景帧数（内核发的 IPv6、交换机学习帧等） |
| `swd50us` / `swd99us` | 软件时延：发送端从打时间戳到发出，反射端从收到到响应发出 |
| `imissed` / `nombuf` | 网卡丢包（描述符不足 / mbuf 不足） |

RTT 直方图 0~200 µs 为 50 ns 一档，200 µs~100 ms 为 100 µs 一档。

### 10.3 结果有效的条件

- 两端的 `err`、`bad`、`miss` 都为 0（`--random` 时还有 `derr`）；
- `lost` 为 0，或者能解释（如 `--drop-every`）；
- 吞吐逐秒平稳；
- 启动时没有 NUMA、TC、devargs 相关的警告。

时延和吞吐的关系（Little 定律）：RTT ≈ 在途事务数 ÷ 事务率。加大窗口或流数后时延成比例上升、吞吐不变，说明瓶颈在后面，再加在途没有用。

### 10.4 理论值

每 100G 线速：写请求 21.3 Mpps / 数据 87.4G；读响应 22.2 Mpps / 数据 91.1G（eth 头，pack 2、beats 4）。

---

## 11. 调优

### 11.1 网卡驱动参数（devargs）

写在 `-a <BDF>,` 后面，两端每块网卡都要加。程序启动时按流数检查，配置不合适会提示。

| 场景 | devargs |
|---|---|
| 单流 | `txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=0` |
| 多流 | `mprq_en=1,rxqs_min_mprq=1,mprq_log_stride_num=9,txqs_min_inline=1,txq_inline_mpw=128,rxq_pkt_pad_en=1,rxq_cqe_comp_en=1` |
| 反射端 `--rx-split` | 在上面的基础上加 `dv_flow_en=2` |

- **多包接收（`mprq_en=1`）**：多流时发送端要以几十 Mpps 收小的写响应，必须开。单流收的是 500 多字节的大包，开了反而略慢。
- **CQE 压缩（`rxq_cqe_comp_en`）**：多流高包率时必须开，关掉时高包率下会丢包（`miss` 增长）；单流或总包率低、要低时延时关掉，满载 RTT 更低。
- **发送内联**：每口发送队列少于 8 个时 mlx5 默认不内联，要加 `txqs_min_inline=1`；`txq_inline_mpw=128` 只内联 128 字节以下的小包。
- `ethtool` 的 `rx_cqe_compress` 和 `mlxconfig` 的 `CQE_COMPRESSION` 都管不到 DPDK 侧的 CQE 压缩，只能用 devargs。

### 11.2 核和流

- 单条流的事务率受一个接收核限制，到上限后（busyR% 接近 100%）要更高的吞吐就加流。
- 发送端单流包率被发送核限制时，用 `--tx-cores 2`。
- 收发核用隔离核、和网卡同一 NUMA；同一物理核的两个超线程不要都分给高负载的核。
- 多流时 `--burst 16` 通常比默认 32 好；反射端 `--rx-split` 高包率时 `--rxd 4096`。

### 11.3 性能测试时关掉

`--dump`、`--pcap`、`--hwts`、`--random` 都会降低性能，只在调试时用。

---

## 12. 注意事项和常见问题

**操作**

- 先起反射端，再起发送端；停的时候先停发送端。中间有被测设备时，反射端先退出会让设备里已经发出、还没回响应的请求无处可回，可能把设备卡住。反射端用 `--time` 时要比发送端的时间长。
- Ctrl-C 一次正常退出（打印全程汇总、关闭抓包文件）；卡住时再按一次强制退出。
- 停后台进程时 `pkill -f '[a]xiperf'`，避免匹配到执行 pkill 的 shell 自己。
- 日志和抓包不要放 `/tmp`。

**常见问题**

| 现象 | 原因和处理 |
|---|---|
| `需要 N 个 lcore` | `-l` 里的核不够，按 [6.5 节](#65-核的分配) 算 |
| `警告：... NUMA ... 的核不够` | `-l` 里和网卡同一 NUMA 的核不够，换核 |
| `ports: 0` | `-a` 的 PCI 地址不对，或网卡不是 mlx5 / 驱动没加载；`lspci -D` 确认 |
| 报大页不足或 `Cannot init memory` | 大页没配或被残留进程占着：查 `nr_hugepages`、`pgrep -af '[a]xiperf'`；加 `--huge-unlink` |
| 报 `--file-prefix` 冲突或端口被占用 | 有残留进程，或两个进程用了同一个 `--file-prefix` |
| `--rx-split 需要 RTE_FLOW_ITEM_TYPE_RANDOM` | DPDK 不带 RANDOM，换 DPDK 版本，见 [2.2 节](#22-软件) |
| `--rx-split：rte_flow_configure 失败` | devargs 没加 `dv_flow_en=2` |
| `*** 警告：... 严格优先级` | 网卡多 TC，读写并发时会饿死低优先级帧；按 [4.2 节](#42-每次开机后) 设单 TC |
| `警告：port N 链路未 up` | 查线缆、`carrier`、对端是否 up |
| 发送端收不到响应（`rxL2G` 为 0） | 反射端没起；`--dmac` 不对；两端 `--hdr`、`--flows` 不一致；过交换机时 `--vid` 和交换机 VLAN 不符；被测设备按 user.port 转发时 `--e100c-port` 不对 |
| `xmac` 持续增长 | 交换机泛洪的帧进了本机网口，已丢弃，不影响结果 |
| `ign` 增长 | 内核在测试口上发的背景帧，不影响结果；被测设备会把它当请求时按 [4.2 节](#42-每次开机后) 静默网口 |
| `miss` 增长 | 接收来不及：加核 / 加流，开 CQE 压缩和多包接收，加大 `--rxd` |
| 某网口吞吐明显偏低 | PCIe 宽度不是 x16，或 MaxReadReq 还是 256 |
| 读写并发时 `maxus` 几十毫秒、`lost` 增加 | 网卡多 TC 严格优先级，改单 TC |

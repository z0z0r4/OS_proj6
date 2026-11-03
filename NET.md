# xv6 网络功能（分支 `net-native`）

在基线 xv6（RISC-V + LoongArch）上增加一个小型 TCP/IP 协议栈、virtio-net 驱动和 xv6 风格的套接字系统调用，
并把 iperf 与 netperf 重写为 xv6 用户程序。**不兼容 Linux ABI，也没有动态链接**；基线的系统调用、文件系统和测试不受影响。

## 组成

| 文件 | 作用 |
|---|---|
| `src/kernel/virtio_net.c` | virtio-net 驱动（现代接口，无 offload）。RISC-V 走 virtio-mmio，LoongArch 走 virtio-pci |
| `src/kernel/arch/loongarch/virtio_pci.c` | 改成按设备保存状态，使磁盘和网卡可以同时存在 |
| `src/kernel/net.c`, `net.h` | ARP、IPv4、ICMP echo、UDP、TCP、回环队列、套接字 |
| `src/kernel/sysnet.c` | 套接字系统调用 |
| `src/user/iperf.c`, `netperf.c`, `netserver.c` | 重写的测试程序（`netutil.h`、`netproto.h` 为共用头） |
| `tests/net/` | 测试：`cmds.sh`、`run.sh`、宿主机对端 `host_server.py` |

## 系统调用

`socket(type)`（`1` 流式 / `2` 数据报）、`bind(fd, port)`、`listen(fd)`、`accept(fd, &ip, &port)`、
`connect(fd, ip, port)`、`sendto(fd, buf, n, ip, port)`、`recvfrom(fd, buf, n, &ip, &port)`、`shutdown(fd)`、
`rcvtimeo(fd, ticks)`、`nstime()`（纳秒单调时钟）、`yield()`。流式套接字也可用 `read`/`write`，
地址是主机序的 32 位整数。

## 设计要点

* **一把锁。** `netlock` 保护整个协议栈；进程用 `sleep(chan, &netlock)` 阻塞。
* **回环不递归。** 发往 `127.0.0.0/8` 和本机地址的包进入一个队列，在每个入口（系统调用、中断、时钟）结束前
  `net_drain()` 逐个处理，因此栈内不会嵌套调用。
* **没有网卡也能用。** 回环不依赖 virtio-net；机器上没有网卡时 `netinit()` 只是不启用以太网。
* **TCP。** 三次握手、MSS 选项、滑动窗口（接收缓冲 32KB）、累计确认、超时与三次重复确认触发的 go-back-N 重传、
  零窗口探测、FIN 状态机、TIME_WAIT；`close` 之后数据仍会继续发送完。没有拥塞控制、乱序缓存、窗口缩放。
* **以太网。** 静态地址 `10.0.2.15/24`，网关 `10.0.2.2`（QEMU 用户态网络的默认值）；ARP 缓存 4 项。
* **中断。** RISC-V 经 PLIC；LoongArch 经 PCH-PIC 的 INTx（线号按槽位计算），并在时钟中断里再轮询一次兜底。
  发送完成不产生中断（`NO_INTERRUPT`），避免电平中断在清除瞬间被吞掉。

## 构建与运行

```sh
cd src && make qemu                      # RISC-V；ARCH=loongarch CPUS=1 make qemu 为 LoongArch
# xv6 shell 里：
iperf -s -1 &            iperf -c 127.0.0.1 -t 5        # 另有 -u [-b Mbit/s]、-n bytes、-l len
netserver &              netperf -t TCP_STREAM -l 5     # TCP_STREAM UDP_STREAM TCP_RR UDP_RR TCP_CRR
netperf -H 10.0.2.2 ...                                 # 经虚拟网卡访问宿主机
```

QEMU 命令行已加入 `-netdev user,id=n0` 和 `virtio-net-(device|pci)`。

## 测试

`make net && make run`（LoongArch：`make loongarch_net && make run`）。`run.sh` 先在宿主机启动
`host_server.py`（端口 5311 / 12875），然后依次运行回环测试和经 virtio-net 到宿主机的测试。
基线 `tests/run_all.sh` 22 项在改动后仍全部通过。

典型结果（QEMU TCG，3 个 CPU）：

|  | RISC-V | LoongArch |
|---|---|---|
| iperf TCP 回环 | ~40 Mbit/s | ~100 Mbit/s |
| iperf UDP 5 Mbit/s | 0 丢包 | 0 丢包 |
| netperf TCP_RR 回环 | ~1150 次/秒 | ~2500 次/秒 |
| netperf TCP_RR 到宿主机 | ~770 次/秒 | ~1270 次/秒 |

## 限制

* 没有 DHCP、IP 分片与重组、IPv6、拥塞控制、乱序缓存、TCP 选项（除 MSS）。
* LoongArch 上批量 TCP 到宿主机只有约 2.5 Mbit/s（其余路径正常）；原因尚未查明，中断路径是第一怀疑对象。
* 空闲核心用 `wfi`/`idle` 休眠，`wakeup()` 不会唤醒它们，所以对 UDP 这类连续发送的程序需要发送方主动 `yield()`。
* 套接字最多 64 个，每个流式套接字占 64KB 缓冲。

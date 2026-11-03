# xv6 动态链接（分支 `net-native`）

在 xv6 上支持动态链接的可执行文件和共享库：内核负责装载，用户态的动态链接器 `ldso` 负责依赖解析、符号绑定和重定位，
并提供 `dlopen`/`dlsym`。原有静态链接的 xv6 程序完全不变。不依赖 Linux ABI。

## 组成

| 文件 | 作用 |
|---|---|
| `src/kernel/exec.c` | `exec` 识别 `PT_INTERP`，把解释器装载到程序之上；新增 `dlmap()` 系统调用的实现 `kdlmap` |
| `src/kernel/elf.h` | 动态节、符号、重定位等结构；`struct dyninfo` |
| `src/user/ldso.c` | 动态链接器（解释器），自重定位的 PIE，约 350 行 |
| `src/user/dl.c` | `dlopen/dlsym/dlclose/dlerror`，通过 `__dl_hooks` 转到 `ldso` |
| `src/user/greet.c`, `plugin.c`, `dynhello.c`, `dyntest.c` | 示例库与程序 |
| `tests/dynlink/` | 测试 |

## 工作流程

1. `exec` 读取 ELF：主程序是 `ET_DYN`（以 PIE 方式链接，地址从 0 开始）。有 `PT_INTERP` 时，先装载主程序，
   再把解释器（`/ldso`）装到主程序镜像之上，入口改为解释器的入口。主程序的入口、程序头位置、解释器基址放进
   `struct dyninfo`，压在用户栈上，地址通过 `a2` 交给解释器（`a0`/`a1` 仍是 `argc`/`argv`）。
2. `ldso` 先处理自己的 `R_*_RELATIVE`（目前没有），再读取主程序的 `.dynamic`，对每个 `DT_NEEDED` 调用
   `dlmap("/<name>")`：内核按各段权限把共享库映射到进程当前大小之上（`p->sz` 之后），返回装载地址。
   依赖按广度优先递归装载，同名库只装一次。
3. 符号按装载顺序在全局作用域查找（主程序、各库）。支持三种重定位：`R_*_RELATIVE`、`R_*_64`、`R_*_JUMP_SLOT`
   （RISC-V 与 LoongArch 的编号相同）。绑定是立即的（`-z now`），所以没有 PLT 解析桩。
4. 按依赖的逆序运行 `DT_INIT` 和 `DT_INIT_ARRAY`（构造函数），然后调用主程序的 `main`，返回值交给 `exit`。
5. `dlopen(path)` 走同样的装载、重定位、构造流程；`dlsym(handle, name)` 在指定对象中查找，`handle` 为 0 时查全局作用域。

xv6 的用户地址空间是 `[0, sz)` 一段连续区间，所以不需要新的内存管理：库映射在其上，之后 `sbrk`/`malloc` 继续向上增长，
`fork` 复制整段，`exit` 释放整段。

## 构建

```sh
# 共享库：-fPIC，-shared；程序：-fPIE，-pie，--dynamic-linker=/ldso，-e main
make -C src fs.img      # 生成 user/ldso、libxv6.so、libgreet.so、libplugin.so、_dynhello、_dyntest 并装入镜像
```

编写自己的动态程序：用 `-fPIE` 编译，链接时加 `-pie -z now -z norelro -z max-page-size=4096 --hash-style=sysv
--dynamic-linker=/ldso -e main`，并链接 `libxv6.so`（C 库）。共享库用 `-fPIC` 和 `-shared -soname lib<name>.so`。
共享库和 `ldso` 放在根目录 `/`。

## 测试

`make dynlink && make run`（LoongArch：`make loongarch_dynlink && make run`）。内容：加载期链接（跨库调用、导出变量、
指针表、函数指针、构造函数）、`dlopen`/`dlsym`（含依赖库、重复 `dlopen`、缺失符号和缺失库）、`fork` 之后继续使用、
重复运行 10 次、静态程序不受影响、删除依赖库后的报错和系统稳定性。两个架构都通过；基线 `tests/run_all.sh` 22 项仍全部通过。

## 限制

* 没有 TLS、IFUNC、COPY 重定位、延迟绑定、符号版本；`dlclose` 不会卸载（对象保持映射）。
* 同一进程最多 16 个对象，库名最长 31 个字符，只在根目录 `/` 查找库；文件名受基线 VFS 的 14 字符限制。
* 不做地址空间随机化；没有对段权限之外的保护（沿用 xv6 的页表权限）。
* 动态程序的主程序必须以 `-e main` 链接（xv6 习惯，入口即 `main`）。
* 根文件系统只有 2000 个块，`usertests` 需要大量空闲块，所以新增的二进制（动态库、`ldso`、`iperf` 等）都用 `-s` 去掉了调试信息；
  新增程序时请保持这一点，否则 `loongarch_baseline` 会因磁盘空间不足而超时。

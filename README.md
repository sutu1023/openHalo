# 给 OpenHalo 的 Hash Join 加上 CPU 计数器

## 1. 怎么测一段执行器代码

我选了 Hash Join 构建哈希表的循环。它从输入表中取一行，算出连接键的哈希值，再把这一行放进哈希表。我想知道，建完整张表花了多少个 CPU 周期，执行了多少条指令。

先在数据库里建立事件表 `public.pmc_name`，用 `SET halo_pmc_list = 'cycles,instructions'` 选定周期数和指令数。后端把事件配置读进自己的哈希表，同一连接可以复用。计数器的初始化和读取写在 `halo_pmc.c`，对执行器提供的接口写在 `halo_pmc.h`。[事件表](doc/halo-pmc/setup.sql) · [实现](src/backend/executor/halo_pmc.c) · [头文件](src/include/executor/halo_pmc.h)

在 `MultiExecPrivateHash()` 里，进入建表循环前读一次，退出时再读一次，两次之差就是这段循环的计数。在 Apple M5 的一次查询中，构建 10,000 行的哈希表消耗了 **5,203,783 个周期，执行了 24,119,987 条指令**。[执行器改动](src/backend/executor/nodeHash.c) · [执行计划和原始计数](doc/halo-pmc/results/hash-demo.txt)

## 2. 怎么降低循环里的读取开销

如果只想测算哈希值的那一步，就把读取点放在 `ExecHashGetHashValue()` 前后。计数器保持开启，每次算完只累计前后两个读数的差。这里有两种省开销的办法。

**一次同时读两个指标。** 在 Mac 上，默认用 `thread_selfcounts` 一次取回当前线程的周期数和指令数。为了检验它快多少，同一份程序也可以切换到进程级接口 `proc_pid_rusage`。在支持用户态读取的 Linux 硬件上，代码还可以直接读计数器寄存器。

**按需要选读取频率。** `halo_pmc_sample_period` 设为 `0` 测整个建表循环，设为 `1` 测每一行的哈希计算，设为 `64` 就测第 1、65、129 行，以此类推。10,000 行里会测到 157 行，输出是这 157 行的计数之和。

## 3. Benchmark 和性能改进证据

在 Apple M5、macOS 26.4 上，用同一份调试构建（`-O0 -g`，开启断言）运行同一条查询。一张表有 10,000 行，另一张有 50,000 行，连接结果为 10,000 行。每种模式预热两次，随后轮换顺序，各测 24 轮。表中耗时是整条查询的中位数。[测试数据和查询设置](doc/halo-pmc/workload.sql) · [逐轮结果](doc/halo-pmc/results/query-times.txt)

| 观测方式 | 每次查询在区间边界读取的次数 | 查询耗时 |
|---|---:|---:|
| 关闭观测 | 0 | 7.4360 ms |
| 进程接口，逐行读取 | 20,000 | 14.5620 ms |
| 线程接口，逐行读取 | 20,000 | 10.3070 ms |
| 线程接口，每 64 行抽样 | 314 | 7.5245 ms |

**读相同的 20,000 次**，换用线程接口后，整条查询的中位耗时下降 **29.2%**，24 轮里每一轮都更快。独立的小程序每批连续读取 100,000 次，测得单次读取的中位成本为线程接口 **121 ns**、进程接口 **366 ns**。[单次读取测试](doc/halo-pmc/read_cost.c) · [原始计时](doc/halo-pmc/results/read-cost.txt)

**用同一个线程接口**，每 64 行抽样把读取次数从 20,000 降到 314。这次测试中，抽样查询比关闭观测多用约 **0.09 ms**，代价是只覆盖了 157 行。

## 4. 怎么复现

在装有 Xcode Command Line Tools、Git、Python 3、readline 和 zlib 的 Apple Silicon Mac 上，从仓库根目录运行：

```bash
bash doc/halo-pmc/reproduce.sh
```

脚本编译当前 Git 提交，启动专用的本地数据库，生成两张表并完成测试，最后自动停库。终端会打印结果目录。打开 `hash-demo.txt` 看执行计划和硬件计数，`query-times.txt` 看四种模式每一轮的耗时，`read-cost.txt` 看两个读取接口的单次成本。[复现脚本](doc/halo-pmc/reproduce.sh)

# SylixOS DMA Engine 框架设计说明

## 1. 概述

本框架参照 Linux DMAengine 子系统（`drivers/dma/dmaengine.c`）设计，在 SylixOS 上提供统一的 DMA 抽象层。框架目标：

- **对齐 Linux 层级与抽象**：保持 `dma_device / dma_chan / dma_async_tx_descriptor` 三层模型不变。
- **使用 SylixOS 原生 API**：以 `LW_LIST_LINE`、`spinlock_t`、`API_SemaphoreBCreate`、`API_ThreadCreate` 等替代 Linux 内核原语。
- **硬件无关**：框架核心（`dmaengine.h / dmaengine.c`）不含任何寄存器操作，硬件驱动通过注册 ops 接入。

---

## 2. 文件结构

```text
src/
├── dma.c                   内核模块入口（module_init / module_exit）
├── dmaengine.h             框架核心头文件（数据结构 + 公开 API + inline helpers）
├── dmaengine.c             框架核心实现（全局设备链表管理、通道调度、状态查询）
├── hw/
│   └── fream/
│       ├── demoip.h        Demo IP 驱动公开头文件
│       └── demoip.c        Demo IP 纯软件模拟 DMA 驱动（memcpy + Slave SG，移植参考模板）
└── test/
    └── dma_test.c          tshell 测试套件（6 个命令：memcpy / multi / sg / loopback / loopback_fd / perf）
```

---

## 3. 对象模型

### 3.1 三层结构对比

| Linux 对象 | SylixOS 对应类型 | 说明 |
| --- | --- | --- |
| `struct dma_device` | `dma_device_t` | DMA 控制器，由硬件驱动填充并注册 |
| `struct dma_chan` | `dma_chan_t` | DMA 通道，嵌入驱动私有通道结构 |
| `struct dma_async_tx_descriptor` | `dma_async_tx_descriptor_t` | 异步传输描述符，由 `device_prep_xxx` 分配 |

### 3.2 核心结构示意

```text
dma_device_t                                dma_chan_t
┌─────────────────────────────┐            ┌──────────────────────────┐
│ dev_name[]                  │ 1        n │ chan_name[]              │
│ chancnt                     │◄───────────│ *device ─────────────────┤
│ channels (list head) ───────┼────────────► node (LW_LIST_LINE)      │
│ lock (spinlock_t)           │            │ cookie / completed_cookie│
│ node (LW_LIST_LINE) ────────┼──► 全局链表 │ *private                 │
│                             │            └──────────────────────────┘
│ ops:                        │
│  device_alloc_chan_resources│        dma_async_tx_descriptor_t
│  device_free_chan_resources │      ┌──────────────────────────┐
│  device_prep_dma_memcpy  ───┼──────► cookie / flags           │
│  device_prep_slave_sg       │      │ *chan                    │
│  device_config              │      │ tx_submit()              │
│  device_tx_status           │      │ callback_result()        │
│  device_issue_pending       │      │ callback_param           │
│  device_terminate_all       │      │ node (LW_LIST_LINE)      │
└─────────────────────────────┘      └──────────────────────────┘
```

---

## 4. 数据流与生命期

### 4.1 驱动注册流程

```text
硬件驱动 init()
  │
  ├─ 填充 dma_device_t（dev_name, chancnt, ops）
  ├─ 初始化通道数组，将各 dma_chan_t.node 加入 device->channels 链表
  └─ dma_async_device_register(device)
       └─ 校验必须 ops → 加入全局 _G_dma_device_list
```

### 4.2 通道申请/释放流程

```text
使用者
  │
  ├─ dma_request_chan_by_name("fream-dma", 0)
  │     └─ 遍历 _G_dma_device_list → 匹配 dev_name + idx
  │         └─ device_alloc_chan_resources(chan)  ← 驱动：初始化 worker 线程 + 信号量
  │
  │  ... 使用通道 ...
  │
  └─ dma_release_channel(chan)
        └─ device_free_chan_resources(chan)  ← 驱动：停止 worker 线程，清理队列
```

### 4.3 MEM_TO_MEM（memcpy）传输链路

```text
使用者
  │
  ├─ dmaengine_prep_dma_memcpy(chan, dst, src, len, flags)
  │     └─ device_prep_dma_memcpy()  ← 驱动：分配私有描述符，设置 tx_submit 指针
  │
  ├─ 设置 desc->callback_result = my_cb;
  │           desc->callback_param  = &my_completion;
  │
  ├─ dmaengine_submit(desc)
  │     └─ desc->tx_submit(desc)  ← 驱动：cookie_assign + 加入 pending 链表
  │         返回 cookie
  │
  ├─ dma_async_issue_pending(chan)
  │     └─ device_issue_pending(chan)  ← 驱动：pending→active + 唤醒 worker
  │
  │  [worker 线程]
  │     └─ lib_memcpy(dst, src, len)  ← demo: 软件模拟；真实: 等待 HW 完成中断
  │         dma_cookie_complete(desc)
  │         desc->callback_result(param, &result)  ← 通知使用者
  │
  └─ 使用者在回调中 post 信号量，主流程 pend 等待
```

### 4.4 Slave SG 传输链路

```text
使用者
  │
  ├─ 准备 dma_sg_entry_t sgl[]（addr + len 数组）
  │
  ├─ dmaengine_slave_config(chan, &cfg)
  │     └─ device_config()  ← 驱动：保存 slave_cfg（方向、设备地址、宽度、burst）
  │
  ├─ dmaengine_prep_slave_sg(chan, sgl, sg_len, dir, flags)
  │     └─ device_prep_slave_sg()  ← 驱动：拷贝 sgl，从 slave_cfg 取 dev_addr，
  │                                          分配含 sgl 副本的私有描述符
  │
  ├─ dmaengine_submit(desc) + dma_async_issue_pending(chan)
  │
  │  [worker 线程]
  │     └─ 遍历 sgl，逐条目执行 memcpy：
  │         MEM_TO_DEV: sgl[i].addr → dev_ptr（模拟写外设 FIFO）
  │         DEV_TO_MEM: dev_ptr → sgl[i].addr（模拟读外设 FIFO）
  │         dev_ptr += sgl[i].len（设备侧线性前进）
  │
  └─ callback_result 通知使用者
```

---

## 5. SylixOS API 映射

| Linux 原语                        | SylixOS 对应                               |
|-----------------------------------|--------------------------------------------|
| `spin_lock_irqsave`               | `LW_SPIN_LOCK_IRQ(&lock, &ireg)`           |
| `spin_unlock_irqrestore`          | `LW_SPIN_UNLOCK_IRQ(&lock, ireg)`          |
| `list_add_tail / list_del`        | `_List_Line_Add_Tail / _List_Line_Del`     |
| `container_of`                    | `_LIST_ENTRY(ptr, type, member)`           |
| `complete / wait_for_completion`  | `API_SemaphoreBPost / API_SemaphoreBPend`  |
| `kthread_create / kthread_stop`   | `API_ThreadCreate / dchan->stop + post sem`|
| `kmalloc / kfree`                 | `sys_malloc / sys_free`                    |
| `dma_alloc_coherent`              | `API_VmmDmaAlloc / API_VmmDmaFree`         |
| `printk`                          | `printk`（SylixOS 同名）                   |
| `devm_ioremap`                    | `API_VmmMap`（手动映射寄存器）             |
| `ktime_get` / `do_gettimeofday`   | `API_TimeGet64()`（64-bit tick 计数）      |

---

## 6. Demo IP 驱动说明

`hw/fream/demoip.c` 为纯软件模拟 DMA 控制器，无真实硬件依赖：

| 属性             | 值                                                |
|------------------|---------------------------------------------------|
| 设备名           | `"fream-dma"`                                     |
| 通道数           | 2（`ch0` / `ch1`，各有独立 worker 线程）          |
| 支持能力         | `DMA_MEMCPY` + `DMA_SLAVE`（Scatter-Gather）      |
| MEMCPY 实现      | `lib_memcpy`（worker 线程中执行）                 |
| Slave SG 实现    | 按 SG 条目线性遍历 `dev_addr`，模拟 FIFO 读写     |
| 每通道 slave_cfg | 独立保存（支持同时配置两个通道，用于回环测试）    |
| SG 最大条目数    | 64（`DEMOIP_SG_MAX`）                             |
| worker 优先级    | 120                                               |
| worker 栈大小    | 8192 字节                                         |
| 描述符内存       | `sys_malloc / sys_free`（驱动管理，含 sgl 副本）  |

### 私有描述符结构

```c
struct demoip_desc {
    dma_async_tx_descriptor_t  txd;        /* 公共描述符（必须第一） */
    demoip_op_t                op;         /* DEMOIP_OP_MEMCPY / DEMOIP_OP_SLAVE_SG */
    /* MEMCPY */
    phys_addr_t  src, dst;  size_t len;
    /* SLAVE_SG */
    dma_sg_entry_t          *sgl;          /* 驱动内部拷贝，prep 后用户可释放原 sgl */
    UINT                     sg_len;
    dma_transfer_direction_t direction;
    phys_addr_t              dev_addr;     /* 来自 slave_cfg.dst_addr / src_addr */
};
```

**移植为真实硬件驱动的步骤：**

1. 将 `__demoip_alloc_chan_resources` 中的信号量/线程初始化替换为中断注册（`API_InterVectorConnect`）。
2. 将 `__demoip_prep_slave_sg` 中的描述符结构扩展为硬件 BD（Buffer Descriptor）格式。
3. 将 `__demoip_issue_pending` 中的 `API_SemaphoreBPost` 替换为写硬件 tail pointer / doorbell 寄存器。
4. 将 worker 线程替换为 ISR + 底半部（`API_InterDeferJobAdd`），在底半部调用 `dma_cookie_complete` 和回调。

---

## 7. 测试命令说明

模块加载后（`insmod dma.ko`），在 tshell 中执行：

### 7.1 命令列表

| 命令 | 默认参数 | 说明 |
| --- | --- | --- |
| `dma_test_memcpy [size]` | size=4096 | 单次 MEM_TO_MEM memcpy 全链路（ch0） |
| `dma_test_multi [cnt] [ent]` | cnt=8 ent=512 | 批量提交多描述符，`dma_sync_wait` 等待（ch1） |
| `dma_test_sg [sg] [ent] [rx\|tx]` | sg=4 ent=512 rx | 单向 Slave SG 传输（ch0） |
| `dma_test_loopback [sg] [ent] [rounds]` | sg=4 ent=512 rounds=1 | 半双工回环：TX 完成后 RX 再开始（ch0+ch1 串行） |
| `dma_test_loopback_fd [sg] [ent] [rounds]` | sg=4 ent=512 rounds=1 | 全双工回环：TX ∥ RX 同时进行（ch0+ch1 并发） |
| `dma_test_perf` | — | 15 种配置全自动性能基准（memcpy/NIC/Block） |

### 7.2 半双工回环（`dma_test_loopback`）

TX 与 RX **串行**，TX 完成后 RX 才开始，ch0/ch1 共用同一 `dev_buf`：

```text
[src_flat] ──TX(ch0, MEM_TO_DEV)──► [dev_buf] ──RX(ch1, DEV_TO_MEM)──► [dst_flat]
verify: dst_flat == src_flat（逐 SG 条目比对）
输出：TX 吞吐 / RX 吞吐 / RTT 往返吞吐
```

典型用法：

```sh
dma_test_loopback 3  500   100    # ETH 1500B 帧回环 100 次（NIC 压力测试）
dma_test_loopback 1  4096  1000   # 4KB 块设备读写回环 1000 次（存储完整性）
dma_test_loopback 1  512   5000   # 512B 扇区高频回环（吞吐量基准）
```

### 7.3 全双工回环（`dma_test_loopback_fd`）

TX 与 RX **并发**，同时 `issue_pending`，ch0/ch1 使用**独立** `tx_fifo`/`rx_fifo`（无竞争）：

```text
[src_flat] ──TX(ch0, MEM_TO_DEV)──► [tx_fifo]          ← 各自独立的模拟设备 FIFO
[rx_fifo]  ──RX(ch1, DEV_TO_MEM)──► [dst_flat]
每轮 issue 前：lib_memcpy(rx_fifo, src_flat) 模拟硬件回环已将 TX 数据搬至 RX 侧
verify TX: tx_fifo == src_flat   verify RX: dst_flat == src_flat
输出：TX 吞吐 / RX 吞吐 / FD 双向合计吞吐（= 2×bytes / max_lat）
```

典型用法：

```sh
dma_test_loopback_fd 3  500   100   # ETH 1500B 帧全双工 100 次
dma_test_loopback_fd 1  4096  1000  # 4KB 块设备全双工读写 1000 次
dma_test_loopback_fd 1  65536  200  # 64KB 顺序全双工 IO
```

| 对比项 | 半双工（`dma_test_loopback`） | 全双工（`dma_test_loopback_fd`） |
| --- | --- | --- |
| 执行方式 | TX 完成 → RX 开始（串行） | TX ∥ RX 同时 issue（并发） |
| 设备缓冲 | 共用 `dev_buf`（顺序安全） | 独立 `tx_fifo` / `rx_fifo`（无竞争） |
| 等待机制 | `__dma_exec_once` × 2（共用 comp） | `__dma_exec_pair`（独立 comp_tx/comp_rx） |
| 延迟测量 | `t_tx + t_rx`（串行累加） | `max(t_tx, t_rx)`（并发取较慢者） |
| 适用场景 | 验证单通道数据正确性 | 验证全双工并发完整性及实际双向吞吐 |

### 7.4 性能基准场景（`dma_test_perf`）

自动运行 15 种配置，覆盖三类典型设备：

| 类别   | 场景示例                                           |
|--------|----------------------------------------------------|
| Memcpy | 64B 控制帧 / 512B / 4KB / 64KB / 1MB              |
| NIC    | ETH 标准帧 SG=3×500B / Jumbo SG=8×1125B（RX+TX） |
| Block  | 512B 扇区 / 4KB 页 / 128KB 顺序 / 512KB 大块      |

### 7.5 预期输出示例

```text
[dma_test] ===== memcpy  size=4096B  ch=0 =====
[dma_test]   src=0x...  dst=0x...  len=4096B
[dma_test]   time=1ms  4.0 MB/s
[dma_test]   verify OK
[dma_test] PASS

[dma_test] ===== loopback  sg=3  entry=500B  total=1500B  rounds=100 =====
[dma_test]   path: src_flat --TX(ch0)--> dev_buf --RX(ch1)--> dst_flat
[dma_test]   TX='fream-dma-ch0'  RX='fream-dma-ch1'  dev_buf=0x...
[dma_test]   TX  : time=105ms  1.3 MB/s
[dma_test]   RX  : time=102ms  1.4 MB/s
[dma_test]   RTT : time=207ms  0.6 MB/s
[dma_test]   verify: 100 rounds × 3 SG entries OK
[dma_test] PASS

[dma_test] ===== loopback-fd  sg=3  entry=500B  total=1500B  rounds=100 =====
[dma_test]   mode: FULL-DUPLEX (TX || RX concurrent, separate FIFOs)
[dma_test]   TX='fream-dma-ch0'  tx_fifo=0x...
[dma_test]   RX='fream-dma-ch1'  rx_fifo=0x...  (pre-loaded each round)
[dma_test]   TX  : time=106ms  1.3 MB/s
[dma_test]   RX  : time=106ms  1.3 MB/s
[dma_test]   FD  : time=106ms  2.6 MB/s
[dma_test]   verify: 100 rounds × 3 SG entries (TX+RX both sides) OK
[dma_test] PASS
```

---

## 8. 关键设计决策

### 8.1 描述符生命期

描述符由驱动的 `device_prep_xxx` 分配（`sys_malloc`），在 worker 线程执行完回调后由驱动的 `__demoip_desc_free` 释放（同时释放 Slave SG 的 `sgl` 副本）。使用者在 `dmaengine_submit` 后不得再访问描述符指针。对应 Linux 中设置 `DMA_CTRL_ACK` 标志的语义。

**未提交描述符的释放**：框架无独立 `free_desc` 接口。若 prep 成功但随后决定取消，需先 `dmaengine_submit` 将描述符入队，再调用 `dmaengine_terminate_all` 由驱动在清链表时统一释放，避免内存泄漏。

### 8.2 Cookie 机制

`dma_cookie_t` 为单调递增整数，通道级管理：

- `chan->cookie`：最后一次 `dma_cookie_assign` 赋值（已提交的最新事务）。
- `chan->completed_cookie`：最后一次 `dma_cookie_complete` 更新（已完成的最新事务）。
- `dma_cookie_status(chan, cookie, state)`：通过比较两个 cookie 推断状态，无需遍历队列。

### 8.3 pending → active 两阶段队列

与 Linux pl330/dw_dmac 等驱动一致：

- `pending`：`dmaengine_submit` 后入队，此时不执行。
- `active`：`dma_async_issue_pending` 触发后从 pending 移入，worker 线程依次处理。

两阶段设计允许使用者批量准备多个描述符后一次性触发，减少 issue 开销。

### 8.4 等待机制

提供三种等待机制：

| 方式 | 实现 | 适用场景 |
| --- | --- | --- |
| 异步回调（推荐） | `callback_result` + 二进制信号量 | 生产代码，不阻塞调用线程 |
| 同步轮询 | `dma_sync_wait`（1 tick 间隔轮询） | 调试、简单脚本（`dma_test_multi` 使用） |
| 并发双路等待 | `__dma_exec_pair`（两个独立 sem，顺序 pend） | 全双工测试，无需额外线程 |

`__dma_exec_once` 封装单路流程：`API_SemaphoreBClear`（清除历史 post）→ submit → issue → `API_SemaphoreBPend`（5 s 超时）。

`__dma_exec_pair` 封装双路流程：清除两个 sem → 同时 submit 两路 → 同时 `issue_pending` → 顺序 pend 两个 sem。计时从双路 issue 到两者均完成，反映全双工实际延迟 `≈ max(t_tx, t_rx)`。任一超时则对两通道均调用 `dmaengine_terminate_all`。

### 8.5 Slave SG 设备地址管理

驱动在 `device_prep_slave_sg` 时从通道的 `slave_cfg` 读取设备地址：

- `MEM_TO_DEV`：`dev_addr = slave_cfg.dst_addr`
- `DEV_TO_MEM`：`dev_addr = slave_cfg.src_addr`

每通道 `slave_cfg` 独立存储，支持 ch0/ch1 配置不同方向和地址。两种回环测试都利用此特性：

- 半双工：ch0 `dst_addr=dev_buf`，ch1 `src_addr=dev_buf`（共用，TX→RX 串行）
- 全双工：ch0 `dst_addr=tx_fifo`，ch1 `src_addr=rx_fifo`（独立，并发无竞争）

### 8.6 计时精度与吞吐计算

计时使用 `API_TimeGet64()` 返回 64-bit tick 计数，差值 `ticks = t1 - t0`。

**精度限制**：测量粒度为 1 tick（默认 1 ms @ 1000 Hz）。传输在同一 tick 内完成时 `ticks = 0`，此时无法计算吞吐量，打印 `N/A`。软件模拟 DMA 的小包传输（< 1 ms）通常落入此情形，属正常现象。

**吞吐公式**：`KB/s = bytes × tickrate / (ticks × 1024)`

中间积 `bytes × tickrate` 在 tickrate ≤ 10 000 Hz 时最大约 5.4 × 10¹⁷，不超过 UINT64 上限（1.8 × 10¹⁹）。移植到 tickrate > 300 kHz 的平台时，需将乘法改为先除后乘（`bytes/1024 × tickrate / ticks`）以避免溢出。

---

## 9. 编译说明

工程需将以下文件加入编译：

```text
src/dmaengine.c
src/hw/fream/demoip.c
src/test/dma_test.c
src/dma.c
```

头文件包含路径需覆盖 `src/`，使 `hw/fream/demoip.c` 能以 `../../dmaengine.h` 找到框架头文件，`test/dma_test.c` 能以 `../dmaengine.h` 找到框架头文件。

> 实际接口由 `SylixOS.h` 提供，`drv/os_common.h` 仅为开发阶段参考，不包含在工程编译中。

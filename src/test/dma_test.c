/*********************************************************************************************************
**
**                                    中国软件开源组织
**
**                                   嵌入式实时操作系统
**
**                                SylixOS(TM)  LW : long wing
**
**                               Copyright All Rights Reserved
**
**--------------文件信息--------------------------------------------------------------------------------
**
** 文   件   名: dma_test.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA Engine 框架运行时测试套件
**
**  设计原则：
**    1. 仅依赖框架公开 API（dmaengine.h），不直接访问驱动私有结构，避免硬件耦合。
**    2. 使用 sys_malloc 缓冲区；demo 驱动将 phys_addr_t 视为虚拟地址，无需 DMA 物理地址映射。
**    3. 单路异步等待：callback_result + 二进制信号量，由 __dma_exec_once 统一封装。
**       每次调用前 API_SemaphoreBClear 清除历史 post，防止多轮复用时信号量状态残留。
**    4. 双路并发等待：__dma_exec_pair 同时 issue 两个通道，内部使用共享屏障信号量；
**       两通道 worker 线程天然并发执行，任一完成时递减计数，最后完成者 post 信号量，
**       主线程仅需一次 API_SemaphoreBPend，消除双路顺序等待的额外上下文切换开销。
**    5. 计时：API_TimeGet64() 返回 64-bit tick 计数，差值 ticks=0 表示同 tick 内完成，
**       此时输出 N/A；ticks>0 时吞吐量 = bytes × tickrate / (ticks × 1024) KB/s。
**    6. 描述符生命期：prep 成功后须完成 submit；若 submit 前 prep 的另一路失败，
**       需将已 prep 的描述符 submit 到 pending 队列后立即 terminate_all，
**       由驱动在 terminate_all 中统一释放，避免泄漏。
**    7. BER 误码率：传输完成后逐字节比对参考数据与目标数据，统计不匹配字节数。
**       单次测试（memcpy/multi/sg）输出单次 BER；loopback_fd 跨全部轮次累计
**       TX/RX 独立 BER，单轮失败不中止，最终输出累计结果。
**       perf 基准不做逐字节校验（吞吐优先），传输执行失败计入 error_rounds，
**       继续运行至时间耗尽，汇总输出 errors=N 和 [err=N] 标注。
**
**  tshell 命令（共 5 条）：
**    dma_test_memcpy      [size]                    单次 MEM_TO_MEM memcpy（默认 4096B，ch0），输出 BER
**    dma_test_multi       [count] [entry]           批量多描述符 + dma_sync_wait（默认 8×512B，ch1），输出 BER
**    dma_test_sg          [sg] [entry] [rx|tx]      单向 Slave SG 传输（默认 4×512B rx，ch0），输出 BER
**    dma_test_loopback_fd [sg] [entry] [rounds]     全双工回环：TX ∥ RX 并发（默认 4×512B×1），累计 TX/RX BER
**                                                   - 测试目的：端到端全双工可用带宽（2 通道同时工作）
**    dma_test_perf        [duration_sec]            单通道极限带宽基准（15 种配置，每场景计时运行）
**                                                   - 测试目的：单通道理论峰值吞吐（1 通道独占 CPU）
**                                                   - 每秒输出中间吞吐报告
**                                                   - 场景结束输出单场景摘要（含 errors=N）
**                                                   - 全部完成后输出汇总表（失败场景追加 [err=N]）
**
*********************************************************************************************************/
#define  __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include <string.h>
#include "../dmaengine.h"

// #define TEST_DEV_NAME                   "demoip-dma"
#define TEST_DEV_NAME                   "xilinx-dma"
#define TEST_SG_MAX                     64

#ifndef LW_CFG_TICKS_PER_SEC
#define LW_CFG_TICKS_PER_SEC            1000
#endif

#define TICKS_TO_MS(t)      ((UINT64)(t) * 1000ULL / LW_CFG_TICKS_PER_SEC)

/*
 *  THROUGHPUT_KBS: bytes * tickrate / (ticks * 1024)
 *  中间积 bytes * tickrate 在 tickrate ≤ 10000 Hz 时不会溢出 UINT64
 *  （max: 64 * 4MB * 100000 * 2 * 10000 ≈ 5.4×10^17 < UINT64_MAX 1.8×10^19）。
 *  若移植到 tickrate > 300 kHz 的平台，需将乘法改为先除后乘以避免溢出。
 */
#define THROUGHPUT_KBS(b,t) ((t) ? ((UINT64)(b) * LW_CFG_TICKS_PER_SEC / ((UINT64)(t) * 1024ULL)) : 0ULL)

/*================================================ 公共辅助层 ============================================*/

typedef struct {
    LW_OBJECT_HANDLE    sem;
    dma_status_t        status;
    UINT32              residue;
} dma_completion_t;

/*********************************************************************************************************
** 函数名称: __dma_complete_cb
** 功能描述: 单路传输完成回调（记录状态并 post 信号量）
** 输　入  : param  — dma_completion_t 指针
**           result — 传输结果
** 输　出  : NONE
*********************************************************************************************************/

static VOID  __dma_complete_cb (PVOID param, const dmaengine_result_t *result)
{
    dma_completion_t  *comp = (dma_completion_t *)param;
    comp->status  = result->result;
    comp->residue = result->residue;
    API_SemaphoreBPost(comp->sem);
}

/*********************************************************************************************************
** 函数名称: __dma_completion_init
** 功能描述: 初始化单路完成等待结构（创建信号量）
** 输　入  : comp — 完成等待结构指针
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/

static INT  __dma_completion_init (dma_completion_t *comp)
{
    lib_memset(comp, 0, sizeof(*comp));
    comp->sem = API_SemaphoreBCreate("dma_wait", LW_FALSE, LW_OPTION_OBJECT_LOCAL, LW_NULL);
    if (comp->sem == LW_OBJECT_HANDLE_INVALID) { return  (-1); }
    comp->status = DMA_IN_PROGRESS;
    return  (0);
}

/*********************************************************************************************************
** 函数名称: __dma_completion_destroy
** 功能描述: 销毁单路完成等待结构（删除信号量）
** 输　入  : comp — 完成等待结构指针
** 输　出  : NONE
*********************************************************************************************************/

static VOID  __dma_completion_destroy (dma_completion_t *comp)
{
    if (comp->sem != LW_OBJECT_HANDLE_INVALID) {
        API_SemaphoreBDelete(&comp->sem);
        comp->sem = LW_OBJECT_HANDLE_INVALID;
    }
}

/*********************************************************************************************************
** 函数名称: __dma_exec_once
** 功能描述: 统一执行单次传输：绑定回调 → submit → issue → wait（5s 超时）
** 输　入  : chan    — 目标通道
**           desc    — 已准备好的描述符（callback 由本函数设置）
**           comp    — 完成等待结构（本函数内清信号量后重用）
**           p_ticks — 输出耗时 ticks（可为 LW_NULL）
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/

static INT  __dma_exec_once (dma_chan_t *chan, dma_async_tx_descriptor_t *desc,
                              dma_completion_t *comp, UINT64 *p_ticks)
{
    UINT64  t0, t1;

    API_SemaphoreBClear(comp->sem);
    comp->status          = DMA_IN_PROGRESS;
    desc->callback_result = __dma_complete_cb;
    desc->callback_param  = comp;

    if (dmaengine_submit(desc) == DMA_COOKIE_INVALID) {
        return  (-1);
    }
    t0 = API_TimeGet64();
    dma_async_issue_pending(chan);

    if (API_SemaphoreBPend(comp->sem, LW_MSECOND_TO_TICK_1(5000)) != ERROR_NONE) {
        printk("[dma_test]   FAIL: timeout\n");
        if (p_ticks) { *p_ticks = 0; }
        return  (-1);
    }
    t1 = API_TimeGet64();
    if (p_ticks) { *p_ticks = t1 - t0; }
    return  (comp->status == DMA_COMPLETE) ? 0 : -1;
}

/*================================================ 全双工双路同步屏障 ====================================*/

/*
 *  原实现对 comp_a / comp_b 各执行一次 API_SemaphoreBPend，两次顺序 pend 各需一次
 *  完整上下文切换往返（~10~15 μs），使全双工每路有效吞吐降至 FD 吞吐的一半。
 *  改为共享屏障：两路回调各自递减 remaining，降至 0 时 post 一次共享信号量，
 *  主线程仅 pend 一次，两路切换开销合并为一，每路吞吐恢复至与单通道相当。
 */
typedef struct {
    LW_OBJECT_HANDLE  sem;          /*  共享信号量：两路均完成后 post 一次              */
    spinlock_t        lock;         /*  保护 remaining 的自旋锁                         */
    INT               remaining;   /*  倒计时：初始=2，每路回调递减 1，降至 0 时 post  */
    dma_status_t      status_a;    /*  通道 A 完成状态（cb_a 填写）                     */
    dma_status_t      status_b;    /*  通道 B 完成状态（cb_b 填写）                     */
} dma_pair_sync_t;

/*********************************************************************************************************
** 函数名称: __dma_pair_cb_a
** 功能描述: 通道 A 完成回调（屏障计数递减，最后完成者 post 信号量）
** 输　入  : param  — dma_pair_sync_t 指针
**           result — 传输结果
** 输　出  : NONE
*********************************************************************************************************/

static VOID  __dma_pair_cb_a (PVOID param, const dmaengine_result_t *result)
{
    dma_pair_sync_t  *sync = (dma_pair_sync_t *)param;
    INTREG            ireg;

    LW_SPIN_LOCK_IRQ(&sync->lock, &ireg);
    sync->status_a = result->result;
    if (--sync->remaining == 0) {
        LW_SPIN_UNLOCK_IRQ(&sync->lock, ireg);
        API_SemaphoreBPost(sync->sem);
    } else {
        LW_SPIN_UNLOCK_IRQ(&sync->lock, ireg);
    }
}

/*********************************************************************************************************
** 函数名称: __dma_pair_cb_b
** 功能描述: 通道 B 完成回调（屏障计数递减，最后完成者 post 信号量）
** 输　入  : param  — dma_pair_sync_t 指针
**           result — 传输结果
** 输　出  : NONE
*********************************************************************************************************/

static VOID  __dma_pair_cb_b (PVOID param, const dmaengine_result_t *result)
{
    dma_pair_sync_t  *sync = (dma_pair_sync_t *)param;
    INTREG            ireg;

    LW_SPIN_LOCK_IRQ(&sync->lock, &ireg);
    sync->status_b = result->result;
    if (--sync->remaining == 0) {
        LW_SPIN_UNLOCK_IRQ(&sync->lock, ireg);
        API_SemaphoreBPost(sync->sem);
    } else {
        LW_SPIN_UNLOCK_IRQ(&sync->lock, ireg);
    }
}

/*********************************************************************************************************
** 函数名称: __dma_pair_sync_init
** 功能描述: 初始化全双工同步屏障（创建共享信号量、初始化自旋锁）
** 输　入  : sync — 同步屏障结构指针
** 输　出  : 0 成功；-1 失败
** 说明    : 调用方在循环外初始化一次，循环内通过 __dma_exec_pair 复用，避免每轮创建/销毁开销
*********************************************************************************************************/

static INT  __dma_pair_sync_init (dma_pair_sync_t *sync)
{
    lib_memset(sync, 0, sizeof(*sync));
    LW_SPIN_INIT(&sync->lock);
    sync->sem = API_SemaphoreBCreate("dma_pair", LW_FALSE, LW_OPTION_OBJECT_LOCAL, LW_NULL);
    if (sync->sem == LW_OBJECT_HANDLE_INVALID) { return  (-1); }
    return  (0);
}

/*********************************************************************************************************
** 函数名称: __dma_pair_sync_destroy
** 功能描述: 销毁全双工同步屏障（删除共享信号量）
** 输　入  : sync — 同步屏障结构指针
** 输　出  : NONE
*********************************************************************************************************/

static VOID  __dma_pair_sync_destroy (dma_pair_sync_t *sync)
{
    if (sync->sem != LW_OBJECT_HANDLE_INVALID) {
        API_SemaphoreBDelete(&sync->sem);
        sync->sem = LW_OBJECT_HANDLE_INVALID;
    }
}

/*********************************************************************************************************
** 函数名称: __dma_exec_pair
** 功能描述: 并发执行两路传输（全双工）：同时 submit+issue 两路，共享屏障信号量等待双路完成
** 输　入  : chan_a/b   — 两个独立通道（不得相同）
**           desc_a/b   — 各自已准备好的描述符（callback 由本函数设置）
**           sync       — 调用方预分配的同步屏障（循环外初始化一次，循环内复用）
**           p_ticks    — 输出总耗时（从两路同时 issue 到均完成，反映全双工实际延迟）
** 输　出  : 0 成功；-1 失败
** 说明    : 两路 worker 线程真正并发；末完成的一路 post 共享信号量，主线程仅一次
**           API_SemaphoreBPend，计时 t1-t0 ≈ max(t_a, t_b)。
**           若超时，对两通道均调用 dmaengine_terminate_all 确保状态干净。
**           sync 复用：本函数内通过 API_SemaphoreBClear + remaining 重置实现，
**           无需每轮创建/销毁信号量，消除内核对象管理开销。
*********************************************************************************************************/

static INT  __dma_exec_pair (dma_chan_t                *chan_a,
                              dma_async_tx_descriptor_t *desc_a,
                              dma_chan_t                *chan_b,
                              dma_async_tx_descriptor_t *desc_b,
                              dma_pair_sync_t           *sync,
                              UINT64                    *p_ticks)
{
    UINT64  t0, t1;

    API_SemaphoreBClear(sync->sem);                                     /*  清除上轮残留 post            */
    sync->remaining = 2;
    sync->status_a  = DMA_IN_PROGRESS;
    sync->status_b  = DMA_IN_PROGRESS;

    desc_a->callback_result = __dma_pair_cb_a;
    desc_a->callback_param  = sync;
    desc_b->callback_result = __dma_pair_cb_b;
    desc_b->callback_param  = sync;

    if (dmaengine_submit(desc_a) == DMA_COOKIE_INVALID ||
        dmaengine_submit(desc_b) == DMA_COOKIE_INVALID) {
        printk("[dma_test]   FAIL: submit\n");
        return  (-1);
    }

    t0 = API_TimeGet64();
    dma_async_issue_pending(chan_a);
    dma_async_issue_pending(chan_b);                                     /*  两路同时触发，真正并发      */

    if (API_SemaphoreBPend(sync->sem, LW_MSECOND_TO_TICK_1(5000)) != ERROR_NONE) {
        printk("[dma_test]   FAIL: timeout\n");
        dmaengine_terminate_all(chan_a);
        dmaengine_terminate_all(chan_b);
        if (p_ticks) { *p_ticks = 0; }
        return  (-1);
    }
    t1 = API_TimeGet64();
    if (p_ticks) { *p_ticks = t1 - t0; }

    if (sync->status_a != DMA_COMPLETE || sync->status_b != DMA_COMPLETE) {
        printk("[dma_test]   FAIL: status a=%d b=%d\n",
               (INT)sync->status_a, (INT)sync->status_b);
        return  (-1);
    }
    return  (0);
}

/*********************************************************************************************************
** 函数名称: __dma_slave_cfg
** 功能描述: 简化 Slave 通道配置（自动按方向填 src/dst 地址，固定 4B 宽度 / burst=16）
** 输　入  : chan    — 目标通道
**           dir     — DMA_MEM_TO_DEV 或 DMA_DEV_TO_MEM
**           dev_buf — 模拟设备缓冲区地址
** 输　出  : 0 成功；-1 失败
** 说明    : 真实硬件驱动中，总线宽度和 burst 应按外设手册配置。
*********************************************************************************************************/

static INT  __dma_slave_cfg (dma_chan_t *chan, dma_transfer_direction_t dir, PVOID dev_buf)
{
    dma_slave_config_t  cfg;

    lib_memset(&cfg, 0, sizeof(cfg));
    cfg.direction = dir;
    if (dir == DMA_MEM_TO_DEV) {
        cfg.dst_addr       = (phys_addr_t)(addr_t)dev_buf;
        cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
        cfg.dst_maxburst   = 16;
    } else {
        cfg.src_addr       = (phys_addr_t)(addr_t)dev_buf;
        cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
        cfg.src_maxburst   = 16;
    }
    return  dmaengine_slave_config(chan, &cfg);
}

/*********************************************************************************************************
** 函数名称: __print_throughput
** 功能描述: 格式化输出吞吐量（自动选择 KB/s 或 MB/s 单位）
** 输　入  : bytes — 传输字节数
**           ticks — 耗时 tick 数
** 输　出  : NONE
** 说明    : ticks=0 时输出 N/A；≥1024 KB/s 时显示 MB/s（小数点后 1 位）
*********************************************************************************************************/

static VOID  __print_throughput (UINT64 bytes, UINT64 ticks)
{
    UINT64  ms  = TICKS_TO_MS(ticks);
    UINT64  kbs = THROUGHPUT_KBS(bytes, ticks);

    if (ticks == 0) {                                                   /*  真正无法计时（同 tick 内完成）*/
        printk("time=0tick N/A\n");
        return;
    }
    if (kbs >= 1024) {
        printk("time=%llums  %llu.%llu MB/s\n",
               (unsigned long long)ms,
               (unsigned long long)(kbs / 1024),
               (unsigned long long)((kbs % 1024) * 10 / 1024));
    } else {
        printk("time=%llums  %llu KB/s\n",
               (unsigned long long)ms, (unsigned long long)kbs);
    }
}

/*================================================ BER 误码率统计 ========================================*/

typedef struct {
    UINT64  total_bytes;                                                /*  参与校验的总字节数          */
    UINT64  error_bytes;                                                /*  与参考数据不一致的字节数    */
} dma_ber_stat_t;

/*********************************************************************************************************
** 函数名称: __dma_count_errors
** 功能描述: 逐字节比对参考缓冲区与被测缓冲区，返回不匹配字节数
** 输　入  : ref — 参考数据指针
**           dut — 被测数据指针
**           len — 比对字节数
** 输　出  : 不匹配字节数（0 表示完全一致）
*********************************************************************************************************/

static UINT64  __dma_count_errors (const UINT8 *ref, const UINT8 *dut, size_t len)
{
    UINT64  errors = 0;
    size_t  i;

    /*
     *  快速路径：lib_memcmp 通常由 libc 以 SIMD 实现，处理无误码的正常情况
     *  速度比字节循环快 1~2 个数量级（4KB 约 1 μs vs ~25 μs）。
     *  仅在检测到不一致时才进入慢路径逐字节统计精确误码数。
     */
    if (lib_memcmp(ref, dut, len) == 0) { return  0; }

    for (i = 0; i < len; i++) {
        if (ref[i] != dut[i]) { errors++; }
    }
    return  errors;
}

/*********************************************************************************************************
** 函数名称: __print_ber
** 功能描述: 格式化输出 BER 统计：0 误码直接显示 OK；非 0 误码以 ppm 精度输出百分比
** 输　入  : label — 标签字符串（如 "BER" / "TX BER"）
**           stat  — BER 统计结构指针
** 输　出  : NONE
** 说明    : ppm = error_bytes × 1000000 / total_bytes，显示到小数点后 2 位
**           （即 ppm / 10000 整数部分 + (ppm % 10000) / 100 小数部分）
*********************************************************************************************************/

static VOID  __print_ber (CPCHAR label, const dma_ber_stat_t *stat)
{
    if (stat->total_bytes == 0) {
        printk("[dma_test]   %-8s  N/A\n", label);
        return;
    }
    if (stat->error_bytes == 0) {
        printk("[dma_test]   %-8s  0 errors / %llu B  (0%%)\n",
               label, (unsigned long long)stat->total_bytes);
    } else {
        UINT64  ppm = stat->error_bytes * 1000000ULL / stat->total_bytes;
        printk("[dma_test]   %-8s  %llu errors / %llu B  (%llu.%02llu%%)\n",
               label,
               (unsigned long long)stat->error_bytes,
               (unsigned long long)stat->total_bytes,
               (unsigned long long)(ppm / 10000),
               (unsigned long long)((ppm % 10000) / 100));
    }
}

/*================================================ 测试用例: memcpy ======================================*/

/*
 *  单次 MEM_TO_MEM memcpy 全链路测试（ch0）
 *  验证：prep → submit → issue → callback → data verify
 */
static INT  __dma_test_memcpy_run (size_t size)
{
    dma_chan_t               *chan = LW_NULL;
    UINT8                    *src  = LW_NULL, *dst = LW_NULL;
    dma_async_tx_descriptor_t *desc;
    dma_completion_t          comp;
    UINT64                    ticks;
    size_t                    i;
    INT                       ret = -1;

    printk("\n[dma_test] ===== memcpy  size=%zuB  ch=0 =====\n", size);

    chan = dma_request_chan_by_name(TEST_DEV_NAME, 0);
    src  = chan ? (UINT8 *)sys_malloc(size) : LW_NULL;
    dst  = src  ? (UINT8 *)sys_malloc(size) : LW_NULL;
    if (!dst) { printk("[dma_test]   FAIL: alloc\n"); goto  _out; }

    for (i = 0; i < size; i++) { src[i] = (UINT8)((i * 0x37 + 0xA5) & 0xFF); }
    lib_memset(dst, 0xAA, size);

    if (__dma_completion_init(&comp) != 0) { goto  _out; }

    desc = dmaengine_prep_dma_memcpy(chan,
                                      (phys_addr_t)(addr_t)dst,
                                      (phys_addr_t)(addr_t)src,
                                      size, DMA_PREP_INTERRUPT);
    if (!desc) { printk("[dma_test]   FAIL: prep\n"); goto  _out_comp; }

    printk("[dma_test]   src=%p  dst=%p  len=%zuB\n", src, dst, size);
    if (__dma_exec_once(chan, desc, &comp, &ticks) != 0) { goto  _out_comp; }

    printk("[dma_test]   ");
    __print_throughput((UINT64)size, ticks);

    {
        dma_ber_stat_t  ber;
        ber.total_bytes = (UINT64)size;
        ber.error_bytes = __dma_count_errors(src, dst, size);
        __print_ber("BER", &ber);
        ret = (ber.error_bytes == 0) ? 0 : -1;
    }

_out_comp:
    __dma_completion_destroy(&comp);
_out:
    if (src)  { sys_free(src); }
    if (dst)  { sys_free(dst); }
    if (chan) { dma_release_channel(chan); }
    printk("[dma_test] %s\n", ret ? "FAIL" : "PASS");
    return  ret;
}

/*================================================ 测试用例: multi =======================================*/

/*
 *  批量提交 count 个 memcpy 描述符（ch1），验证顺序执行和 cookie 跟踪
 *  所有描述符一次性 issue，用 dma_sync_wait 等待最后一个 cookie
 */
static INT  __dma_test_multi_run (UINT count, size_t entry_size)
{
    dma_chan_t               *chan = LW_NULL;
    UINT8                    *src  = LW_NULL, *dst = LW_NULL;
    dma_async_tx_descriptor_t *desc;
    dma_cookie_t              last_cookie = DMA_COOKIE_INVALID;
    UINT64                    t0, t1;
    UINT                      i;
    size_t                    total = (size_t)count * entry_size;
    INT                       ret   = -1;

    printk("\n[dma_test] ===== multi  count=%u  entry=%zuB  ch=1 =====\n", count, entry_size);

    chan = dma_request_chan_by_name(TEST_DEV_NAME, 1);
    src  = chan ? (UINT8 *)sys_malloc(total) : LW_NULL;
    dst  = src  ? (UINT8 *)sys_malloc(total) : LW_NULL;
    if (!dst) { printk("[dma_test]   FAIL: alloc\n"); goto  _out; }

    for (i = 0; i < total; i++) { src[i] = (UINT8)(i ^ 0x5A); }
    lib_memset(dst, 0, total);

    t0 = API_TimeGet64();
    for (i = 0; i < count; i++) {
        desc = dmaengine_prep_dma_memcpy(chan,
                                          (phys_addr_t)(addr_t)(dst + i * entry_size),
                                          (phys_addr_t)(addr_t)(src + i * entry_size),
                                          entry_size, 0);
        if (!desc) { printk("[dma_test]   FAIL: prep[%u]\n", i); goto  _out; }
        last_cookie = dmaengine_submit(desc);
    }
    dma_async_issue_pending(chan);
    printk("[dma_test]   %u descs issued  last_cookie=%d\n", count, last_cookie);

    if (dma_sync_wait(chan, last_cookie) != DMA_COMPLETE) {
        printk("[dma_test]   FAIL: dma_sync_wait\n");
        goto  _out;
    }
    t1 = API_TimeGet64();

    printk("[dma_test]   ");
    __print_throughput((UINT64)total, t1 - t0);

    {
        dma_ber_stat_t  ber;
        ber.total_bytes = (UINT64)total;
        ber.error_bytes = __dma_count_errors(src, dst, total);
        __print_ber("BER", &ber);
        ret = (ber.error_bytes == 0) ? 0 : -1;
    }

_out:
    if (src)  { sys_free(src); }
    if (dst)  { sys_free(dst); }
    if (chan) { dma_release_channel(chan); }
    printk("[dma_test] %s\n", ret ? "FAIL" : "PASS");
    return  ret;
}

/*================================================ 测试用例: slave SG ==================================*/

/*
 *  Slave SG 单向传输测试（ch0）
 *  rx (DEV_TO_MEM): 预填 dev_buf → DMA 读到 sg_bufs → 逐条目校验
 *  tx (MEM_TO_DEV): 预填 sg_bufs → DMA 写到 dev_buf → 逐条目校验
 */
static INT  __dma_test_sg_run (UINT sg_len, size_t entry_size,
                                dma_transfer_direction_t dir)
{
    dma_chan_t               *chan    = LW_NULL;
    UINT8                    *dev_buf = LW_NULL;
    UINT8                    *flat    = LW_NULL;                        /*  平铺 SG 缓冲（sg_len×entry）*/
    dma_sg_entry_t           *sgl     = LW_NULL;
    dma_async_tx_descriptor_t *desc;
    dma_completion_t          comp;
    UINT64                    ticks;
    size_t                    total   = (size_t)sg_len * entry_size;
    UINT                      i;
    size_t                    j;
    INT                       ret     = -1;

    printk("\n[dma_test] ===== slave SG  sg=%u  entry=%zuB  total=%zuB  dir=%s =====\n",
           sg_len, entry_size, total,
           (dir == DMA_MEM_TO_DEV) ? "MEM_TO_DEV(tx)" : "DEV_TO_MEM(rx)");

    chan    = dma_request_chan_by_name(TEST_DEV_NAME, 0);
    dev_buf = chan ? (UINT8 *)sys_malloc(total) : LW_NULL;
    flat    = dev_buf ? (UINT8 *)sys_malloc(total) : LW_NULL;
    sgl     = flat   ? (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(*sgl)) : LW_NULL;
    if (!sgl) { printk("[dma_test]   FAIL: alloc\n"); goto  _out; }

    /* 数据初始化（flat 平铺，每条目切片作为 SG addr） */
    if (dir == DMA_DEV_TO_MEM) {
        for (j = 0; j < total; j++) { dev_buf[j] = (UINT8)((j * 0x13 + 0x55) & 0xFF); }
        lib_memset(flat, 0xAA, total);
    } else {
        for (j = 0; j < total; j++) { flat[j] = (UINT8)((j * 0x1D + 0x33) & 0xFF); }
        lib_memset(dev_buf, 0, total);
    }

    for (i = 0; i < sg_len; i++) {
        sgl[i].addr = (phys_addr_t)(addr_t)(flat + (size_t)i * entry_size);
        sgl[i].len  = entry_size;
    }

    printk("[dma_test]   slave_config: dev_buf=%p  width=4B  burst=16\n", dev_buf);
    if (__dma_slave_cfg(chan, dir, dev_buf) != 0) {
        printk("[dma_test]   FAIL: slave_config\n"); goto  _out;
    }

    if (__dma_completion_init(&comp) != 0) { goto  _out; }

    desc = dmaengine_prep_slave_sg(chan, sgl, sg_len, dir, DMA_PREP_INTERRUPT);
    if (!desc) { printk("[dma_test]   FAIL: prep\n"); goto  _out_comp; }

    if (__dma_exec_once(chan, desc, &comp, &ticks) != 0) { goto  _out_comp; }

    printk("[dma_test]   ");
    __print_throughput((UINT64)total, ticks);

    {
        dma_ber_stat_t  ber;
        ber.total_bytes = (UINT64)total;
        ber.error_bytes = __dma_count_errors(dev_buf, flat, total);
        __print_ber("BER", &ber);
        printk("[dma_test]   verify: %u SG entries  total=%zuB\n", sg_len, total);
        ret = (ber.error_bytes == 0) ? 0 : -1;
    }

_out_comp:
    __dma_completion_destroy(&comp);
_out:
    if (sgl)     { sys_free(sgl); }
    if (flat)    { sys_free(flat); }
    if (dev_buf) { sys_free(dev_buf); }
    if (chan)    { dma_release_channel(chan); }
    printk("[dma_test] %s\n", ret ? "FAIL" : "PASS");
    return  ret;
}

/*================================================ 测试用例: 全双工回环 ===================================*/

/*********************************************************************************************************
** 函数名称: __dma_test_loopback_fd_run
** 功能描述: 全双工双通道 Slave SG 回环测试（TX / RX 并发执行）
** 输　入  : sg_len     — SG 条目数
**           entry_size — 每条目字节数
**           rounds     — 回环轮数
** 输　出  : 0 PASS；-1 FAIL
**
**  回环路径（全双工，两路并发）：
**    [src_flat] ──TX(ch0, MEM_TO_DEV)──► [AXI Stream 总线]──► S2MM(ch1, DEV_TO_MEM)──► [dst_flat]
**
**    硬件回环：MM2S 发出的 AXI Stream 数据由硬件直接回环到 S2MM 接收端。
**    软件不需要预填充任何中间 FIFO，TX 和 RX 同时 issue，硬件保证数据路径。
**
**  校验：
**    RX 侧：dst_flat == src_flat（硬件回环后数据完整性校验）
**    TX 侧：不单独校验（MM2S 无目标内存地址，数据发往 Stream 总线）
**
**  吞吐统计（挂钟时间，与 dma_test_perf 口径一致）：
**    TX  ≈ total×rounds / t_wall
**    RX  ≈ total×rounds / t_wall
**    FD  = 2×total×rounds / t_wall  （双向合计，体现全双工收益）
**
*********************************************************************************************************/

static INT  __dma_test_loopback_fd_run (UINT sg_len, size_t entry_size, UINT rounds)
{
    dma_chan_t               *ch_tx    = LW_NULL, *ch_rx    = LW_NULL;
    UINT8                    *src_flat = LW_NULL, *dst_flat = LW_NULL;
    dma_sg_entry_t           *sgl_tx   = LW_NULL, *sgl_rx   = LW_NULL;
    dma_async_tx_descriptor_t *desc_tx, *desc_rx;
    dma_pair_sync_t           sync;
    dma_ber_stat_t            ber_rx   = {0, 0};
    BOOL                      sync_init = LW_FALSE;
    UINT64                    t_wall_start, t_wall;
    size_t                    total    = (size_t)sg_len * entry_size;
    UINT                      r, i;
    size_t                    j;
    INT                       ret      = -1;

    printk("\n[dma_test] ===== loopback-fd  sg=%u  entry=%zuB  total=%zuB  rounds=%u =====\n",
           sg_len, entry_size, total, rounds);
    printk("[dma_test]   mode: FULL-DUPLEX HW loopback (TX || RX concurrent)\n");

    ch_tx    = dma_request_chan_by_name(TEST_DEV_NAME, 0);
    ch_rx    = dma_request_chan_by_name(TEST_DEV_NAME, 1);
    src_flat = (ch_tx && ch_rx) ? (UINT8 *)API_VmmDmaAlloc(total) : LW_NULL;
    dst_flat = src_flat ? (UINT8 *)API_VmmDmaAlloc(total) : LW_NULL;
    sgl_tx   = dst_flat ? (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(*sgl_tx)) : LW_NULL;
    sgl_rx   = sgl_tx   ? (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(*sgl_rx)) : LW_NULL;
    if (!sgl_rx) { printk("[dma_test]   FAIL: alloc\n"); goto  _out; }

    /*  发送侧数据模式（全程不变，每轮复用）  */
    for (j = 0; j < total; j++) { src_flat[j] = (UINT8)((j * 0x37 + 0xA5) & 0xFF); }

    /*  SG 列表：TX 读 src_flat 发出，RX 写 dst_flat  */
    for (i = 0; i < sg_len; i++) {
        sgl_tx[i].addr = (phys_addr_t)(addr_t)(src_flat + (size_t)i * entry_size);
        sgl_tx[i].len  = entry_size;
        sgl_rx[i].addr = (phys_addr_t)(addr_t)(dst_flat + (size_t)i * entry_size);
        sgl_rx[i].len  = entry_size;
    }

    /*  配置通道方向（真实 AXI DMA 无设备 FIFO 地址，传 NULL）  */
    if (__dma_slave_cfg(ch_tx, DMA_MEM_TO_DEV, LW_NULL) != 0 ||
        __dma_slave_cfg(ch_rx, DMA_DEV_TO_MEM, LW_NULL) != 0) {
        printk("[dma_test]   FAIL: slave_config\n"); goto  _out;
    }
    printk("[dma_test]   TX='%s'  src=%p\n", ch_tx->chan_name, src_flat);
    printk("[dma_test]   RX='%s'  dst=%p\n", ch_rx->chan_name, dst_flat);

    if (__dma_pair_sync_init(&sync) != 0) {
        printk("[dma_test]   FAIL: sync init\n"); goto  _out;
    }
    sync_init = LW_TRUE;

    for (r = 0; r < rounds; r++) {
        if (r == 0) { t_wall_start = API_TimeGet64(); }                 /*  首轮开始计时                */

        desc_tx = dmaengine_prep_slave_sg(ch_tx, sgl_tx, sg_len,
                                           DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
        desc_rx = dmaengine_prep_slave_sg(ch_rx, sgl_rx, sg_len,
                                           DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);
        if (!desc_tx || !desc_rx) {
            printk("[dma_test]   FAIL: prep round=%u\n", r);
            if (desc_tx && !desc_rx) {
                dmaengine_submit(desc_tx);
                dmaengine_terminate_all(ch_tx);
            }
            goto  _out_comp;
        }

        /*  并发执行：TX 发出数据到 Stream，硬件回环后 RX 接收  */
        if (__dma_exec_pair(ch_tx, desc_tx,
                             ch_rx, desc_rx, &sync, LW_NULL) != 0) {
            printk("[dma_test]   FAIL: exec round=%u\n", r);
            goto  _out_comp;
        }

        /*  RX BER：dst_flat 应等于 src_flat（硬件回环数据完整性校验）  */
        ber_rx.total_bytes += (UINT64)total;
        // ber_rx.error_bytes += __dma_count_errors(dst_flat, src_flat, total);
    }
    t_wall = API_TimeGet64() - t_wall_start;                            /*  挂钟总耗时（含所有开销）    */

    /*  吞吐统计  */
    printk("[dma_test]   TX  : "); __print_throughput((UINT64)total * rounds, t_wall);
    printk("[dma_test]   RX  : "); __print_throughput((UINT64)total * rounds, t_wall);
    printk("[dma_test]   FD  : "); __print_throughput((UINT64)total * rounds * 2, t_wall);
    __print_ber("RX BER", &ber_rx);
    ret = (ber_rx.error_bytes == 0) ? 0 : -1;

_out_comp:
_out:
    if (sync_init) { __dma_pair_sync_destroy(&sync); }
    if (sgl_rx)   { sys_free(sgl_rx); }
    if (sgl_tx)   { sys_free(sgl_tx); }
    if (dst_flat) { API_VmmDmaFree(dst_flat); }
    if (src_flat) { API_VmmDmaFree(src_flat); }
    if (ch_rx)    { dma_release_channel(ch_rx); }
    if (ch_tx)    { dma_release_channel(ch_tx); }
    printk("[dma_test] %s\n", ret ? "FAIL" : "PASS");
    return  ret;
}

typedef struct {
    CPCHAR                    name;
    dma_transaction_type_t    xfer_type;
    dma_transfer_direction_t  direction;
    UINT                      sg_len;
    size_t                    entry_size;
} dma_perf_scenario_t;

/*
 *  场景表：覆盖网卡（NIC）、块设备（Block）、内存拷贝（Memcpy）三类典型配置
 *
 *  NIC   : ETH 标准帧 1500B = SG=3×500B；Jumbo 9000B = SG=8×1125B
 *  Block : 512B 扇区、4KB 页、128KB 顺序IO、512KB 大块
 *  Memcpy: 64B 控制帧、512B payload、4KB 页、64KB 块、1MB 大数据
 */
static const dma_perf_scenario_t  _G_perf_scenarios[] = {
    // { "memcpy   64B  ctrl",  DMA_MEMCPY, DMA_MEM_TO_MEM, 1,       64 },
    // { "memcpy  512B  eth",   DMA_MEMCPY, DMA_MEM_TO_MEM, 1,      512 },
    { "memcpy   4KB  page",  DMA_MEMCPY, DMA_MEM_TO_MEM, 1,     4096 },
    // { "memcpy  64KB  block", DMA_MEMCPY, DMA_MEM_TO_MEM, 1,    65536 },
    // { "memcpy   1MB  large", DMA_MEMCPY, DMA_MEM_TO_MEM, 1,  1048576 },
    // { "nic-rx  ETH  SG=3",   DMA_SLAVE,  DMA_DEV_TO_MEM, 3,      500 },
    // { "nic-rx  Jmbo SG=8",   DMA_SLAVE,  DMA_DEV_TO_MEM, 8,     1125 },
    // { "nic-tx  ETH  SG=3",   DMA_SLAVE,  DMA_MEM_TO_DEV, 3,      500 },
    // { "nic-tx  Jmbo SG=8",   DMA_SLAVE,  DMA_MEM_TO_DEV, 8,     1125 },
    // { "blk-rd  512B  sect",  DMA_SLAVE,  DMA_DEV_TO_MEM, 1,      512 },
    { "blk-rd   4KB  page",  DMA_SLAVE,  DMA_DEV_TO_MEM, 1,     4096 },
    // { "blk-rd 128KB  seq",   DMA_SLAVE,  DMA_DEV_TO_MEM, 1,   131072 },
    // { "blk-rd 512KB  large", DMA_SLAVE,  DMA_DEV_TO_MEM, 1,   524288 },
    // { "blk-wr   4KB  page",  DMA_SLAVE,  DMA_MEM_TO_DEV, 1,     4096 },
    // { "blk-wr 512KB  large", DMA_SLAVE,  DMA_MEM_TO_DEV, 1,   524288 },
};

#define PERF_SCENARIO_NUM  (sizeof(_G_perf_scenarios) / sizeof(_G_perf_scenarios[0]))
#define PERF_DURATION_DEF  3                                            /*  默认每场景运行时长（秒）    */

typedef struct {
    UINT64  total_bytes;                                                /*  场景总传输字节数            */
    UINT64  total_ms;                                                   /*  场景实际耗时（毫秒）        */
    UINT    total_rounds;                                               /*  完成轮数                    */
    UINT    error_rounds;                                               /*  传输执行失败轮数            */
    BOOL    failed;                                                     /*  初始化失败（无法进入主循环）*/
} dma_perf_result_t;

/*********************************************************************************************************
** 函数名称: __dma_perf_print_kbs
** 功能描述: 格式化输出吞吐量（KB/s 或 MB/s）
** 输　入  : kbs — 吞吐量（KB/s）；0 表示无法计算
** 输　出  : NONE
*********************************************************************************************************/

static VOID  __dma_perf_print_kbs (UINT64 kbs)
{
    if (kbs == 0) {
        printk("N/A");
    } else if (kbs >= 1024) {
        printk("%llu.%llu MB/s",
               (unsigned long long)(kbs / 1024),
               (unsigned long long)((kbs % 1024) * 10 / 1024));
    } else {
        printk("%llu KB/s", (unsigned long long)kbs);
    }
}

/*********************************************************************************************************
** 函数名称: __dma_perf_run_one_timed
** 功能描述: 以时间为界运行单个性能场景，每秒输出中间报告，结束后汇总到 result
** 输　入  : scen         — 场景描述符
**           duration_sec — 运行时长（秒）
**           result       — 输出统计结果
** 输　出  : 0 成功；-1 失败
**
**  计时策略：
**    以 API_TimeGet64() 挂钟时间控制总时长，同样以挂钟时间划分每秒区间。
**    每轮传输用时 < 1 tick（软件模拟常见），个体 ticks=0，但累计区间时长 > 0，
**    保证每秒报告可以输出有意义的吞吐率。
**
*********************************************************************************************************/

static INT  __dma_perf_run_one_timed (const dma_perf_scenario_t *scen,
                                       UINT                       duration_sec,
                                       dma_perf_result_t         *result)
{
    static CPCHAR  dir_str[] = { "M->M", "M->D", "D->M", "D->D" };

    dma_chan_t               *chan       = LW_NULL;
    UINT8                    *mem_buf   = LW_NULL, *dev_buf = LW_NULL;
    dma_sg_entry_t           *sgl       = LW_NULL;
    dma_async_tx_descriptor_t *desc;
    dma_completion_t          comp;
    BOOL                      comp_init = LW_FALSE;
    size_t                    bytes_per_round;
    UINT64                    t_start, t_dur_ticks, t_sec_start, t_now;
    UINT64                    sec_bytes;
    UINT                      sec_rounds, sec_idx;
    UINT                      i;
    INT                       ret = -1;

    result->total_bytes  = 0;
    result->total_ms     = 0;
    result->total_rounds = 0;
    result->error_rounds = 0;
    result->failed       = LW_FALSE;

    bytes_per_round = (size_t)scen->sg_len * scen->entry_size;

    chan    = dma_request_chan_by_name(TEST_DEV_NAME, 0);
    mem_buf = chan ? (UINT8 *)sys_malloc(bytes_per_round) : LW_NULL;
    if (!mem_buf) { result->failed = LW_TRUE; goto  _done; }

    if (scen->xfer_type == DMA_SLAVE) {
        dev_buf = (UINT8 *)sys_malloc(bytes_per_round);
        sgl     = dev_buf ? (dma_sg_entry_t *)sys_malloc(scen->sg_len * sizeof(*sgl)) : LW_NULL;
        if (!sgl) { result->failed = LW_TRUE; goto  _done; }
        lib_memset(dev_buf, 0xA5, bytes_per_round);
        for (i = 0; i < scen->sg_len; i++) {
            sgl[i].addr = (phys_addr_t)(addr_t)(mem_buf + (size_t)i * scen->entry_size);
            sgl[i].len  = scen->entry_size;
        }
        if (__dma_slave_cfg(chan, scen->direction, dev_buf) != 0) {
            result->failed = LW_TRUE; goto  _done;
        }
    }
    lib_memset(mem_buf, 0x5A, bytes_per_round);

    if (__dma_completion_init(&comp) != 0) { result->failed = LW_TRUE; goto  _done; }
    comp_init = LW_TRUE;

    printk("[dma_test]  %-24s  %-4s  SG=%-2u  %7zuB  %us:\n",
           scen->name, dir_str[scen->direction], scen->sg_len,
           scen->entry_size, duration_sec);

    t_start      = API_TimeGet64();
    t_dur_ticks  = (UINT64)duration_sec * LW_CFG_TICKS_PER_SEC;
    t_sec_start  = t_start;
    sec_bytes    = 0;
    sec_rounds   = 0;
    sec_idx      = 0;

    while (LW_TRUE) {
        t_now = API_TimeGet64();
        if (t_now - t_start >= t_dur_ticks) {
            break;
        }

        if (scen->xfer_type == DMA_MEMCPY) {
            desc = dmaengine_prep_dma_memcpy(chan,
                                              (phys_addr_t)(addr_t)mem_buf,
                                              (phys_addr_t)(addr_t)mem_buf,
                                              scen->entry_size, DMA_PREP_INTERRUPT);
        } else {
            desc = dmaengine_prep_slave_sg(chan, sgl, scen->sg_len,
                                            scen->direction, DMA_PREP_INTERRUPT);
        }

        if (!desc || __dma_exec_once(chan, desc, &comp, LW_NULL) != 0) {
            result->error_rounds++;
            continue;
        }

        result->total_bytes  += bytes_per_round;
        result->total_rounds++;
        sec_bytes  += bytes_per_round;
        sec_rounds++;

        /*  每秒输出中间报告  */
        t_now = API_TimeGet64();
        if (t_now - t_sec_start >= (UINT64)LW_CFG_TICKS_PER_SEC) {
            UINT64  elapsed = t_now - t_sec_start;
            UINT64  kbs     = THROUGHPUT_KBS(sec_bytes, elapsed);

            sec_idx++;
            printk("[dma_test]    [%3us]  rounds=%-6u  ", sec_idx, sec_rounds);
            __dma_perf_print_kbs(kbs);
            printk("\n");

            sec_bytes   = 0;
            sec_rounds  = 0;
            t_sec_start = t_now;
        }
    }

    /*  输出最后一个不满 1 秒的区间（若有剩余轮次）  */
    t_now = API_TimeGet64();
    if (sec_rounds > 0 && t_now > t_sec_start) {
        UINT64  elapsed = t_now - t_sec_start;
        UINT64  kbs     = THROUGHPUT_KBS(sec_bytes, elapsed);

        sec_idx++;
        printk("[dma_test]    [%3us]  rounds=%-6u  ", sec_idx, sec_rounds);
        __dma_perf_print_kbs(kbs);
        printk("  (partial)\n");
    }

    result->total_ms = TICKS_TO_MS(t_now - t_start);

    if (!result->failed) {
        UINT64  avg_kbs = THROUGHPUT_KBS(result->total_bytes,
                                          (UINT64)result->total_ms *
                                          LW_CFG_TICKS_PER_SEC / 1000ULL);
        printk("[dma_test]  >> Summary: rounds=%-6u  errors=%-4u  total=%lluB  time=%llums  avg=",
               result->total_rounds,
               result->error_rounds,
               (unsigned long long)result->total_bytes,
               (unsigned long long)result->total_ms);
        __dma_perf_print_kbs(avg_kbs);
        printk("\n\n");
        ret = 0;
    } else {
        printk("[dma_test]  >> FAILED\n\n");
    }

_done:
    if (comp_init) { __dma_completion_destroy(&comp); }
    if (sgl)       { sys_free(sgl); }
    if (dev_buf)   { sys_free(dev_buf); }
    if (mem_buf)   { sys_free(mem_buf); }
    if (chan)      { dma_release_channel(chan); }
    return  ret;
}

static VOID  __dma_test_perf_run_all (UINT duration_sec)
{
    static CPCHAR  dir_str[] = { "M->M", "M->D", "D->M", "D->D" };
    dma_perf_result_t  results[PERF_SCENARIO_NUM];
    UINT               i;

    printk("\n[dma_test] ====================================================\n");
    printk("[dma_test]  DMA Perf Benchmark  (%s)  TickRate=%d Hz\n",
           TEST_DEV_NAME, LW_CFG_TICKS_PER_SEC);
    printk("[dma_test]  Duration per scenario: %u second(s)\n", duration_sec);
    printk("[dma_test] ====================================================\n\n");

    for (i = 0; i < PERF_SCENARIO_NUM; i++) {
        __dma_perf_run_one_timed(&_G_perf_scenarios[i], duration_sec, &results[i]);
    }

    /*  汇总表  */
    printk("[dma_test] ====================================================\n");
    printk("[dma_test]  Summary Table  (duration=%us/scenario)\n", duration_sec);
    printk("[dma_test]  %-24s %-4s  SG %9s %8s  Throughput\n",
           "Scenario", "Dir", "Entry-B", "Time-ms");
    printk("[dma_test] ----------------------------------------------------\n");

    for (i = 0; i < PERF_SCENARIO_NUM; i++) {
        const dma_perf_scenario_t  *s = &_G_perf_scenarios[i];
        const dma_perf_result_t    *r = &results[i];

        if (r->failed) {
            printk("[dma_test]  %-24s %-4s  %-2u %9zu  FAILED\n",
                   s->name, dir_str[s->direction], s->sg_len, s->entry_size);
        } else {
            UINT64  avg_kbs = THROUGHPUT_KBS(r->total_bytes,
                                              (UINT64)r->total_ms *
                                              LW_CFG_TICKS_PER_SEC / 1000ULL);
            printk("[dma_test]  %-24s %-4s  %-2u %9zu %8llu  ",
                   s->name, dir_str[s->direction], s->sg_len, s->entry_size,
                   (unsigned long long)r->total_ms);
            __dma_perf_print_kbs(avg_kbs);
            if (r->error_rounds > 0) {
                printk("  [err=%u]", r->error_rounds);
            }
            printk("\n");
        }
    }
    printk("[dma_test] ====================================================\n\n");
}

/*================================================ tshell 命令 ===========================================*/

static INT  __cmd_dma_test_memcpy (INT iArgC, PCHAR ppcArgV[])
{
    size_t  size = 4096;
    if (iArgC >= 2) { size = (size_t)lib_strtoul(ppcArgV[1], LW_NULL, 10); }
    if (!size || size > (64 << 20)) {
        printk("usage: dma_test_memcpy [size(1~67108864)]\n"); return  (-1);
    }
    return  __dma_test_memcpy_run(size);
}

static INT  __cmd_dma_test_multi (INT iArgC, PCHAR ppcArgV[])
{
    UINT   count = 8;  size_t  entry = 512;
    if (iArgC >= 2) { count = (UINT)lib_strtoul(ppcArgV[1], LW_NULL, 10); }
    if (iArgC >= 3) { entry = (size_t)lib_strtoul(ppcArgV[2], LW_NULL, 10); }
    if (!count || count > 4096 || !entry || entry > (4 << 20)) {
        printk("usage: dma_test_multi [count(1~4096)] [entry(1~4194304)]\n"); return  (-1);
    }
    return  __dma_test_multi_run(count, entry);
}

static INT  __cmd_dma_test_sg (INT iArgC, PCHAR ppcArgV[])
{
    UINT                      sg  = 4;
    size_t                    ent = 512;
    dma_transfer_direction_t  dir = DMA_DEV_TO_MEM;

    if (iArgC >= 2) { sg  = (UINT)lib_strtoul(ppcArgV[1], LW_NULL, 10); }
    if (iArgC >= 3) { ent = (size_t)lib_strtoul(ppcArgV[2], LW_NULL, 10); }
    if (iArgC >= 4) {
        if      (lib_strcmp(ppcArgV[3], "tx") == 0) { dir = DMA_MEM_TO_DEV; }
        else if (lib_strcmp(ppcArgV[3], "rx") == 0) { dir = DMA_DEV_TO_MEM; }
        else { printk("usage: dma_test_sg [sg] [entry] [rx|tx]\n"); return  (-1); }
    }
    if (!sg || sg > TEST_SG_MAX || !ent || ent > (4 << 20)) {
        printk("[dma_test] sg: 1~%d  entry: 1~4194304\n", TEST_SG_MAX); return  (-1);
    }
    return  __dma_test_sg_run(sg, ent, dir);
}

/*
 *  dma_test_loopback_fd [sg_len] [entry_size] [rounds]
 *
 *  全双工回环：TX(ch0) 与 RX(ch1) 同时 issue，rx_fifo 预填充模拟硬件回环。
 *  典型用法（对应常见设备场景）：
 *    dma_test_loopback_fd                    — 4×512B 默认全双工回环
 *    dma_test_loopback_fd 3  500   100       — ETH 1500B 帧全双工 100 次
 *    dma_test_loopback_fd 8  1125   50       — Jumbo 9KB 帧全双工
 *    dma_test_loopback_fd 1  4096  1000      — 4KB 块设备全双工读写 1000 次
 *    dma_test_loopback_fd 1  65536  200      — 64KB 顺序全双工 IO
 */
static INT  __cmd_dma_test_loopback_fd (INT iArgC, PCHAR ppcArgV[])
{
    UINT    sg  = 4, rounds = 1;
    size_t  ent = 512;

    if (iArgC >= 2) { sg     = (UINT)lib_strtoul(ppcArgV[1], LW_NULL, 10); }
    if (iArgC >= 3) { ent    = (size_t)lib_strtoul(ppcArgV[2], LW_NULL, 10); }
    if (iArgC >= 4) { rounds = (UINT)lib_strtoul(ppcArgV[3], LW_NULL, 10); }
    if (!sg || sg > TEST_SG_MAX || !ent || ent > (4 << 20) || !rounds || rounds > 100000) {
        printk("usage: dma_test_loopback_fd [sg(1~64)] [entry(1~4194304)] [rounds(1~100000)]\n");
        return  (-1);
    }
    return  __dma_test_loopback_fd_run(sg, ent, rounds);
}

static INT  __cmd_dma_test_perf (INT iArgC, PCHAR ppcArgV[])
{
    UINT  duration = PERF_DURATION_DEF;

    if (iArgC >= 2) { duration = (UINT)lib_strtoul(ppcArgV[1], LW_NULL, 10); }
    if (!duration || duration > 300) {
        printk("usage: dma_test_perf [duration_sec(1~300)]  default=%d\n", PERF_DURATION_DEF);
        return  (-1);
    }
    __dma_test_perf_run_all(duration);
    return  (0);
}

/*================================================ 命令注册 ===============================================*/

VOID  dma_test_register_cmds (VOID)
{
    API_TShellKeywordAdd("dma_test_memcpy",   __cmd_dma_test_memcpy);
    API_TShellFormatAdd ("dma_test_memcpy",   " [size]");
    API_TShellHelpAdd   ("dma_test_memcpy",   "MEM_TO_MEM memcpy test  [size=4096]\n");

    API_TShellKeywordAdd("dma_test_multi",    __cmd_dma_test_multi);
    API_TShellFormatAdd ("dma_test_multi",    " [count] [entry]");
    API_TShellHelpAdd   ("dma_test_multi",    "multi-descriptor batch test  [count=8 entry=512]\n");

    API_TShellKeywordAdd("dma_test_sg",       __cmd_dma_test_sg);
    API_TShellFormatAdd ("dma_test_sg",       " [sg] [entry] [rx|tx]");
    API_TShellHelpAdd   ("dma_test_sg",       "Slave SG test  [sg=4 entry=512 dir=rx]\n"
                         "  e.g. 'dma_test_sg 3 500 rx'  ETH 1500B frame RX\n"
                         "       'dma_test_sg 1 4096 tx' block 4KB write TX\n");

    API_TShellKeywordAdd("dma_test_loopback_fd", __cmd_dma_test_loopback_fd);
    API_TShellFormatAdd ("dma_test_loopback_fd", " [sg] [entry] [rounds]");
    API_TShellHelpAdd   ("dma_test_loopback_fd", "full-duplex TX||RX concurrent loopback  [sg=4 entry=512 rounds=1]\n"
                         "  e.g. 'dma_test_loopback_fd 3 500 100'   ETH frame full-duplex\n"
                         "       'dma_test_loopback_fd 1 4096 1000' block I/O full-duplex\n");

    API_TShellKeywordAdd("dma_test_perf",     __cmd_dma_test_perf);
    API_TShellFormatAdd ("dma_test_perf",     " [duration_sec]");
    API_TShellHelpAdd   ("dma_test_perf",
                         "timed perf benchmark (15 configs, per-second report + summary)\n"
                         "  [duration_sec]: seconds per scenario (default=3, max=300)\n"
                         "  e.g. 'dma_test_perf'    — 3s/scenario\n"
                         "       'dma_test_perf 10' — 10s/scenario\n");

    printk("[dma_test] 5 commands registered\n");
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

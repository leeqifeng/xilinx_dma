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
**    4. 双路并发等待：__dma_exec_pair 同时 issue 两个通道，主线程顺序 pend 两个信号量；
**       两通道 worker 线程天然并发执行，无需额外测试线程。
**    5. 计时：API_TimeGet64() 返回 64-bit tick 计数，差值 ticks=0 表示同 tick 内完成，
**       此时输出 N/A；ticks>0 时吞吐量 = bytes × tickrate / (ticks × 1024) KB/s。
**    6. 描述符生命期：prep 成功后须完成 submit；若 submit 前 prep 的另一路失败，
**       需将已 prep 的描述符 submit 到 pending 队列后立即 terminate_all，
**       由驱动在 terminate_all 中统一释放，避免泄漏。
**
**  tshell 命令（共 6 条）：
**    dma_test_memcpy      [size]                    单次 MEM_TO_MEM memcpy（默认 4096B，ch0）
**    dma_test_multi       [count] [entry]           批量多描述符 + dma_sync_wait（默认 8×512B，ch1）
**    dma_test_sg          [sg] [entry] [rx|tx]      单向 Slave SG 传输（默认 4×512B rx，ch0）
**    dma_test_loopback    [sg] [entry] [rounds]     半双工回环：TX 完成→RX 串行（默认 4×512B×1）
**    dma_test_loopback_fd [sg] [entry] [rounds]     全双工回环：TX ∥ RX 并发（默认 4×512B×1）
**    dma_test_perf                                  全场景自动性能基准（15 种配置）
**
*********************************************************************************************************/
#define  __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include <string.h>
#include "../dmaengine.h"

#define TEST_DEV_NAME                   "fream-dma"
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

static VOID  __dma_complete_cb (PVOID param, const dmaengine_result_t *result)
{
    dma_completion_t  *comp = (dma_completion_t *)param;
    comp->status  = result->result;
    comp->residue = result->residue;
    API_SemaphoreBPost(comp->sem);
}

static INT  __dma_completion_init (dma_completion_t *comp)
{
    lib_memset(comp, 0, sizeof(*comp));
    comp->sem = API_SemaphoreBCreate("dma_wait", LW_FALSE, LW_OPTION_OBJECT_LOCAL, LW_NULL);
    if (comp->sem == LW_OBJECT_HANDLE_INVALID) { return  (-1); }
    comp->status = DMA_IN_PROGRESS;
    return  (0);
}

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

/*********************************************************************************************************
** 函数名称: __dma_exec_pair
** 功能描述: 并发执行两路传输（全双工）：同时 submit+issue 两个描述符，分别等待各自完成
** 输　入  : chan_a/b   — 两个独立通道（不得相同）
**           desc_a/b   — 各自已准备好的描述符（callback 由本函数设置）
**           comp_a/b   — 各自独立的完成等待结构（本函数内清信号量后重用）
**           p_ticks    — 输出总耗时（从两路同时 issue 到均完成，反映全双工实际延迟）
** 输　出  : 0 成功；-1 失败
** 说明    : 两路通道各自的 worker 线程真正并发执行，主线程顺序 pend 两个信号量即可，
**           无需额外线程。计时 t1-t0 ≈ max(t_a, t_b)，即全双工下的有效延迟。
**           若任一方超时，对两通道均调用 dmaengine_terminate_all 确保状态干净。
*********************************************************************************************************/

static INT  __dma_exec_pair (dma_chan_t                *chan_a,
                              dma_async_tx_descriptor_t *desc_a,
                              dma_completion_t          *comp_a,
                              dma_chan_t                *chan_b,
                              dma_async_tx_descriptor_t *desc_b,
                              dma_completion_t          *comp_b,
                              UINT64                    *p_ticks)
{
    UINT64  t0, t1;
    INT     ret = -1;

    API_SemaphoreBClear(comp_a->sem);
    API_SemaphoreBClear(comp_b->sem);
    comp_a->status = DMA_IN_PROGRESS;
    comp_b->status = DMA_IN_PROGRESS;

    desc_a->callback_result = __dma_complete_cb;
    desc_a->callback_param  = comp_a;
    desc_b->callback_result = __dma_complete_cb;
    desc_b->callback_param  = comp_b;

    if (dmaengine_submit(desc_a) == DMA_COOKIE_INVALID ||
        dmaengine_submit(desc_b) == DMA_COOKIE_INVALID) {
        printk("[dma_test]   FAIL: submit\n");
        return  (-1);
    }

    t0 = API_TimeGet64();
    dma_async_issue_pending(chan_a);
    dma_async_issue_pending(chan_b);                                     /*  两路同时触发，真正并发      */

    if (API_SemaphoreBPend(comp_a->sem, LW_MSECOND_TO_TICK_1(5000)) != ERROR_NONE) {
        printk("[dma_test]   FAIL: timeout chan_a='%s'\n", chan_a->chan_name);
        dmaengine_terminate_all(chan_a);
        dmaengine_terminate_all(chan_b);
        if (p_ticks) { *p_ticks = 0; }
        return  (-1);
    }
    if (API_SemaphoreBPend(comp_b->sem, LW_MSECOND_TO_TICK_1(5000)) != ERROR_NONE) {
        printk("[dma_test]   FAIL: timeout chan_b='%s'\n", chan_b->chan_name);
        dmaengine_terminate_all(chan_b);
        if (p_ticks) { *p_ticks = 0; }
        return  (-1);
    }
    t1 = API_TimeGet64();
    if (p_ticks) { *p_ticks = t1 - t0; }

    ret = (comp_a->status == DMA_COMPLETE && comp_b->status == DMA_COMPLETE) ? 0 : -1;
    if (ret) {
        printk("[dma_test]   FAIL: status a=%d b=%d\n",
               (INT)comp_a->status, (INT)comp_b->status);
    }
    return  ret;
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

    ret = (lib_memcmp(dst, src, size) == 0) ? 0 : -1;
    if (ret) { printk("[dma_test]   FAIL: data mismatch\n"); }
    else     { printk("[dma_test]   verify OK\n"); }

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

    ret = (lib_memcmp(dst, src, total) == 0) ? 0 : -1;
    if (ret) { printk("[dma_test]   FAIL: data mismatch\n"); }
    else     { printk("[dma_test]   verify OK\n"); }

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

    /* 校验：dev_buf 与 flat 的对应切片应一致 */
    ret = 0;
    for (i = 0; i < sg_len; i++) {
        if (lib_memcmp(dev_buf + (size_t)i * entry_size,
                       flat   + (size_t)i * entry_size, entry_size) != 0) {
            printk("[dma_test]   FAIL: mismatch at SG[%u]\n", i);
            ret = -1;
            break;
        }
    }
    if (ret == 0) { printk("[dma_test]   verify: %u SG entries OK\n", sg_len); }

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

/*================================================ 测试用例: 回环 ========================================*/

/*********************************************************************************************************
** 函数名称: __dma_test_loopback_run
** 功能描述: 双通道 Slave SG 回环测试
** 输　入  : sg_len     — SG 条目数
**           entry_size — 每条目字节数
**           rounds     — 回环轮数（每轮独立传输、校验）
** 输　出  : 0 PASS；-1 FAIL
**
**  回环路径：
**    [src_flat]  ──TX(ch0, MEM_TO_DEV)──►  [dev_buf]  ──RX(ch1, DEV_TO_MEM)──►  [dst_flat]
**    verify: dst_flat == src_flat
**
**  模拟的典型场景：
**    网卡帧回环：NIC TX 把帧写入设备 FIFO（dev_buf），NIC RX 再从 FIFO 读出，
**               验证收到的帧与发送帧完全一致，检测 DMA 路径的完整性。
**    块设备回环：向块设备写一扇区，再读回，比对数据一致性。
**
**  统计输出：TX 吞吐量、RX 吞吐量、往返（round-trip）吞吐量
**
*********************************************************************************************************/

static INT  __dma_test_loopback_run (UINT sg_len, size_t entry_size, UINT rounds)
{
    dma_chan_t               *ch_tx   = LW_NULL, *ch_rx = LW_NULL;
    UINT8                    *dev_buf = LW_NULL;
    UINT8                    *src_flat = LW_NULL, *dst_flat = LW_NULL;
    dma_sg_entry_t           *sgl_tx  = LW_NULL, *sgl_rx = LW_NULL;
    dma_async_tx_descriptor_t *desc;
    dma_completion_t          comp;
    UINT64                    ticks, t_tx = 0, t_rx = 0;
    size_t                    total   = (size_t)sg_len * entry_size;
    UINT                      r, i;
    size_t                    j;
    INT                       ret     = -1;

    printk("\n[dma_test] ===== loopback  sg=%u  entry=%zuB  total=%zuB  rounds=%u =====\n",
           sg_len, entry_size, total, rounds);
    printk("[dma_test]   path: src_flat --TX(ch0)--> dev_buf --RX(ch1)--> dst_flat\n");

    ch_tx    = dma_request_chan_by_name(TEST_DEV_NAME, 0);
    ch_rx    = dma_request_chan_by_name(TEST_DEV_NAME, 1);
    dev_buf  = (ch_tx && ch_rx) ? (UINT8 *)sys_malloc(total) : LW_NULL;
    src_flat = dev_buf ? (UINT8 *)sys_malloc(total) : LW_NULL;
    dst_flat = src_flat ? (UINT8 *)sys_malloc(total) : LW_NULL;
    sgl_tx   = dst_flat ? (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(*sgl_tx)) : LW_NULL;
    sgl_rx   = sgl_tx   ? (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(*sgl_rx)) : LW_NULL;
    if (!sgl_rx) { printk("[dma_test]   FAIL: alloc\n"); goto  _out; }

    /* src_flat 填充可辨识模式 */
    for (j = 0; j < total; j++) { src_flat[j] = (UINT8)((j * 0x37 + 0xA5) & 0xFF); }

    /* 配置 SG 列表（TX 指向 src_flat，RX 指向 dst_flat，各条目等大平铺） */
    for (i = 0; i < sg_len; i++) {
        sgl_tx[i].addr = (phys_addr_t)(addr_t)(src_flat + (size_t)i * entry_size);
        sgl_tx[i].len  = entry_size;
        sgl_rx[i].addr = (phys_addr_t)(addr_t)(dst_flat + (size_t)i * entry_size);
        sgl_rx[i].len  = entry_size;
    }

    /* 配置两通道 slave 参数（共用 dev_buf） */
    if (__dma_slave_cfg(ch_tx, DMA_MEM_TO_DEV, dev_buf) != 0 ||
        __dma_slave_cfg(ch_rx, DMA_DEV_TO_MEM, dev_buf) != 0) {
        printk("[dma_test]   FAIL: slave_config\n"); goto  _out;
    }
    printk("[dma_test]   TX='%s'  RX='%s'  dev_buf=%p\n",
           ch_tx->chan_name, ch_rx->chan_name, dev_buf);

    if (__dma_completion_init(&comp) != 0) { goto  _out; }

    for (r = 0; r < rounds; r++) {
        /* ---- TX: src_flat → dev_buf ---- */
        lib_memset(dev_buf, 0, total);
        desc = dmaengine_prep_slave_sg(ch_tx, sgl_tx, sg_len, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
        if (!desc || __dma_exec_once(ch_tx, desc, &comp, &ticks) != 0) {
            printk("[dma_test]   FAIL: TX round=%u\n", r);
            goto  _out_comp;
        }
        t_tx += ticks;

        /* ---- RX: dev_buf → dst_flat ---- */
        lib_memset(dst_flat, 0xAA, total);
        desc = dmaengine_prep_slave_sg(ch_rx, sgl_rx, sg_len, DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);
        if (!desc || __dma_exec_once(ch_rx, desc, &comp, &ticks) != 0) {
            printk("[dma_test]   FAIL: RX round=%u\n", r);
            goto  _out_comp;
        }
        t_rx += ticks;

        /* ---- 逐条目校验 ---- */
        for (i = 0; i < sg_len; i++) {
            if (lib_memcmp(dst_flat + (size_t)i * entry_size,
                           src_flat + (size_t)i * entry_size, entry_size) != 0) {
                printk("[dma_test]   FAIL: mismatch SG[%u] round=%u\n", i, r);
                goto  _out_comp;
            }
        }
    }

    printk("[dma_test]   TX  : "); __print_throughput((UINT64)total * rounds, t_tx);
    printk("[dma_test]   RX  : "); __print_throughput((UINT64)total * rounds, t_rx);
    printk("[dma_test]   RTT : "); __print_throughput((UINT64)total * rounds, t_tx + t_rx);
    printk("[dma_test]   verify: %u rounds × %u SG entries OK\n", rounds, sg_len);
    ret = 0;

_out_comp:
    __dma_completion_destroy(&comp);
_out:
    if (sgl_rx)   { sys_free(sgl_rx); }
    if (sgl_tx)   { sys_free(sgl_tx); }
    if (dst_flat) { sys_free(dst_flat); }
    if (src_flat) { sys_free(src_flat); }
    if (dev_buf)  { sys_free(dev_buf); }
    if (ch_rx)    { dma_release_channel(ch_rx); }
    if (ch_tx)    { dma_release_channel(ch_tx); }
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
**    [src_flat] ──TX(ch0, MEM_TO_DEV)──► [tx_fifo]          ← 软件模拟 TX 输出 FIFO
**    [rx_fifo]  ──RX(ch1, DEV_TO_MEM)──► [dst_flat]         ← 软件模拟 RX 输入 FIFO
**
**    物理回环含义：真实 NIC/串口中，发送器输出经回环线连到接收器输入。
**    软件中以 lib_memcpy(rx_fifo, src_flat) 在每轮 issue 前预填充，
**    模拟"上一帧已由硬件从 TX FIFO 搬到 RX FIFO"这一物理行为；
**    随后 TX 与 RX 两路 DMA 同时启动，互不阻塞。
**
**  与半双工 dma_test_loopback 的区别：
**    半双工：TX 等待完成 → RX 再开始（串行，tx_fifo == rx_fifo == dev_buf）
**    全双工：TX / RX 同时 issue（并发，tx_fifo ≠ rx_fifo，无竞争）
**
**  校验：
**    TX 侧：tx_fifo == src_flat（DMA 正确将发送数据搬出到设备）
**    RX 侧：dst_flat == src_flat（DMA 正确将接收数据搬入内存）
**
**  吞吐统计（t_total ≈ Σ max(tx_lat_r, rx_lat_r)）：
**    TX  ≈ total×rounds / t_total
**    RX  ≈ total×rounds / t_total
**    FD  = 2×total×rounds / t_total  （双向合计，体现全双工收益）
**
*********************************************************************************************************/

static INT  __dma_test_loopback_fd_run (UINT sg_len, size_t entry_size, UINT rounds)
{
    dma_chan_t               *ch_tx    = LW_NULL, *ch_rx    = LW_NULL;
    UINT8                    *tx_fifo  = LW_NULL, *rx_fifo  = LW_NULL;
    UINT8                    *src_flat = LW_NULL, *dst_flat = LW_NULL;
    dma_sg_entry_t           *sgl_tx   = LW_NULL, *sgl_rx   = LW_NULL;
    dma_async_tx_descriptor_t *desc_tx, *desc_rx;
    dma_completion_t          comp_tx, comp_rx;
    BOOL                      tx_init  = LW_FALSE, rx_init = LW_FALSE;
    UINT64                    ticks, t_total = 0;
    size_t                    total    = (size_t)sg_len * entry_size;
    UINT                      r, i;
    size_t                    j;
    INT                       ret      = -1;

    printk("\n[dma_test] ===== loopback-fd  sg=%u  entry=%zuB  total=%zuB  rounds=%u =====\n",
           sg_len, entry_size, total, rounds);
    printk("[dma_test]   mode: FULL-DUPLEX (TX || RX concurrent, separate FIFOs)\n");

    ch_tx    = dma_request_chan_by_name(TEST_DEV_NAME, 0);
    ch_rx    = dma_request_chan_by_name(TEST_DEV_NAME, 1);
    tx_fifo  = (ch_tx && ch_rx) ? (UINT8 *)sys_malloc(total) : LW_NULL;
    rx_fifo  = tx_fifo  ? (UINT8 *)sys_malloc(total) : LW_NULL;
    src_flat = rx_fifo  ? (UINT8 *)sys_malloc(total) : LW_NULL;
    dst_flat = src_flat ? (UINT8 *)sys_malloc(total) : LW_NULL;
    sgl_tx   = dst_flat ? (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(*sgl_tx)) : LW_NULL;
    sgl_rx   = sgl_tx   ? (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(*sgl_rx)) : LW_NULL;
    if (!sgl_rx) { printk("[dma_test]   FAIL: alloc\n"); goto  _out; }

    /* 发送侧数据模式（全程不变，每轮复用） */
    for (j = 0; j < total; j++) { src_flat[j] = (UINT8)((j * 0x37 + 0xA5) & 0xFF); }

    /* SG 列表：TX 读 src_flat，RX 写 dst_flat */
    for (i = 0; i < sg_len; i++) {
        sgl_tx[i].addr = (phys_addr_t)(addr_t)(src_flat + (size_t)i * entry_size);
        sgl_tx[i].len  = entry_size;
        sgl_rx[i].addr = (phys_addr_t)(addr_t)(dst_flat + (size_t)i * entry_size);
        sgl_rx[i].len  = entry_size;
    }

    /* 配置两通道：各自独立的 FIFO 地址，不共享，无竞争 */
    if (__dma_slave_cfg(ch_tx, DMA_MEM_TO_DEV, tx_fifo) != 0 ||
        __dma_slave_cfg(ch_rx, DMA_DEV_TO_MEM, rx_fifo) != 0) {
        printk("[dma_test]   FAIL: slave_config\n"); goto  _out;
    }
    printk("[dma_test]   TX='%s'  tx_fifo=%p\n", ch_tx->chan_name, tx_fifo);
    printk("[dma_test]   RX='%s'  rx_fifo=%p  (pre-loaded each round)\n",
           ch_rx->chan_name, rx_fifo);

    if (__dma_completion_init(&comp_tx) != 0) { goto  _out; }
    tx_init = LW_TRUE;
    if (__dma_completion_init(&comp_rx) != 0) { goto  _out; }
    rx_init = LW_TRUE;

    for (r = 0; r < rounds; r++) {
        /*
         *  每轮重置：
         *    tx_fifo 清零           — 确保 TX 写入结果可被校验
         *    rx_fifo = src_flat     — 模拟硬件回环：发送数据已到达 RX FIFO
         *    dst_flat = 0xAA 填充   — 确保 RX 写入结果可被校验（非初始值）
         */
        lib_memset(tx_fifo,  0,    total);
        lib_memcpy(rx_fifo,  src_flat, total);
        lib_memset(dst_flat, 0xAA, total);

        desc_tx = dmaengine_prep_slave_sg(ch_tx, sgl_tx, sg_len,
                                           DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
        desc_rx = dmaengine_prep_slave_sg(ch_rx, sgl_rx, sg_len,
                                           DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);
        if (!desc_tx || !desc_rx) {
            printk("[dma_test]   FAIL: prep round=%u\n", r);
            /*
             *  若 desc_tx 已分配而 desc_rx 失败：框架无独立 free_desc 接口，
             *  将 desc_tx 提交至 pending 队列后立即 terminate_all，驱动在
             *  terminate_all 中遍历 pending 链表并 sys_free 描述符，避免泄漏。
             */
            if (desc_tx && !desc_rx) {
                dmaengine_submit(desc_tx);
                dmaengine_terminate_all(ch_tx);
            }
            goto  _out_comp;
        }

        /* 并发执行：两路同时 issue，t ≈ max(TX延迟, RX延迟) */
        if (__dma_exec_pair(ch_tx, desc_tx, &comp_tx,
                             ch_rx, desc_rx, &comp_rx, &ticks) != 0) {
            printk("[dma_test]   FAIL: exec round=%u\n", r);
            goto  _out_comp;
        }
        t_total += ticks;

        /* 校验 TX 侧：tx_fifo 应等于 src_flat（DMA 正确搬出） */
        for (i = 0; i < sg_len; i++) {
            if (lib_memcmp(tx_fifo  + (size_t)i * entry_size,
                           src_flat + (size_t)i * entry_size, entry_size) != 0) {
                printk("[dma_test]   FAIL: TX mismatch SG[%u] round=%u\n", i, r);
                goto  _out_comp;
            }
        }
        /* 校验 RX 侧：dst_flat 应等于 src_flat（DMA 正确搬入） */
        for (i = 0; i < sg_len; i++) {
            if (lib_memcmp(dst_flat + (size_t)i * entry_size,
                           src_flat + (size_t)i * entry_size, entry_size) != 0) {
                printk("[dma_test]   FAIL: RX mismatch SG[%u] round=%u\n", i, r);
                goto  _out_comp;
            }
        }
    }

    /* 吞吐统计（t_total ≈ Σ max(tx_r, rx_r)） */
    printk("[dma_test]   TX  : "); __print_throughput((UINT64)total * rounds, t_total);
    printk("[dma_test]   RX  : "); __print_throughput((UINT64)total * rounds, t_total);
    printk("[dma_test]   FD  : "); __print_throughput((UINT64)total * rounds * 2, t_total);
    printk("[dma_test]   verify: %u rounds × %u SG entries (TX+RX both sides) OK\n",
           rounds, sg_len);
    ret = 0;

_out_comp:
    if (rx_init) { __dma_completion_destroy(&comp_rx); }
    if (tx_init) { __dma_completion_destroy(&comp_tx); }
_out:
    if (sgl_rx)   { sys_free(sgl_rx); }
    if (sgl_tx)   { sys_free(sgl_tx); }
    if (dst_flat) { sys_free(dst_flat); }
    if (src_flat) { sys_free(src_flat); }
    if (rx_fifo)  { sys_free(rx_fifo); }
    if (tx_fifo)  { sys_free(tx_fifo); }
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
    UINT                      rounds;
} dma_perf_scenario_t;

/*
 *  场景表：覆盖网卡（NIC）、块设备（Block）、内存拷贝（Memcpy）三类典型配置
 *
 *  NIC   : ETH 标准帧 1500B = SG=3×500B；Jumbo 9000B = SG=8×1125B
 *  Block : 512B 扇区、4KB 页、128KB 顺序IO、512KB 大块
 *  Memcpy: 64B 控制帧、512B payload、4KB 页、64KB 块、1MB 大数据
 */
static const dma_perf_scenario_t  _G_perf_scenarios[] = {
    { "memcpy   64B  ctrl",  DMA_MEMCPY, DMA_MEM_TO_MEM, 1,       64, 10000 },
    { "memcpy  512B  eth",   DMA_MEMCPY, DMA_MEM_TO_MEM, 1,      512,  5000 },
    { "memcpy   4KB  page",  DMA_MEMCPY, DMA_MEM_TO_MEM, 1,     4096,  2000 },
    { "memcpy  64KB  block", DMA_MEMCPY, DMA_MEM_TO_MEM, 1,    65536,   500 },
    { "memcpy   1MB  large", DMA_MEMCPY, DMA_MEM_TO_MEM, 1,  1048576,    16 },
    { "nic-rx  ETH  SG=3",   DMA_SLAVE,  DMA_DEV_TO_MEM, 3,      500,  2000 },
    { "nic-rx  Jmbo SG=8",   DMA_SLAVE,  DMA_DEV_TO_MEM, 8,     1125,   500 },
    { "nic-tx  ETH  SG=3",   DMA_SLAVE,  DMA_MEM_TO_DEV, 3,      500,  2000 },
    { "nic-tx  Jmbo SG=8",   DMA_SLAVE,  DMA_MEM_TO_DEV, 8,     1125,   500 },
    { "blk-rd  512B  sect",  DMA_SLAVE,  DMA_DEV_TO_MEM, 1,      512,  5000 },
    { "blk-rd   4KB  page",  DMA_SLAVE,  DMA_DEV_TO_MEM, 1,     4096,  2000 },
    { "blk-rd 128KB  seq",   DMA_SLAVE,  DMA_DEV_TO_MEM, 1,   131072,   200 },
    { "blk-rd 512KB  large", DMA_SLAVE,  DMA_DEV_TO_MEM, 1,   524288,    50 },
    { "blk-wr   4KB  page",  DMA_SLAVE,  DMA_MEM_TO_DEV, 1,     4096,  2000 },
    { "blk-wr 512KB  large", DMA_SLAVE,  DMA_MEM_TO_DEV, 1,   524288,    50 },
};

#define PERF_SCENARIO_NUM  (sizeof(_G_perf_scenarios) / sizeof(_G_perf_scenarios[0]))

static UINT64  __dma_perf_run_one (const dma_perf_scenario_t *scen)
{
    dma_chan_t               *chan    = LW_NULL;
    UINT8                    *mem_buf = LW_NULL, *dev_buf = LW_NULL;
    dma_sg_entry_t           *sgl     = LW_NULL;
    dma_async_tx_descriptor_t *desc;
    dma_completion_t          comp;
    UINT64                    total_ticks = 0, ticks;
    size_t                    total_entry = (size_t)scen->sg_len * scen->entry_size;
    UINT                      r, i;

    chan    = dma_request_chan_by_name(TEST_DEV_NAME, 0);
    mem_buf = chan ? (UINT8 *)sys_malloc(total_entry) : LW_NULL;
    if (!mem_buf) { goto  _done; }

    if (scen->xfer_type == DMA_SLAVE) {
        dev_buf = (UINT8 *)sys_malloc(total_entry);
        sgl     = dev_buf ? (dma_sg_entry_t *)sys_malloc(scen->sg_len * sizeof(*sgl)) : LW_NULL;
        if (!sgl) { goto  _done; }
        lib_memset(dev_buf, 0xA5, total_entry);
        for (i = 0; i < scen->sg_len; i++) {
            sgl[i].addr = (phys_addr_t)(addr_t)(mem_buf + (size_t)i * scen->entry_size);
            sgl[i].len  = scen->entry_size;
        }
        if (__dma_slave_cfg(chan, scen->direction, dev_buf) != 0) { goto  _done; }
    }
    lib_memset(mem_buf, 0x5A, total_entry);

    if (__dma_completion_init(&comp) != 0) { goto  _done; }

    for (r = 0; r < scen->rounds; r++) {
        if (scen->xfer_type == DMA_MEMCPY) {
            desc = dmaengine_prep_dma_memcpy(chan,
                                              (phys_addr_t)(addr_t)mem_buf,
                                              (phys_addr_t)(addr_t)mem_buf,
                                              scen->entry_size, DMA_PREP_INTERRUPT);
        } else {
            desc = dmaengine_prep_slave_sg(chan, sgl, scen->sg_len,
                                            scen->direction, DMA_PREP_INTERRUPT);
        }
        if (!desc || __dma_exec_once(chan, desc, &comp, &ticks) != 0) {
            total_ticks = 0;
            break;
        }
        total_ticks += ticks;
    }
    __dma_completion_destroy(&comp);

_done:
    if (sgl)     { sys_free(sgl); }
    if (dev_buf) { sys_free(dev_buf); }
    if (mem_buf) { sys_free(mem_buf); }
    if (chan)    { dma_release_channel(chan); }
    return  total_ticks;
}

static VOID  __dma_test_perf_run_all (VOID)
{
    static CPCHAR  dir_str[] = { "M->M  ", "M->D  ", "D->M  ", "D->D  " };
    UINT           i;
    UINT64         ticks, total_bytes, ms, kbs;

    printk("\n[dma_test] ====================================================\n");
    printk("[dma_test]  DMA Perf Benchmark  (%s)  TickRate=%d Hz\n",
           TEST_DEV_NAME, LW_CFG_TICKS_PER_SEC);
    printk("[dma_test]  %-24s %-6s %3s %9s %6s %9s  Throughput\n",
           "Scenario", "Dir", "SG", "Entry-B", "Rounds", "Time-ms");
    printk("[dma_test] ----------------------------------------------------\n");

    for (i = 0; i < PERF_SCENARIO_NUM; i++) {
        const dma_perf_scenario_t *s = &_G_perf_scenarios[i];

        ticks       = __dma_perf_run_one(s);
        total_bytes = (UINT64)s->sg_len * s->entry_size * s->rounds;
        ms          = TICKS_TO_MS(ticks);
        kbs         = THROUGHPUT_KBS(total_bytes, ticks);

        if (ticks == 0) {
            printk("[dma_test]  %-24s %-6s %3u %9zu %6u  FAILED\n",
                   s->name, dir_str[s->direction], s->sg_len, s->entry_size, s->rounds);
        } else if (kbs >= 1024) {
            printk("[dma_test]  %-24s %-6s %3u %9zu %6u %9llu  %llu.%llu MB/s\n",
                   s->name, dir_str[s->direction], s->sg_len, s->entry_size, s->rounds,
                   (unsigned long long)ms,
                   (unsigned long long)(kbs / 1024),
                   (unsigned long long)((kbs % 1024) * 10 / 1024));
        } else {
            printk("[dma_test]  %-24s %-6s %3u %9zu %6u %9llu  %llu KB/s\n",
                   s->name, dir_str[s->direction], s->sg_len, s->entry_size, s->rounds,
                   (unsigned long long)ms, (unsigned long long)kbs);
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
 *  dma_test_loopback [sg_len] [entry_size] [rounds]
 *
 *  典型用法（对应常见设备场景）：
 *    dma_test_loopback                  — 4×512B 默认回环
 *    dma_test_loopback 3  500   100     — ETH 1500B 帧回环 100 次（NIC 压力测试）
 *    dma_test_loopback 8  1125  50      — Jumbo 9KB 帧回环 50 次
 *    dma_test_loopback 1  4096  1000    — 4KB 块设备读写回环 1000 次（存储完整性）
 *    dma_test_loopback 1  65536 200     — 64KB 顺序 IO 回环
 *    dma_test_loopback 1  512   5000    — 512B 扇区高频回环（吞吐量基准）
 */
static INT  __cmd_dma_test_loopback (INT iArgC, PCHAR ppcArgV[])
{
    UINT    sg  = 4, rounds = 1;
    size_t  ent = 512;

    if (iArgC >= 2) { sg     = (UINT)lib_strtoul(ppcArgV[1], LW_NULL, 10); }
    if (iArgC >= 3) { ent    = (size_t)lib_strtoul(ppcArgV[2], LW_NULL, 10); }
    if (iArgC >= 4) { rounds = (UINT)lib_strtoul(ppcArgV[3], LW_NULL, 10); }
    if (!sg || sg > TEST_SG_MAX || !ent || ent > (4 << 20) || !rounds || rounds > 100000) {
        printk("usage: dma_test_loopback [sg(1~64)] [entry(1~4194304)] [rounds(1~100000)]\n");
        return  (-1);
    }
    return  __dma_test_loopback_run(sg, ent, rounds);
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
    (VOID)iArgC; (VOID)ppcArgV;
    __dma_test_perf_run_all();
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

    API_TShellKeywordAdd("dma_test_loopback", __cmd_dma_test_loopback);
    API_TShellFormatAdd ("dma_test_loopback", " [sg] [entry] [rounds]");
    API_TShellHelpAdd   ("dma_test_loopback", "half-duplex TX->dev->RX loopback (serial)  [sg=4 entry=512 rounds=1]\n"
                         "  e.g. 'dma_test_loopback 3 500 100'   ETH frame loopback\n"
                         "       'dma_test_loopback 1 4096 1000' block I/O integrity\n");

    API_TShellKeywordAdd("dma_test_loopback_fd", __cmd_dma_test_loopback_fd);
    API_TShellFormatAdd ("dma_test_loopback_fd", " [sg] [entry] [rounds]");
    API_TShellHelpAdd   ("dma_test_loopback_fd", "full-duplex TX||RX concurrent loopback  [sg=4 entry=512 rounds=1]\n"
                         "  e.g. 'dma_test_loopback_fd 3 500 100'   ETH frame full-duplex\n"
                         "       'dma_test_loopback_fd 1 4096 1000' block I/O full-duplex\n");

    API_TShellKeywordAdd("dma_test_perf",     __cmd_dma_test_perf);
    API_TShellFormatAdd ("dma_test_perf",     "");
    API_TShellHelpAdd   ("dma_test_perf",     "full-scenario perf benchmark (15 configs auto-run)\n");

    printk("[dma_test] 6 commands registered\n");
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

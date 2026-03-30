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
** 文   件   名: axidma_loopback_test.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 27 日
**
** 描        述: AXI DMA 硬件回环独立测试程序（双线程并发）
**
**  提供简化接口用于硬件回环功能验证：
**    - AXIDMA_init()  : 初始化 TX/RX 通道
**    - AXIDMA_send()  : 发送数据到 MM2S（TX）
**    - AXIDMA_recv()  : 从 S2MM（RX）接收数据
**
**  测试模式：
**    - TX 线程：持续发送数据直到时间耗尽
**    - RX 线程：持续接收数据直到时间耗尽
**    - 双线程并发执行，测量全双工吞吐量
**
**  参数：
**    argv[1] — 每包长度（字节，默认 512）
**    argv[2] — SG 条目数（默认 1）
**    argv[3] — 测试时间（秒，默认 3）
**
*********************************************************************************************************/
#define  __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include <string.h>
#include "../dmaengine.h"

#define TEST_DEV_NAME  "xilinx-dma"

static dma_chan_t      *_G_ch_tx = NULL;
static dma_chan_t      *_G_ch_rx = NULL;
static LW_OBJECT_HANDLE _G_tx_sem = LW_OBJECT_HANDLE_INVALID;
static LW_OBJECT_HANDLE _G_rx_sem = LW_OBJECT_HANDLE_INVALID;

typedef struct {
    char          *buf;
    unsigned int   entry_size;
    unsigned int   sg_len;
    unsigned int   duration_sec;
    UINT64         rounds;
    UINT64         errors;
    UINT64         t_start;
    UINT64         t_end;
} thread_ctx_t;

/*********************************************************************************************************
** 函数名称: __axidma_tx_callback
** 功能描述: TX 完成回调
*********************************************************************************************************/
static VOID __axidma_tx_callback(PVOID param, const dmaengine_result_t *result)
{
    API_SemaphoreBPost(_G_tx_sem);
}

/*********************************************************************************************************
** 函数名称: __axidma_rx_callback
** 功能描述: RX 完成回调
*********************************************************************************************************/
static VOID __axidma_rx_callback(PVOID param, const dmaengine_result_t *result)
{
    API_SemaphoreBPost(_G_rx_sem);
}

/*********************************************************************************************************
** 函数名称: AXIDMA_init
** 功能描述: 初始化 AXI DMA TX/RX 通道
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/
int AXIDMA_init(void)
{
    dma_slave_config_t cfg;

    _G_ch_tx = dma_request_chan_by_name(TEST_DEV_NAME, 0);
    _G_ch_rx = dma_request_chan_by_name(TEST_DEV_NAME, 1);
    if (!_G_ch_tx || !_G_ch_rx) {
        printk("[axidma] FAIL: request channels\n");
        goto _err;
    }

    _G_tx_sem = API_SemaphoreBCreate("axidma_tx", LW_FALSE, LW_OPTION_OBJECT_LOCAL, NULL);
    _G_rx_sem = API_SemaphoreBCreate("axidma_rx", LW_FALSE, LW_OPTION_OBJECT_LOCAL, NULL);
    if (_G_tx_sem == LW_OBJECT_HANDLE_INVALID || _G_rx_sem == LW_OBJECT_HANDLE_INVALID) {
        printk("[axidma] FAIL: create semaphores\n");
        goto _err;
    }

    lib_memset(&cfg, 0, sizeof(cfg));
    cfg.direction = DMA_MEM_TO_DEV;
    cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
    cfg.dst_maxburst = 16;
    if (dmaengine_slave_config(_G_ch_tx, &cfg) != 0) {
        printk("[axidma] FAIL: config TX\n");
        goto _err;
    }

    cfg.direction = DMA_DEV_TO_MEM;
    cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
    cfg.src_maxburst = 16;
    if (dmaengine_slave_config(_G_ch_rx, &cfg) != 0) {
        printk("[axidma] FAIL: config RX\n");
        goto _err;
    }

    printk("[axidma] init OK\n");
    return 0;

_err:
    if (_G_tx_sem != LW_OBJECT_HANDLE_INVALID) { API_SemaphoreBDelete(&_G_tx_sem); }
    if (_G_rx_sem != LW_OBJECT_HANDLE_INVALID) { API_SemaphoreBDelete(&_G_rx_sem); }
    if (_G_ch_tx) { dma_release_channel(_G_ch_tx); _G_ch_tx = NULL; }
    if (_G_ch_rx) { dma_release_channel(_G_ch_rx); _G_ch_rx = NULL; }
    return -1;
}

/*********************************************************************************************************
** 函数名称: AXIDMA_send
** 功能描述: 发送数据到 MM2S（TX），支持 SG
** 输　入  : buff   — 数据首地址
**           len    — 总数据长度
**           sg_len — SG 条目数
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/
int AXIDMA_send(char *buff, unsigned int len, unsigned int sg_len)
{
    dma_async_tx_descriptor_t *desc;
    dma_sg_entry_t *sgl;
    unsigned int entry_size, i;

    if (!_G_ch_tx || !buff || !len || !sg_len) { return -1; }

    sgl = (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(dma_sg_entry_t));
    if (!sgl) { return -1; }

    entry_size = len / sg_len;
    for (i = 0; i < sg_len; i++) {
        sgl[i].addr = (phys_addr_t)(addr_t)(buff + i * entry_size);
        sgl[i].len = entry_size;
    }

    API_SemaphoreBClear(_G_tx_sem);
    desc = dmaengine_prep_slave_sg(_G_ch_tx, sgl, sg_len, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
    sys_free(sgl);
    if (!desc) { return -1; }

    desc->callback_result = __axidma_tx_callback;
    desc->callback_param = NULL;

    if (dmaengine_submit(desc) == DMA_COOKIE_INVALID) { return -1; }
    dma_async_issue_pending(_G_ch_tx);

    if (API_SemaphoreBPend(_G_tx_sem, LW_MSECOND_TO_TICK_1(5000)) != ERROR_NONE) {
        dmaengine_terminate_all(_G_ch_tx);
        return -1;
    }
    return 0;
}

/*********************************************************************************************************
** 函数名称: AXIDMA_recv
** 功能描述: 从 S2MM（RX）接收数据，支持 SG
** 输　入  : buff   — 接收缓冲区首地址
**           len    — 总接收数据长度
**           sg_len — SG 条目数
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/
int AXIDMA_recv(char *buff, unsigned int len, unsigned int sg_len)
{
    dma_async_tx_descriptor_t *desc;
    dma_sg_entry_t *sgl;
    unsigned int entry_size, i;

    if (!_G_ch_rx || !buff || !len || !sg_len) { return -1; }

    sgl = (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(dma_sg_entry_t));
    if (!sgl) { return -1; }

    entry_size = len / sg_len;
    for (i = 0; i < sg_len; i++) {
        sgl[i].addr = (phys_addr_t)(addr_t)(buff + i * entry_size);
        sgl[i].len = entry_size;
    }

    API_SemaphoreBClear(_G_rx_sem);
    desc = dmaengine_prep_slave_sg(_G_ch_rx, sgl, sg_len, DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);
    sys_free(sgl);
    if (!desc) { return -1; }

    desc->callback_result = __axidma_rx_callback;
    desc->callback_param = NULL;

    if (dmaengine_submit(desc) == DMA_COOKIE_INVALID) { return -1; }
    dma_async_issue_pending(_G_ch_rx);

    if (API_SemaphoreBPend(_G_rx_sem, LW_MSECOND_TO_TICK_1(5000)) != ERROR_NONE) {
        dmaengine_terminate_all(_G_ch_rx);
        return -1;
    }
    return 0;
}

/*********************************************************************************************************
** 函数名称: __tx_thread
** 功能描述: TX 线程：持续发送直到时间耗尽
*********************************************************************************************************/
static PVOID __tx_thread(PVOID arg)
{
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    UINT64 t_dur = (UINT64)ctx->duration_sec * LW_CFG_TICKS_PER_SEC;
    unsigned int total_len = ctx->entry_size * ctx->sg_len;

    ctx->t_start = API_TimeGet64();
    ctx->rounds = 0;

    while ((API_TimeGet64() - ctx->t_start) < t_dur) {
        if (AXIDMA_send(ctx->buf, total_len, ctx->sg_len) == 0) {
            ctx->rounds++;
        }
    }
    ctx->t_end = API_TimeGet64();
    return NULL;
}

/*********************************************************************************************************
** 函数名称: __rx_thread
** 功能描述: RX 线程：持续接收直到时间耗尽，并校验数据
*********************************************************************************************************/
static PVOID __rx_thread(PVOID arg)
{
    thread_ctx_t *ctx = (thread_ctx_t *)arg;
    UINT64 t_dur = (UINT64)ctx->duration_sec * LW_CFG_TICKS_PER_SEC;
    unsigned int total_len = ctx->entry_size * ctx->sg_len;
    char *ref_buf;
    unsigned int i;

    ref_buf = (char *)sys_malloc(total_len);
    if (!ref_buf) { return NULL; }
    for (i = 0; i < total_len; i++) { ref_buf[i] = (char)(i & 0xFF); }

    ctx->t_start = API_TimeGet64();
    ctx->rounds = 0;
    ctx->errors = 0;

    while ((API_TimeGet64() - ctx->t_start) < t_dur) {
        memset(ctx->buf, 0, total_len);
        if (AXIDMA_recv(ctx->buf, total_len, ctx->sg_len) == 0) {
            ctx->rounds++;
            for (i = 0; i < total_len; i++) {
                if (ctx->buf[i] != ref_buf[i]) { ctx->errors++; }
            }
        }
    }
    ctx->t_end = API_TimeGet64();
    sys_free(ref_buf);
    return NULL;
}

/*********************************************************************************************************
** 函数名称: main
** 功能描述: 测试入口
** 参　数  : argv[1] — 每包长度（字节，默认 512）
**           argv[2] — SG 条目数（默认 1）
**           argv[3] — 测试时间（秒，默认 3）
*********************************************************************************************************/
int main(int argc, char **argv)
{
    thread_ctx_t tx_ctx = {0}, rx_ctx = {0};
    LW_OBJECT_HANDLE tx_thread, rx_thread;
    unsigned int entry_size = 512, sg_len = 1, duration = 3;
    unsigned int total_len;
    UINT64 tx_bytes, rx_bytes, tx_ticks, rx_ticks;
    UINT64 tx_kbs, rx_kbs, fd_kbs;

    if (argc >= 2) { entry_size = (unsigned int)strtoul(argv[1], NULL, 10); }
    if (argc >= 3) { sg_len = (unsigned int)strtoul(argv[2], NULL, 10); }
    if (argc >= 4) { duration = (unsigned int)strtoul(argv[3], NULL, 10); }
    if (entry_size == 0 || entry_size > (4 << 20)) { entry_size = 512; }
    if (sg_len == 0 || sg_len > 64) { sg_len = 1; }
    if (duration == 0 || duration > 300) { duration = 3; }

    total_len = entry_size * sg_len;

    tx_ctx.buf = (char *)API_VmmDmaAlloc(total_len);
    rx_ctx.buf = (char *)API_VmmDmaAlloc(total_len);
    if (!tx_ctx.buf || !rx_ctx.buf) {
        printk("[axidma] FAIL: alloc buffers\n");
        return -1;
    }

    for (unsigned int i = 0; i < total_len; i++) { tx_ctx.buf[i] = (char)(i & 0xFF); }

    tx_ctx.entry_size = rx_ctx.entry_size = entry_size;
    tx_ctx.sg_len = rx_ctx.sg_len = sg_len;
    tx_ctx.duration_sec = rx_ctx.duration_sec = duration;

    if (AXIDMA_init() != 0) {
        printk("[axidma] FAIL: init\n");
        goto _out;
    }

    printk("[axidma] loopback test: entry=%u sg=%u duration=%us\n", entry_size, sg_len, duration);

    tx_thread = API_ThreadCreate("axidma_tx", __tx_thread, &tx_ctx, NULL, 8192, LW_PRIO_NORMAL);
    rx_thread = API_ThreadCreate("axidma_rx", __rx_thread, &rx_ctx, NULL, 8192, LW_PRIO_NORMAL);
    if (tx_thread == LW_OBJECT_HANDLE_INVALID || rx_thread == LW_OBJECT_HANDLE_INVALID) {
        printk("[axidma] FAIL: create threads\n");
        goto _out;
    }

    API_ThreadJoin(tx_thread, NULL);
    API_ThreadJoin(rx_thread, NULL);

    tx_bytes = (UINT64)total_len * tx_ctx.rounds;
    rx_bytes = (UINT64)total_len * rx_ctx.rounds;
    tx_ticks = tx_ctx.t_end - tx_ctx.t_start;
    rx_ticks = rx_ctx.t_end - rx_ctx.t_start;

    tx_kbs = tx_ticks ? (tx_bytes * LW_CFG_TICKS_PER_SEC / (tx_ticks * 1024)) : 0;
    rx_kbs = rx_ticks ? (rx_bytes * LW_CFG_TICKS_PER_SEC / (rx_ticks * 1024)) : 0;
    fd_kbs = tx_kbs + rx_kbs;

    printk("[axidma] === Results ===\n");
    printk("[axidma] TX: %llu rounds, ", (unsigned long long)tx_ctx.rounds);
    if (tx_kbs >= 1024) {
        printk("%llu.%llu MB/s\n", (unsigned long long)(tx_kbs/1024), (unsigned long long)((tx_kbs%1024)*10/1024));
    } else {
        printk("%llu KB/s\n", (unsigned long long)tx_kbs);
    }

    printk("[axidma] RX: %llu rounds, ", (unsigned long long)rx_ctx.rounds);
    if (rx_kbs >= 1024) {
        printk("%llu.%llu MB/s\n", (unsigned long long)(rx_kbs/1024), (unsigned long long)((rx_kbs%1024)*10/1024));
    } else {
        printk("%llu KB/s\n", (unsigned long long)rx_kbs);
    }

    printk("[axidma] FD: ");
    if (fd_kbs >= 1024) {
        printk("%llu.%llu MB/s\n", (unsigned long long)(fd_kbs/1024), (unsigned long long)((fd_kbs%1024)*10/1024));
    } else {
        printk("%llu KB/s\n", (unsigned long long)fd_kbs);
    }

    if (rx_ctx.errors == 0) {
        printk("[axidma] BER: 0 errors / %llu bytes (0%%) - PASS\n", rx_bytes);
    } else {
        UINT64 ppm = rx_ctx.errors * 1000000ULL / rx_bytes;
        printk("[axidma] BER: %llu errors / %llu bytes (%llu.%02llu%%) - FAIL\n",
               (unsigned long long)rx_ctx.errors, (unsigned long long)rx_bytes,
               (unsigned long long)(ppm/10000), (unsigned long long)((ppm%10000)/100));
    }

_out:
    if (tx_ctx.buf) { API_VmmDmaFree(tx_ctx.buf); }
    if (rx_ctx.buf) { API_VmmDmaFree(rx_ctx.buf); }
    if (_G_ch_tx) { dma_release_channel(_G_ch_tx); }
    if (_G_ch_rx) { dma_release_channel(_G_ch_rx); }
    if (_G_tx_sem != LW_OBJECT_HANDLE_INVALID) { API_SemaphoreBDelete(&_G_tx_sem); }
    if (_G_rx_sem != LW_OBJECT_HANDLE_INVALID) { API_SemaphoreBDelete(&_G_rx_sem); }

    return rx_ctx.errors ? -1 : 0;
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

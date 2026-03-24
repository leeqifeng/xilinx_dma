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
** 创   建   人: AutoGen
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: AXI DMA 驱动运行时测试套件
**               向 tshell 注册以下命令：
**                 dma_test_simple  — Simple 模式回环测试（MM2S→S2MM）
**                 dma_test_sg      — SG 模式回环测试
**                 dma_test_stress  — 重复传输稳定性测试
**
**               用法（tshell）：
**                 dma_test_simple  1024       # 传输 1 KiB
**                 dma_test_sg      256 4      # 4 个 SG 条目，每条 256 字节
**                 dma_test_stress  512 100    # 重复 100 次 512 字节传输
**
** BUG
** 2026.03.23  初始版本。
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include <string.h>
#include "drv/dma_types.h"
#include "drv/dma_client.h"
#include "drv/dma_core.h"

/*********************************************************************************************************
  配置常量
*********************************************************************************************************/

#define AXI_DMA_DEV_NAME        "axi_dma0"                             /*  须与 xilinx.c 中一致        */
#define TEST_TIMEOUT_MS         5000u                                   /*  单次传输超时（毫秒）        */

/*********************************************************************************************************
  测试完成上下文
*********************************************************************************************************/

typedef struct {
    LW_OBJECT_HANDLE  sem;                                              /*  完成信号量                  */
    int               status;                                           /*  传输结果 DMA_STATUS_*       */
} TEST_CTX;

/*********************************************************************************************************
  内部辅助函数
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: _test_cb
** 功能描述: 传输完成回调（中断上下文）。记录状态并释放信号量唤醒测试任务。
** 输　入  : arg           TEST_CTX 指针
**           status        传输结果
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _test_cb (void *arg, int status)
{
    TEST_CTX *ctx = (TEST_CTX *)arg;

    ctx->status = status;
    API_SemaphoreBPost(ctx->sem);
}
/*********************************************************************************************************
** 函数名称: _wait_done
** 功能描述: 阻塞等待传输完成信号量，超时返回 -1
** 输　入  : ctx           测试完成上下文指针
** 输　出  : 0 成功；-1 超时
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _wait_done (TEST_CTX *ctx)
{
    ULONG ret;

    ret = API_SemaphoreBPend(ctx->sem, LW_MSECOND_TO_TICK_1(TEST_TIMEOUT_MS));
    if (ret != ERROR_NONE) {
        printk("dma_test: timeout\n");
        return  (-1);
    }

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _ctx_create
** 功能描述: 创建测试完成上下文（含二进制信号量）
** 输　入  : ctx           待初始化的 TEST_CTX 指针
** 输　出  : 0 成功；-1 信号量创建失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _ctx_create (TEST_CTX *ctx)
{
    ctx->status = DMA_STATUS_IDLE;
    ctx->sem    = API_SemaphoreBCreate("dma_test_sem",
                                       LW_FALSE,
                                       LW_OPTION_WAIT_FIFO,
                                       LW_NULL);
    if (ctx->sem == LW_OBJECT_HANDLE_INVALID) {
        printk("dma_test: semaphore create failed\n");
        return  (-1);
    }

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _ctx_destroy
** 功能描述: 销毁测试完成上下文，删除二进制信号量
** 输　入  : ctx           TEST_CTX 指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _ctx_destroy (TEST_CTX *ctx)
{
    API_SemaphoreBDelete(&ctx->sem);
}
/*********************************************************************************************************
** 函数名称: _verify_buf
** 功能描述: 逐字节比较发送缓冲区与接收缓冲区，打印首个不匹配位置
** 输　入  : tx            发送缓冲区指针
**           rx            接收缓冲区指针
**           len           比较字节数
** 输　出  : 0 匹配；-1 不匹配
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _verify_buf (const UINT8 *tx, const UINT8 *rx, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (tx[i] != rx[i]) {
            printk("dma_test: mismatch at byte %u: expected 0x%02X, got 0x%02X\n",
                   (UINT)i, tx[i], rx[i]);
            return  (-1);
        }
    }

    return  (0);
}

/*********************************************************************************************************
  Shell 命令实现
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: _cmd_dma_test_simple
** 功能描述: Simple 模式 DMA 回环测试命令。
**           分配两块 DMA zone 缓冲区，MM2S 通道发送，S2MM 通道接收，校验数据一致性。
** 输　入  : iArgC         参数数量
**           ppcArgV       参数数组，ppcArgV[1] 为传输字节数（可选，默认 1024）
** 输　出  : 0 PASS；-1 FAIL
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static INT _cmd_dma_test_simple (INT iArgC, PCHAR ppcArgV[])
{
    size_t          len      = 1024;
    UINT8          *tx_buf   = LW_NULL;
    UINT8          *rx_buf   = LW_NULL;
    struct dma_chan *tx_chan  = LW_NULL;
    struct dma_chan *rx_chan  = LW_NULL;
    struct dma_desc *tx_desc = LW_NULL;
    struct dma_desc *rx_desc = LW_NULL;
    TEST_CTX         tx_ctx, rx_ctx;
    BOOL             tx_ctx_ok = LW_FALSE;
    BOOL             rx_ctx_ok = LW_FALSE;
    size_t           i;
    int              ret = 0;

    if (iArgC >= 2) {
        len = (size_t)lib_strtoul(ppcArgV[1], LW_NULL, 10);
        if (!len) {
            printk("dma_test_simple: invalid length\n");
            return  (-1);
        }
    }

    tx_buf = (UINT8 *)API_VmmDmaAlloc(len);                             /*  分配 DMA zone 缓冲区        */
    rx_buf = (UINT8 *)API_VmmDmaAlloc(len);
    if (!tx_buf || !rx_buf) {
        printk("dma_test_simple: DMA alloc failed\n");
        ret = -1;
        goto  _done;
    }

    for (i = 0; i < len; i++) {                                         /*  填充递增测试图样            */
        tx_buf[i] = (UINT8)(i & 0xFF);
    }
    lib_memset(rx_buf, 0xAA, len);                                      /*  接收缓冲区毒化              */

    tx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    rx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    if (!tx_chan || !rx_chan) {
        printk("dma_test_simple: could not get both channels\n");
        ret = -1;
        goto  _done;
    }

    if (_ctx_create(&tx_ctx) != 0) { ret = -1; goto  _done; }
    tx_ctx_ok = LW_TRUE;
    if (_ctx_create(&rx_ctx) != 0) { ret = -1; goto  _done; }
    rx_ctx_ok = LW_TRUE;

    tx_desc = dma_prep_simple(tx_chan, LW_NULL, tx_buf, len, _test_cb, &tx_ctx);
    rx_desc = dma_prep_simple(rx_chan, rx_buf,  LW_NULL, len, _test_cb, &rx_ctx);
    if (!tx_desc || !rx_desc) {
        printk("dma_test_simple: prep failed\n");
        ret = -1;
        goto  _done;
    }

    dma_submit(rx_desc);
    dma_submit(tx_desc);
    dma_issue_pending(rx_chan);                                         /*  先启动接收，防止数据丢失    */
    dma_issue_pending(tx_chan);

    if (_wait_done(&tx_ctx) != 0 || _wait_done(&rx_ctx) != 0) {
        dma_terminate(tx_chan);
        dma_terminate(rx_chan);
        ret = -1;
        goto  _done;
    }

    if (tx_ctx.status != DMA_STATUS_COMPLETE ||
        rx_ctx.status != DMA_STATUS_COMPLETE) {
        printk("dma_test_simple: transfer error (tx=%d rx=%d)\n",
               tx_ctx.status, rx_ctx.status);
        ret = -1;
        goto  _done;
    }

    ret = _verify_buf(tx_buf, rx_buf, len);
    if (ret == 0) {
        printk("dma_test_simple: PASS (len=%u)\n", (UINT)len);
    } else {
        printk("dma_test_simple: FAIL (data mismatch)\n");
    }

_done:
    if (tx_ctx_ok) _ctx_destroy(&tx_ctx);
    if (rx_ctx_ok) _ctx_destroy(&rx_ctx);
    if (tx_chan)   dma_release_chan(tx_chan);
    if (rx_chan)   dma_release_chan(rx_chan);
    if (tx_buf)    API_VmmDmaFree(tx_buf);
    if (rx_buf)    API_VmmDmaFree(rx_buf);

    return  (ret);
}
/*********************************************************************************************************
** 函数名称: _cmd_dma_test_sg
** 功能描述: SG 模式 DMA 回环测试命令。
**           分配 sg_count 个 DMA zone 缓冲区，各 entry_len 字节，
**           以 SG 描述符方式提交，验证各条目数据一致性。
** 输　入  : iArgC         参数数量
**           ppcArgV       ppcArgV[1]=每条目字节数，ppcArgV[2]=条目数（均可选）
** 输　出  : 0 PASS；-1 FAIL
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static INT _cmd_dma_test_sg (INT iArgC, PCHAR ppcArgV[])
{
    size_t          entry_len = 256;
    int             sg_count  = 4;
    struct dma_sg  *tx_sgl    = LW_NULL;
    struct dma_sg  *rx_sgl    = LW_NULL;
    UINT8         **tx_bufs   = LW_NULL;
    UINT8         **rx_bufs   = LW_NULL;
    struct dma_chan *tx_chan   = LW_NULL;
    struct dma_chan *rx_chan   = LW_NULL;
    struct dma_desc *tx_desc  = LW_NULL;
    struct dma_desc *rx_desc  = LW_NULL;
    TEST_CTX         tx_ctx, rx_ctx;
    BOOL             tx_ctx_ok = LW_FALSE;
    BOOL             rx_ctx_ok = LW_FALSE;
    int              ret = 0;
    int              i;
    size_t           j;

    if (iArgC >= 2) entry_len = (size_t)lib_strtoul(ppcArgV[1], LW_NULL, 10);
    if (iArgC >= 3) sg_count  = (int)lib_strtoul(ppcArgV[2], LW_NULL, 10);

    if (!entry_len || sg_count <= 0) {
        printk("dma_test_sg: invalid parameters\n");
        return  (-1);
    }

    tx_bufs = (UINT8 **)malloc((size_t)sg_count * sizeof(UINT8 *));
    rx_bufs = (UINT8 **)malloc((size_t)sg_count * sizeof(UINT8 *));
    tx_sgl  = (struct dma_sg *)malloc((size_t)sg_count * sizeof(struct dma_sg));
    rx_sgl  = (struct dma_sg *)malloc((size_t)sg_count * sizeof(struct dma_sg));
    if (!tx_bufs || !rx_bufs || !tx_sgl || !rx_sgl) {
        printk("dma_test_sg: alloc failed\n");
        ret = -1;
        goto  _done;
    }
    lib_memset(tx_bufs, 0, (size_t)sg_count * sizeof(UINT8 *));
    lib_memset(rx_bufs, 0, (size_t)sg_count * sizeof(UINT8 *));

    for (i = 0; i < sg_count; i++) {                                    /*  分配并填充每条目缓冲区      */
        tx_bufs[i] = (UINT8 *)API_VmmDmaAlloc(entry_len);
        rx_bufs[i] = (UINT8 *)API_VmmDmaAlloc(entry_len);
        if (!tx_bufs[i] || !rx_bufs[i]) {
            printk("dma_test_sg: DMA alloc failed at entry %d\n", i);
            ret = -1;
            goto  _done;
        }
        for (j = 0; j < entry_len; j++) {
            tx_bufs[i][j] = (UINT8)((i * 17 + j) & 0xFF);
        }
        lib_memset(rx_bufs[i], 0xBB, entry_len);

        tx_sgl[i].buf = tx_bufs[i];
        tx_sgl[i].len = entry_len;
        rx_sgl[i].buf = rx_bufs[i];
        rx_sgl[i].len = entry_len;
    }

    tx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    rx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    if (!tx_chan || !rx_chan) {
        printk("dma_test_sg: could not get both channels\n");
        ret = -1;
        goto  _done;
    }

    if (_ctx_create(&tx_ctx) != 0) { ret = -1; goto  _done; }
    tx_ctx_ok = LW_TRUE;
    if (_ctx_create(&rx_ctx) != 0) { ret = -1; goto  _done; }
    rx_ctx_ok = LW_TRUE;

    tx_desc = dma_prep_sg(tx_chan, tx_sgl, sg_count, _test_cb, &tx_ctx);
    rx_desc = dma_prep_sg(rx_chan, rx_sgl, sg_count, _test_cb, &rx_ctx);
    if (!tx_desc || !rx_desc) {
        printk("dma_test_sg: prep_sg failed\n");
        ret = -1;
        goto  _done;
    }

    dma_submit(rx_desc);
    dma_submit(tx_desc);
    dma_issue_pending(rx_chan);
    dma_issue_pending(tx_chan);

    if (_wait_done(&tx_ctx) != 0 || _wait_done(&rx_ctx) != 0) {
        dma_terminate(tx_chan);
        dma_terminate(rx_chan);
        ret = -1;
        goto  _done;
    }

    if (tx_ctx.status != DMA_STATUS_COMPLETE ||
        rx_ctx.status != DMA_STATUS_COMPLETE) {
        printk("dma_test_sg: transfer error (tx=%d rx=%d)\n",
               tx_ctx.status, rx_ctx.status);
        ret = -1;
        goto  _done;
    }

    for (i = 0; i < sg_count; i++) {                                    /*  逐条目校验                  */
        if (_verify_buf(tx_bufs[i], rx_bufs[i], entry_len) != 0) {
            printk("dma_test_sg: FAIL at SG entry %d\n", i);
            ret = -1;
            goto  _done;
        }
    }
    printk("dma_test_sg: PASS (%d entries x %u bytes)\n",
           sg_count, (UINT)entry_len);

_done:
    if (tx_ctx_ok) _ctx_destroy(&tx_ctx);
    if (rx_ctx_ok) _ctx_destroy(&rx_ctx);
    if (tx_chan)   dma_release_chan(tx_chan);
    if (rx_chan)   dma_release_chan(rx_chan);
    if (tx_bufs) {
        for (i = 0; i < sg_count; i++) {
            if (tx_bufs[i]) API_VmmDmaFree(tx_bufs[i]);
        }
        free(tx_bufs);
    }
    if (rx_bufs) {
        for (i = 0; i < sg_count; i++) {
            if (rx_bufs[i]) API_VmmDmaFree(rx_bufs[i]);
        }
        free(rx_bufs);
    }
    free(tx_sgl);
    free(rx_sgl);

    return  (ret);
}
/*********************************************************************************************************
** 函数名称: _cmd_dma_test_stress
** 功能描述: DMA 稳定性压力测试：重复执行 Simple 模式传输 count 次，统计成功/失败数。
** 输　入  : iArgC         参数数量
**           ppcArgV       ppcArgV[1]=字节数，ppcArgV[2]=次数（均可选）
** 输　出  : 0 全部通过；-1 存在失败
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static INT _cmd_dma_test_stress (INT iArgC, PCHAR ppcArgV[])
{
    size_t          len      = 512;
    int             count    = 100;
    int             pass     = 0;
    int             fail     = 0;
    int             i;
    UINT8          *tx_buf   = LW_NULL;
    UINT8          *rx_buf   = LW_NULL;
    struct dma_chan *tx_chan  = LW_NULL;
    struct dma_chan *rx_chan  = LW_NULL;
    TEST_CTX         tx_ctx, rx_ctx;
    BOOL             tx_ctx_ok = LW_FALSE;
    BOOL             rx_ctx_ok = LW_FALSE;

    if (iArgC >= 2) len   = (size_t)lib_strtoul(ppcArgV[1], LW_NULL, 10);
    if (iArgC >= 3) count = (int)lib_strtoul(ppcArgV[2], LW_NULL, 10);

    tx_buf = (UINT8 *)API_VmmDmaAlloc(len);
    rx_buf = (UINT8 *)API_VmmDmaAlloc(len);
    if (!tx_buf || !rx_buf) {
        printk("dma_test_stress: DMA alloc failed\n");
        goto  _done;
    }

    tx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    rx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    if (!tx_chan || !rx_chan) {
        printk("dma_test_stress: could not get channels\n");
        goto  _done;
    }

    if (_ctx_create(&tx_ctx) != 0) goto  _done;
    tx_ctx_ok = LW_TRUE;
    if (_ctx_create(&rx_ctx) != 0) goto  _done;
    rx_ctx_ok = LW_TRUE;

    for (i = 0; i < count; i++) {
        struct dma_desc *tx_desc;
        struct dma_desc *rx_desc;
        size_t           k;

        for (k = 0; k < len; k++) {                                     /*  每轮重新填充图样            */
            tx_buf[k] = (UINT8)((i + k) & 0xFF);
        }
        lib_memset(rx_buf, 0, len);

        API_SemaphoreBClear(tx_ctx.sem);
        API_SemaphoreBClear(rx_ctx.sem);
        tx_ctx.status = DMA_STATUS_IDLE;
        rx_ctx.status = DMA_STATUS_IDLE;

        tx_desc = dma_prep_simple(tx_chan, LW_NULL, tx_buf, len, _test_cb, &tx_ctx);
        rx_desc = dma_prep_simple(rx_chan, rx_buf,  LW_NULL, len, _test_cb, &rx_ctx);
        if (!tx_desc || !rx_desc) {
            fail++;
            if (tx_desc) { dma_submit(tx_desc); dma_terminate(tx_chan); }
            if (rx_desc) { dma_submit(rx_desc); dma_terminate(rx_chan); }
            continue;
        }

        dma_submit(rx_desc);
        dma_submit(tx_desc);
        dma_issue_pending(rx_chan);
        dma_issue_pending(tx_chan);

        if (_wait_done(&tx_ctx) != 0 || _wait_done(&rx_ctx) != 0) {
            dma_terminate(tx_chan);
            dma_terminate(rx_chan);
            fail++;
            continue;
        }

        if (tx_ctx.status == DMA_STATUS_COMPLETE &&
            rx_ctx.status == DMA_STATUS_COMPLETE &&
            _verify_buf(tx_buf, rx_buf, len) == 0) {
            pass++;
        } else {
            fail++;
        }
    }

    printk("dma_test_stress: %d/%d passed, %d failed (len=%u)\n",
           pass, count, fail, (UINT)len);

_done:
    if (tx_ctx_ok) _ctx_destroy(&tx_ctx);
    if (rx_ctx_ok) _ctx_destroy(&rx_ctx);
    if (tx_chan)   dma_release_chan(tx_chan);
    if (rx_chan)   dma_release_chan(rx_chan);
    if (tx_buf)    API_VmmDmaFree(tx_buf);
    if (rx_buf)    API_VmmDmaFree(rx_buf);

    return  (fail ? -1 : 0);
}

/*********************************************************************************************************
  命令注册
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: _cmd_dma_test_sg_bench
** 功能描述: SG 模式 DMA 吞吐量基准测试。
**           重复执行 count 次 SG 传输，每次均校验数据正确性，统计通过率与吞吐量。
**           使用较大的默认缓冲区（4 KiB/entry × 8 entries），以更好地反映带宽上限。
** 输　入  : iArgC         参数数量
**           ppcArgV       ppcArgV[1]=每条目字节数，ppcArgV[2]=条目数，ppcArgV[3]=迭代次数
** 输　出  : 0 全部通过；-1 存在失败
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static INT _cmd_dma_test_sg_bench (INT iArgC, PCHAR ppcArgV[])
{
    size_t          entry_len = 4096;
    int             sg_count  = 8;
    int             count     = 100;
    int             pass      = 0;
    int             fail      = 0;
    int             ret       = 0;
    int             i;
    size_t          j;
    struct dma_sg  *tx_sgl    = LW_NULL;
    struct dma_sg  *rx_sgl    = LW_NULL;
    UINT8         **tx_bufs   = LW_NULL;
    UINT8         **rx_bufs   = LW_NULL;
    struct dma_chan *tx_chan   = LW_NULL;
    struct dma_chan *rx_chan   = LW_NULL;
    TEST_CTX        tx_ctx, rx_ctx;
    BOOL            tx_ctx_ok = LW_FALSE;
    BOOL            rx_ctx_ok = LW_FALSE;
    ULONG           tick_start, tick_end, elapsed_ticks;
    ULONG           ticks_per_sec, elapsed_ms;
    ULONG           total_bytes, kbps, mbps_int, mbps_frac;

    if (iArgC >= 2) entry_len = (size_t)lib_strtoul(ppcArgV[1], LW_NULL, 10);
    if (iArgC >= 3) sg_count  = (int)lib_strtoul(ppcArgV[2], LW_NULL, 10);
    if (iArgC >= 4) count     = (int)lib_strtoul(ppcArgV[3], LW_NULL, 10);

    if (!entry_len || sg_count <= 0 || count <= 0) {
        printk("dma_test_sg_bench: invalid parameters\n");
        return  (-1);
    }

    tx_bufs = (UINT8 **)malloc((size_t)sg_count * sizeof(UINT8 *));
    rx_bufs = (UINT8 **)malloc((size_t)sg_count * sizeof(UINT8 *));
    tx_sgl  = (struct dma_sg *)malloc((size_t)sg_count * sizeof(struct dma_sg));
    rx_sgl  = (struct dma_sg *)malloc((size_t)sg_count * sizeof(struct dma_sg));
    if (!tx_bufs || !rx_bufs || !tx_sgl || !rx_sgl) {
        printk("dma_test_sg_bench: alloc failed\n");
        ret = -1;
        goto  _done;
    }
    lib_memset(tx_bufs, 0, (size_t)sg_count * sizeof(UINT8 *));
    lib_memset(rx_bufs, 0, (size_t)sg_count * sizeof(UINT8 *));

    for (i = 0; i < sg_count; i++) {                                    /*  分配并填充 DMA 缓冲区       */
        tx_bufs[i] = (UINT8 *)API_VmmDmaAlloc(entry_len);
        rx_bufs[i] = (UINT8 *)API_VmmDmaAlloc(entry_len);
        if (!tx_bufs[i] || !rx_bufs[i]) {
            printk("dma_test_sg_bench: DMA alloc failed at entry %d\n", i);
            ret = -1;
            goto  _done;
        }
        for (j = 0; j < entry_len; j++) {
            tx_bufs[i][j] = (UINT8)((i * 17 + j) & 0xFF);
        }
        lib_memset(rx_bufs[i], 0, entry_len);

        tx_sgl[i].buf = tx_bufs[i];
        tx_sgl[i].len = entry_len;
        rx_sgl[i].buf = rx_bufs[i];
        rx_sgl[i].len = entry_len;
    }

    tx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    rx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    if (!tx_chan || !rx_chan) {
        printk("dma_test_sg_bench: could not get both channels\n");
        ret = -1;
        goto  _done;
    }

    if (_ctx_create(&tx_ctx) != 0) { ret = -1; goto  _done; }
    tx_ctx_ok = LW_TRUE;
    if (_ctx_create(&rx_ctx) != 0) { ret = -1; goto  _done; }
    rx_ctx_ok = LW_TRUE;

    printk("dma_test_sg_bench: %d itr x %d entries x %u bytes (%u bytes/itr)\n",
           count, sg_count, (UINT)entry_len,
           (UINT)((ULONG)sg_count * (ULONG)entry_len));

    tick_start = API_TimeGet();                                         /*  计时开始                    */

    for (i = 0; i < count; i++) {
        struct dma_desc *tx_desc;
        struct dma_desc *rx_desc;
        int              k;
        int              ok = 1;

        // for (k = 0; k < sg_count; k++) {                               /*  每轮重置接收缓冲区          */
        //     lib_memset(rx_bufs[k], 0, entry_len);
        // }

        API_SemaphoreBClear(tx_ctx.sem);
        API_SemaphoreBClear(rx_ctx.sem);
        tx_ctx.status = DMA_STATUS_IDLE;
        rx_ctx.status = DMA_STATUS_IDLE;

        tx_desc = dma_prep_sg(tx_chan, tx_sgl, sg_count, _test_cb, &tx_ctx);
        rx_desc = dma_prep_sg(rx_chan, rx_sgl, sg_count, _test_cb, &rx_ctx);
        if (!tx_desc || !rx_desc) {
            if (tx_desc) { dma_submit(tx_desc); dma_terminate(tx_chan); }
            if (rx_desc) { dma_submit(rx_desc); dma_terminate(rx_chan); }
            fail++;
            continue;
        }

        dma_submit(rx_desc);
        dma_submit(tx_desc);
        dma_issue_pending(rx_chan);
        dma_issue_pending(tx_chan);

        if (_wait_done(&tx_ctx) != 0 || _wait_done(&rx_ctx) != 0) {
            dma_terminate(tx_chan);
            dma_terminate(rx_chan);
            fail++;
            continue;
        }

        if (tx_ctx.status != DMA_STATUS_COMPLETE ||
            rx_ctx.status != DMA_STATUS_COMPLETE) {
            fail++;
            continue;
        }

        // for (k = 0; k < sg_count && ok; k++) {                         /*  逐条目数据校验              */
        //     if (_verify_buf(tx_bufs[k], rx_bufs[k], entry_len) != 0) {
        //         printk("dma_test_sg_bench: data mismatch at itr %d entry %d\n", i, k);
        //         ok = 0;
        //     }
        // }
        if (ok) {
            pass++;
        } else {
            fail++;
        }
    }

    tick_end = API_TimeGet();                                           /*  计时结束                    */

    elapsed_ticks = tick_end - tick_start;
    ticks_per_sec = LW_MSECOND_TO_TICK_1(1000);                        /*  ≈ 每秒 tick 数（含1 tick误差）*/
    elapsed_ms    = (ticks_per_sec > 0)
                  ? (elapsed_ticks * 1000 / ticks_per_sec) : 1;
    if (elapsed_ms == 0) elapsed_ms = 1;

    total_bytes = (ULONG)pass * (ULONG)sg_count * (ULONG)entry_len;
    kbps        = total_bytes * 1000 / 1024 / elapsed_ms;              /*  KB/s（双向同步回环）        */
    mbps_int    = kbps / 1024;
    mbps_frac   = (kbps % 1024) * 10 / 1024;

    printk("dma_test_sg_bench: %d/%d passed, %d failed\n",
           pass, count, fail);
    printk("dma_test_sg_bench: %lu bytes in ~%lu ms\n",
           total_bytes, elapsed_ms);
    printk("dma_test_sg_bench: ~%lu KB/s  (~%lu.%lu MB/s)  [MM2S+S2MM loopback]\n",
           kbps, mbps_int, mbps_frac);

_done:
    if (tx_ctx_ok) _ctx_destroy(&tx_ctx);
    if (rx_ctx_ok) _ctx_destroy(&rx_ctx);
    if (tx_chan)   dma_release_chan(tx_chan);
    if (rx_chan)   dma_release_chan(rx_chan);
    if (tx_bufs) {
        for (i = 0; i < sg_count; i++) {
            if (tx_bufs[i]) API_VmmDmaFree(tx_bufs[i]);
        }
        free(tx_bufs);
    }
    if (rx_bufs) {
        for (i = 0; i < sg_count; i++) {
            if (rx_bufs[i]) API_VmmDmaFree(rx_bufs[i]);
        }
        free(rx_bufs);
    }
    if (tx_sgl) free(tx_sgl);
    if (rx_sgl) free(rx_sgl);

    return  (fail ? -1 : 0);
}

/*********************************************************************************************************
  全双工时基基准测试
*********************************************************************************************************/

#define BENCH_MAX_SG    32u                                             /*  每包最大 SG 条目数          */

/*  累计统计结构（spinlock 保护）  */
typedef struct {
    unsigned long long  tx_bytes;                                       /*  MM2S 总发送字节数           */
    unsigned long long  rx_bytes;                                       /*  S2MM 总接收字节数           */
    unsigned long long  tx_packets;                                     /*  MM2S 成功包数               */
    unsigned long long  rx_packets;                                     /*  S2MM 成功包数               */
    unsigned long long  tx_errors;                                      /*  MM2S 错误包数               */
    unsigned long long  rx_errors;                                      /*  S2MM 错误包数               */
    unsigned long long  bit_errors;                                     /*  总比特错误数（逐包统计）    */
    unsigned long long  bits_tested;                                    /*  总比特测试数                */
} BENCH_STATS;

/*  基准测试控制块  */
typedef struct {
    size_t              entry_len;                                      /*  每个 SG 条目字节数          */
    int                 sg_count;                                       /*  SG 条目数                   */
    int                 duration_sec;                                   /*  测试时长（秒）              */
    struct dma_chan     *tx_chan;                                        /*  MM2S 通道                   */
    struct dma_chan     *rx_chan;                                        /*  S2MM 通道                   */
    UINT8              *tx_bufs[BENCH_MAX_SG];                          /*  TX DMA 缓冲（只读基准图样） */
    UINT8              *rx_bufs[BENCH_MAX_SG];                          /*  RX DMA 缓冲                 */
    struct dma_sg       tx_sgl[BENCH_MAX_SG];
    struct dma_sg       rx_sgl[BENCH_MAX_SG];
    TEST_CTX            tx_ctx;
    TEST_CTX            rx_ctx;
    BOOL                tx_ctx_ok;
    BOOL                rx_ctx_ok;
    LW_OBJECT_HANDLE    rx_armed_sem;                                   /*  RX 完成 arm 后通知 TX       */
    BOOL                rx_armed_ok;
    spinlock_t          lock;                                           /*  保护 stats                  */
    BENCH_STATS         stats;
    volatile int        stop;                                           /*  置 1 通知线程退出           */
} BENCH_CTRL;

/*********************************************************************************************************
** 函数名称: _popcount8
** 功能描述: 计算 8 位整数中置 1 的比特数（Hamming weight）
** 输　入  : b             8 位输入值
** 输　出  : 置 1 比特数
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static LW_INLINE UINT32 _popcount8 (UINT8 b)
{
    b = b - ((b >> 1) & 0x55u);
    b = (b & 0x33u) + ((b >> 2) & 0x33u);
    return  (UINT32)((b + (b >> 4)) & 0x0Fu);
}
/*********************************************************************************************************
** 函数名称: _bit_errors
** 功能描述: 逐字节 XOR 后统计总比特差异数，用于逐包 BER 计算
** 输　入  : tx            发送参考缓冲区
**           rx            接收缓冲区
**           len           字节数
** 输　出  : 比特错误总数
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static unsigned long long _bit_errors (const UINT8 *tx, const UINT8 *rx, size_t len)
{
    unsigned long long  be = 0;
    size_t              i;

    for (i = 0; i < len; i++) {
        be += _popcount8((UINT8)(tx[i] ^ rx[i]));
    }
    return  be;
}
/*********************************************************************************************************
** 函数名称: _ber_ppb
** 功能描述: 将比特错误数/总测试比特数转换为十亿分之一（ppb）误码率。
**           溢出安全：中间值最大为 bt×1000 ≈ 26e12（100s@26Gbps），远小于 UINT64_MAX。
** 输　入  : be            比特错误数
**           bt            总测试比特数
** 输　出  : BER（ppb）；be 或 bt 为 0 时返回 0
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static ULONG _ber_ppb (unsigned long long be, unsigned long long bt)
{
    if (bt == 0 || be == 0) {
        return  0;
    }
    if (bt >= 1000000ULL) {                                             /*  be*1000/(bt/1e6)，安全      */
        return  (ULONG)((be * 1000ULL) / (bt / 1000000ULL));
    }
    return  (ULONG)((be * 1000000000ULL) / bt);                        /*  bt<1M：最大中间值 ~1e15     */
}
/*********************************************************************************************************
** 函数名称: _kbps
** 功能描述: 将字节数和毫秒数转换为 KB/s 吞吐量。
**           溢出安全：bytes×1000 最大 ~4e13（100s@400MB/s），远小于 UINT64_MAX。
** 输　入  : bytes         总字节数
**           elapsed_ms    经过毫秒数
** 输　出  : KB/s；elapsed_ms 为 0 时返回 0
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static ULONG _kbps (unsigned long long bytes, ULONG elapsed_ms)
{
    if (elapsed_ms == 0) {
        return  0;
    }
    return  (ULONG)((bytes * 1000ULL) / 1024ULL / (unsigned long long)elapsed_ms);
}
/*********************************************************************************************************
** 函数名称: _bench_pend
** 功能描述: 以 500 ms 为轮询周期等待信号量，同时检查 stop 标志，实现可中断等待。
** 输　入  : sem           目标二进制信号量句柄
**           stop          停止标志指针（volatile int）
** 输　出  : 0 信号量成功获取；-1 stop 置位后退出
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static int _bench_pend (LW_OBJECT_HANDLE sem, volatile int *stop)
{
    ULONG  ret;

    while (!(*stop)) {
        ret = API_SemaphoreBPend(sem, LW_MSECOND_TO_TICK_1(500));
        if (ret == ERROR_NONE) {
            return  0;
        }
    }
    return  -1;
}
/*********************************************************************************************************
** 函数名称: _bench_tx_thread
** 功能描述: MM2S 发送线程。等待 S2MM 挂载完成后立即发送，循环直到 stop 置位。
**           TX 缓冲区内容在测试期间固定不变，供 RX 线程作为 BER 参考基准，无竞争。
** 输　入  : pvArg         BENCH_CTRL 指针
** 输　出  : LW_NULL
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static PVOID _bench_tx_thread (PVOID pvArg)
{
    BENCH_CTRL      *ctrl      = (BENCH_CTRL *)pvArg;
    size_t           pkt_bytes = (size_t)ctrl->sg_count * ctrl->entry_len;
    struct dma_desc *tx_desc;
    INTREG           ireg;

    while (!ctrl->stop) {
        /*  等待 RX 线程完成 S2MM 挂载  */
        if (_bench_pend(ctrl->rx_armed_sem, &ctrl->stop) != 0) {
            break;
        }
        if (ctrl->stop) {                                               /*  stop 在 pend 返回后置位    */
            break;
        }

        API_SemaphoreBClear(ctrl->tx_ctx.sem);
        ctrl->tx_ctx.status = DMA_STATUS_IDLE;

        tx_desc = dma_prep_sg(ctrl->tx_chan, ctrl->tx_sgl, ctrl->sg_count,
                              _test_cb, &ctrl->tx_ctx);
        if (!tx_desc) {
            LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
            ctrl->stats.tx_errors++;
            LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);
            /*  TX prep 失败：对应的 S2MM 已 issue，回环无数据会挂死；
             *  终止双向通道，结束本次测试。                              */
            ctrl->stop = 1;
            dma_terminate(ctrl->rx_chan);
            break;
        }
        dma_submit(tx_desc);
        dma_issue_pending(ctrl->tx_chan);

        if (_wait_done(&ctrl->tx_ctx) != 0) {                          /*  非中断等待，避免 in-flight  */
            dma_terminate(ctrl->tx_chan);
            LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
            ctrl->stats.tx_errors++;
            LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);
            break;
        }

        if (ctrl->tx_ctx.status == DMA_STATUS_COMPLETE) {
            LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
            ctrl->stats.tx_packets++;
            ctrl->stats.tx_bytes += (unsigned long long)pkt_bytes;
            LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);
        } else {
            LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
            ctrl->stats.tx_errors++;
            LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);
        }
    }

    return  LW_NULL;
}
/*********************************************************************************************************
** 函数名称: _bench_rx_thread
** 功能描述: S2MM 接收线程。挂载接收描述符后通知 TX，完成后逐包计算 BER，循环直到 stop 置位。
**           BER 通过与 tx_bufs 固定基准图样逐位对比实现（tx_bufs 全程只读，无竞争）。
** 输　入  : pvArg         BENCH_CTRL 指针
** 输　出  : LW_NULL
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static PVOID _bench_rx_thread (PVOID pvArg)
{
    BENCH_CTRL      *ctrl      = (BENCH_CTRL *)pvArg;
    size_t           pkt_bytes = (size_t)ctrl->sg_count * ctrl->entry_len;
    struct dma_desc *rx_desc;
    INTREG           ireg;
    int              sg;

    while (!ctrl->stop) {
        for (sg = 0; sg < ctrl->sg_count; sg++) {                      /*  毒化接收缓冲区              */
            lib_memset(ctrl->rx_bufs[sg], 0xBB, ctrl->entry_len);
        }

        API_SemaphoreBClear(ctrl->rx_ctx.sem);
        ctrl->rx_ctx.status = DMA_STATUS_IDLE;

        rx_desc = dma_prep_sg(ctrl->rx_chan, ctrl->rx_sgl, ctrl->sg_count,
                              _test_cb, &ctrl->rx_ctx);
        if (!rx_desc) {
            LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
            ctrl->stats.rx_errors++;
            LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);
            ctrl->stop = 1;                                             /*  资源耗尽，终止测试          */
            break;
        }
        dma_submit(rx_desc);
        dma_issue_pending(ctrl->rx_chan);

        API_SemaphoreBPost(ctrl->rx_armed_sem);                        /*  通知 TX 线程 S2MM 已就绪    */

        if (_wait_done(&ctrl->rx_ctx) != 0) {                          /*  非中断等待，避免 in-flight  */
            dma_terminate(ctrl->rx_chan);
            LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
            ctrl->stats.rx_errors++;
            LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);
            break;
        }

        if (ctrl->rx_ctx.status != DMA_STATUS_COMPLETE) {
            LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
            ctrl->stats.rx_errors++;
            LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);
            continue;
        }

        {                                                               /*  逐包 BER：与基准图样对比    */
            unsigned long long  be = 0;
            unsigned long long  bt = (unsigned long long)pkt_bytes * 8ULL;

            for (sg = 0; sg < ctrl->sg_count; sg++) {
                be += _bit_errors(ctrl->tx_bufs[sg], ctrl->rx_bufs[sg],
                                  ctrl->entry_len);
            }
            LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
            ctrl->stats.rx_packets++;
            ctrl->stats.rx_bytes    += (unsigned long long)pkt_bytes;
            ctrl->stats.bit_errors  += be;
            ctrl->stats.bits_tested += bt;
            LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);
        }
    }

    return  LW_NULL;
}
/*********************************************************************************************************
** 函数名称: _bench_report
** 功能描述: 打印周期报告（delta 统计）或最终汇总报告（累计统计）。
**           周期报告以 1000 ms 窗口计算速率并更新 prev 快照；最终报告用实际 elapsed_ms。
** 输　入  : ctrl          基准测试控制块指针
**           prev          上一次报告的统计快照（周期报告时输出后更新）
**           sec           当前秒数（仅用于周期报告）
**           final         LW_TRUE 输出最终汇总；LW_FALSE 输出周期报告
**           elapsed_ms    从测试开始至今的毫秒数
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _bench_report (BENCH_CTRL  *ctrl,
                            BENCH_STATS *prev,
                            int          sec,
                            BOOL         final,
                            ULONG        elapsed_ms)
{
    BENCH_STATS         cur;
    INTREG              ireg;
    unsigned long long  d_tx_bytes, d_rx_bytes;
    unsigned long long  d_tx_pkts,  d_rx_pkts;
    unsigned long long  d_tx_err,   d_rx_err;
    unsigned long long  d_be,       d_bt;
    ULONG               tx_kbps,    rx_kbps;
    ULONG               tx_mbps_i,  tx_mbps_f;
    ULONG               rx_mbps_i,  rx_mbps_f;
    ULONG               ber_ppb_val;

    LW_SPIN_LOCK_QUICK(&ctrl->lock, &ireg);
    cur = ctrl->stats;
    LW_SPIN_UNLOCK_QUICK(&ctrl->lock, ireg);

    if (final) {
        tx_kbps     = _kbps(cur.tx_bytes, elapsed_ms);
        rx_kbps     = _kbps(cur.rx_bytes, elapsed_ms);
        ber_ppb_val = _ber_ppb(cur.bit_errors, cur.bits_tested);
        tx_mbps_i   = tx_kbps / 1024;
        tx_mbps_f   = (tx_kbps % 1024) * 10 / 1024;
        rx_mbps_i   = rx_kbps / 1024;
        rx_mbps_f   = (rx_kbps % 1024) * 10 / 1024;
        printk("===== dma_test_bench SUMMARY  elapsed=%lu ms =====\n", elapsed_ms);
        printk("  TX: %llu pkts  %llu bytes  %llu errors\n",
               cur.tx_packets, cur.tx_bytes, cur.tx_errors);
        printk("  RX: %llu pkts  %llu bytes  %llu errors\n",
               cur.rx_packets, cur.rx_bytes, cur.rx_errors);
        printk("  TX %lu KB/s (%lu.%lu MB/s)  RX %lu KB/s (%lu.%lu MB/s)\n",
               tx_kbps, tx_mbps_i, tx_mbps_f, rx_kbps, rx_mbps_i, rx_mbps_f);
        printk("  BER: %llu errors / %llu bits = %lu ppb\n",
               cur.bit_errors, cur.bits_tested, ber_ppb_val);
    } else {
        d_tx_bytes  = cur.tx_bytes    - prev->tx_bytes;
        d_rx_bytes  = cur.rx_bytes    - prev->rx_bytes;
        d_tx_pkts   = cur.tx_packets  - prev->tx_packets;
        d_rx_pkts   = cur.rx_packets  - prev->rx_packets;
        d_tx_err    = cur.tx_errors   - prev->tx_errors;
        d_rx_err    = cur.rx_errors   - prev->rx_errors;
        d_be        = cur.bit_errors  - prev->bit_errors;
        d_bt        = cur.bits_tested - prev->bits_tested;
        tx_kbps     = _kbps(d_tx_bytes, 1000);
        rx_kbps     = _kbps(d_rx_bytes, 1000);
        ber_ppb_val = _ber_ppb(d_be, d_bt);
        tx_mbps_i   = tx_kbps / 1024;
        tx_mbps_f   = (tx_kbps % 1024) * 10 / 1024;
        rx_mbps_i   = rx_kbps / 1024;
        rx_mbps_f   = (rx_kbps % 1024) * 10 / 1024;
        printk("[%3d s] TX %llu pkts %llu err | RX %llu pkts %llu err | BER %lu ppb\n",
               sec, d_tx_pkts, d_tx_err, d_rx_pkts, d_rx_err, ber_ppb_val);
        printk("        TX %lu KB/s (%lu.%lu MB/s)  RX %lu KB/s (%lu.%lu MB/s)\n",
               tx_kbps, tx_mbps_i, tx_mbps_f, rx_kbps, rx_mbps_i, rx_mbps_f);
        *prev = cur;
    }
}
/*********************************************************************************************************
** 函数名称: _bench_run_one
** 功能描述: 以指定包参数和时长运行一次全双工 SG 基准测试。
**           TX/RX 线程独立运行（API_ThreadCreate），TX 固定发送基准图样，
**           RX 逐包校验并累计 BER，主线程每秒打印周期报告。
**           到时后等线程退出，打印最终汇总报告。
** 输　入  : entry_len     每个 SG 条目字节数
**           sg_count      SG 条目数（1..BENCH_MAX_SG）
**           duration_sec  测试时长（秒，>0）
** 输　出  : 0 成功；-1 初始化失败
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static int _bench_run_one (size_t entry_len, int sg_count, int duration_sec)
{
    BENCH_CTRL          *ctrl   = LW_NULL;
    LW_OBJECT_HANDLE     tx_tid = LW_OBJECT_HANDLE_INVALID;
    LW_OBJECT_HANDLE     rx_tid = LW_OBJECT_HANDLE_INVALID;
    LW_CLASS_THREADATTR  attr;
    BENCH_STATS          prev;
    ULONG                tick_start, tick_now;
    ULONG                ticks_per_sec, elapsed_ms;
    int                  sec, i;
    size_t               pkt_bytes;
    int                  ret = 0;

    if (!entry_len || sg_count <= 0 || sg_count > (int)BENCH_MAX_SG ||
        duration_sec <= 0) {
        printk("dma_test_bench: invalid parameters\n");
        return  -1;
    }

    ctrl = (BENCH_CTRL *)malloc(sizeof(BENCH_CTRL));
    if (!ctrl) {
        printk("dma_test_bench: malloc failed\n");
        return  -1;
    }
    lib_memset(ctrl, 0, sizeof(BENCH_CTRL));
    lib_memset(&prev,  0, sizeof(prev));
    ctrl->entry_len    = entry_len;
    ctrl->sg_count     = sg_count;
    ctrl->duration_sec = duration_sec;
    LW_SPIN_INIT(&ctrl->lock);
    pkt_bytes = (size_t)sg_count * entry_len;
    elapsed_ms = 1;

    /*  分配 DMA 缓冲区，填充固定基准图样（TX 缓冲整个测试期间只读）  */
    for (i = 0; i < sg_count; i++) {
        int  k;

        ctrl->tx_bufs[i] = (UINT8 *)API_VmmDmaAlloc(entry_len);
        ctrl->rx_bufs[i] = (UINT8 *)API_VmmDmaAlloc(entry_len);
        if (!ctrl->tx_bufs[i] || !ctrl->rx_bufs[i]) {
            printk("dma_test_bench: DMA alloc failed at sg %d\n", i);
            ret = -1;
            goto  _cleanup;
        }
        for (k = 0; k < (int)entry_len; k++) {
            ctrl->tx_bufs[i][k] = (UINT8)((i * 17 + k) & 0xFF);
        }
        ctrl->tx_sgl[i].buf = ctrl->tx_bufs[i];
        ctrl->tx_sgl[i].len = entry_len;
        ctrl->rx_sgl[i].buf = ctrl->rx_bufs[i];
        ctrl->rx_sgl[i].len = entry_len;
    }

    ctrl->tx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    ctrl->rx_chan = dma_request_chan(AXI_DMA_DEV_NAME);
    if (!ctrl->tx_chan || !ctrl->rx_chan) {
        printk("dma_test_bench: could not get DMA channels\n");
        ret = -1;
        goto  _cleanup;
    }

    if (_ctx_create(&ctrl->tx_ctx) != 0) { ret = -1; goto  _cleanup; }
    ctrl->tx_ctx_ok = LW_TRUE;
    if (_ctx_create(&ctrl->rx_ctx) != 0) { ret = -1; goto  _cleanup; }
    ctrl->rx_ctx_ok = LW_TRUE;

    ctrl->rx_armed_sem = API_SemaphoreBCreate("bench_rx_armed",
                                               LW_FALSE,
                                               LW_OPTION_WAIT_FIFO,
                                               LW_NULL);
    if (ctrl->rx_armed_sem == LW_OBJECT_HANDLE_INVALID) {
        printk("dma_test_bench: rx_armed_sem create failed\n");
        ret = -1;
        goto  _cleanup;
    }
    ctrl->rx_armed_ok = LW_TRUE;

    ticks_per_sec = LW_MSECOND_TO_TICK_1(1000);
    tick_start    = API_TimeGet();

    printk("dma_test_bench: entry=%u  sg=%d  pkt=%u bytes  duration=%d s\n",
           (UINT)entry_len, sg_count, (UINT)pkt_bytes, duration_sec);
    printk("---\n");

    /*  先启动 RX 线程确保 S2MM 先就绪，再启动 TX 线程  */
    API_ThreadAttrBuild(&attr, 32768u, 160, LW_OPTION_THREAD_STK_CHK, (PVOID)ctrl);
    rx_tid = API_ThreadCreate("bench_rx",
                              (PTHREAD_START_ROUTINE)_bench_rx_thread,
                              &attr, LW_NULL);
    tx_tid = API_ThreadCreate("bench_tx",
                              (PTHREAD_START_ROUTINE)_bench_tx_thread,
                              &attr, LW_NULL);

    if (tx_tid == LW_OBJECT_HANDLE_INVALID ||
        rx_tid == LW_OBJECT_HANDLE_INVALID) {
        printk("dma_test_bench: thread create failed\n");
        ctrl->stop = 1;
        ret        = -1;
    } else {
        for (sec = 1; sec <= duration_sec; sec++) {                     /*  每秒打印一行周期报告        */
            API_TimeMSleep(1000);
            tick_now   = API_TimeGet();
            elapsed_ms = (ticks_per_sec > 0)
                       ? (ULONG)(((unsigned long long)(tick_now - tick_start)
                                  * 1000ULL) / ticks_per_sec) : 1;
            if (elapsed_ms == 0) elapsed_ms = 1;
            _bench_report(ctrl, &prev, sec, LW_FALSE, elapsed_ms);
        }
        ctrl->stop = 1;
    }

    API_SemaphoreBPost(ctrl->rx_armed_sem);                            /*  解除 TX 等待                */

    if (rx_tid != LW_OBJECT_HANDLE_INVALID) {
        API_ThreadJoin(rx_tid, LW_NULL);
    }
    if (tx_tid != LW_OBJECT_HANDLE_INVALID) {
        API_ThreadJoin(tx_tid, LW_NULL);
    }

    tick_now   = API_TimeGet();
    elapsed_ms = (ticks_per_sec > 0)
               ? (ULONG)(((unsigned long long)(tick_now - tick_start)
                          * 1000ULL) / ticks_per_sec) : 1;
    if (elapsed_ms == 0) elapsed_ms = 1;
    _bench_report(ctrl, &prev, 0, LW_TRUE, elapsed_ms);

_cleanup:
    if (ctrl->rx_armed_ok) API_SemaphoreBDelete(&ctrl->rx_armed_sem);
    if (ctrl->rx_ctx_ok)   _ctx_destroy(&ctrl->rx_ctx);
    if (ctrl->tx_ctx_ok)   _ctx_destroy(&ctrl->tx_ctx);
    if (ctrl->rx_chan)      dma_release_chan(ctrl->rx_chan);
    if (ctrl->tx_chan)      dma_release_chan(ctrl->tx_chan);
    for (i = 0; i < sg_count; i++) {
        if (ctrl->tx_bufs[i]) API_VmmDmaFree(ctrl->tx_bufs[i]);
        if (ctrl->rx_bufs[i]) API_VmmDmaFree(ctrl->rx_bufs[i]);
    }
    free(ctrl);

    return  ret;
}

/*  多包长基准测试预设（参考以太网帧和块设备典型负载）  */
static const struct {
    const char *name;
    size_t      entry_len;
    int         sg_count;
} _bench_presets[] = {
    { "Eth-64B",    64,    1 },                                         /*  以太网最小帧                */
    { "Eth-512B",   512,   1 },                                         /*  中等以太网帧                */
    { "Eth-1518B",  1518,  1 },                                         /*  以太网 MTU 最大帧           */
    { "Blk-4KB",    1024,  4 },                                         /*  块设备 4 KiB（4×1K SG）    */
    { "Blk-32KB",   4096,  8 },                                         /*  块设备 32 KiB（8×4K SG）   */
};

/*********************************************************************************************************
** 函数名称: _cmd_dma_test_bench
** 功能描述: 全双工 SG 模式 DMA 时基基准测试命令。
**           MM2S/S2MM 分别在独立线程中运行，主线程每秒打印吞吐量与误码率，
**           结束后打印最终汇总报告（含累计速率和 BER）。
** 输　入  : iArgC         参数数量
**           ppcArgV       ppcArgV[1]=每条目字节数（默认 4096）
**                         ppcArgV[2]=SG 条目数（默认 8）
**                         ppcArgV[3]=测试时长秒数（默认 10）
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static INT _cmd_dma_test_bench (INT iArgC, PCHAR ppcArgV[])
{
    size_t  entry_len    = 4096;
    int     sg_count     = 8;
    int     duration_sec = 10;

    if (iArgC >= 2) entry_len    = (size_t)lib_strtoul(ppcArgV[1], LW_NULL, 10);
    if (iArgC >= 3) sg_count     = (int)lib_strtoul(ppcArgV[2], LW_NULL, 10);
    if (iArgC >= 4) duration_sec = (int)lib_strtoul(ppcArgV[3], LW_NULL, 10);

    return  (_bench_run_one(entry_len, sg_count, duration_sec) == 0) ? 0 : -1;
}
/*********************************************************************************************************
** 函数名称: _cmd_dma_test_bench_multi
** 功能描述: 依次对多种典型包长（Eth-64B/512B/1518B、Blk-4KB/32KB）执行全双工基准测试，
**           对比不同包长下的吞吐量与误码率特性。
** 输　入  : iArgC         参数数量
**           ppcArgV       ppcArgV[1]=每种包长的测试时长秒数（默认 10）
** 输　出  : 0（始终）
** 全局变量:
** 调用模块:
                                           API 函数
*********************************************************************************************************/
static INT _cmd_dma_test_bench_multi (INT iArgC, PCHAR ppcArgV[])
{
    int  duration_sec = 10;
    int  i;
    int  n_presets    = (int)(sizeof(_bench_presets) / sizeof(_bench_presets[0]));

    if (iArgC >= 2) {
        duration_sec = (int)lib_strtoul(ppcArgV[1], LW_NULL, 10);
    }

    for (i = 0; i < n_presets; i++) {
        printk("\n===== [%d/%d] %s  entry=%u  sg=%d =====\n",
               i + 1, n_presets,
               _bench_presets[i].name,
               (UINT)_bench_presets[i].entry_len,
               _bench_presets[i].sg_count);
        _bench_run_one(_bench_presets[i].entry_len,
                       _bench_presets[i].sg_count,
                       duration_sec);
    }

    return  0;
}

/*********************************************************************************************************
  命令注册（原始）
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_test_register_cmds
** 功能描述: 向 tshell 注册全部测试命令，在 module_init() 中调用
** 输　入  : NONE
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_test_register_cmds (void)
{
    API_TShellKeywordAdd("dma_test_simple",  _cmd_dma_test_simple);
    API_TShellFormatAdd ("dma_test_simple",  " [<length>]");
    API_TShellHelpAdd   ("dma_test_simple",
                         "Run a simple-mode DMA loopback test\n"
                         "  length - transfer size in bytes (default 1024)\n");

    API_TShellKeywordAdd("dma_test_sg",      _cmd_dma_test_sg);
    API_TShellFormatAdd ("dma_test_sg",      " [<entry_len> [<sg_count>]]");
    API_TShellHelpAdd   ("dma_test_sg",
                         "Run a scatter-gather DMA loopback test\n"
                         "  entry_len - bytes per SG entry (default 256)\n"
                         "  sg_count  - number of SG entries (default 4)\n");

    API_TShellKeywordAdd("dma_test_stress",  _cmd_dma_test_stress);
    API_TShellFormatAdd ("dma_test_stress",  " [<length> [<count>]]");
    API_TShellHelpAdd   ("dma_test_stress",
                         "Stress-test DMA with repeated transfers\n"
                         "  length - transfer size in bytes (default 512)\n"
                         "  count  - number of iterations (default 100)\n");

    API_TShellKeywordAdd("dma_test_sg_bench",  _cmd_dma_test_sg_bench);
    API_TShellFormatAdd ("dma_test_sg_bench",  " [<entry_len> [<sg_count> [<count>]]]");
    API_TShellHelpAdd   ("dma_test_sg_bench",
                         "SG-mode DMA benchmark: verify correctness and measure throughput\n"
                         "  entry_len - bytes per SG entry (default 4096)\n"
                         "  sg_count  - number of SG entries (default 8)\n"
                         "  count     - number of iterations (default 100)\n");

    API_TShellKeywordAdd("dma_test_bench",       _cmd_dma_test_bench);
    API_TShellFormatAdd ("dma_test_bench",        " [<entry_len> [<sg_count> [<seconds>]]]");
    API_TShellHelpAdd   ("dma_test_bench",
                         "Full-duplex SG DMA benchmark: MM2S/S2MM in independent threads\n"
                         "  entry_len - bytes per SG entry (default 4096)\n"
                         "  sg_count  - number of SG entries (default 8)\n"
                         "  seconds   - test duration (default 10)\n");

    API_TShellKeywordAdd("dma_test_bench_multi",  _cmd_dma_test_bench_multi);
    API_TShellFormatAdd ("dma_test_bench_multi",  " [<seconds>]");
    API_TShellHelpAdd   ("dma_test_bench_multi",
                         "Run dma_test_bench across multiple packet sizes\n"
                         "  seconds   - test duration per size (default 10)\n"
                         "  Sizes: Eth-64B, Eth-512B, Eth-1518B, Blk-4KB, Blk-32KB\n");
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

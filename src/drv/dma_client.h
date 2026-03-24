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
** 文   件   名: dma_client.h
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA 引擎客户端 API（对标 Linux dmaengine 客户端接口）
**               面向设备驱动和应用程序，不直接操作任何硬件寄存器。
**
**               典型调用流程：
**                 chan = dma_request_chan("axi_dma0", DMA_DIR_MM2S);
**
**                 desc = dmaengine_prep_slave_sg(chan, sgl, nents, DMA_PREP_INTERRUPT);
**                 desc->callback       = my_done_cb;
**                 desc->callback_param = my_arg;
**
**                 cookie = dmaengine_submit(desc);
**                 dma_async_issue_pending(chan);
**
**                 // 等待回调触发...
**                 status = dma_async_is_tx_complete(chan, cookie);
**
**                 dma_release_channel(chan);
**
** 修改记录:
** 2026.03.24  对齐 Linux dmaengine API：重命名所有接口，引入 cookie 机制，
**             删除适配层（原 dma_adapter），参数校验在本层完成。
**
*********************************************************************************************************/

#ifndef DMA_CLIENT_H_
#define DMA_CLIENT_H_

#include "dma_types.h"

/*********************************************************************************************************
  通道管理
*********************************************************************************************************/

/*
 * dma_request_chan - 按名称和方向申请通道
 * direction: DMA_DIR_MM2S / DMA_DIR_S2MM / DMA_DIR_MEM2MEM / DMA_DIR_ANY
 * 失败返回 NULL。
 */
struct dma_chan *dma_request_chan(const char *name, int direction);

/*
 * dma_request_mcdma_chan - MCDMA 专用：按名称、方向和 TDEST 精确申请通道
 */
struct dma_chan *dma_request_mcdma_chan(const char *name, int direction, int tdest);

/*
 * dma_release_channel - 释放通道
 * 终止所有传输，触发 DMA_ERROR 回调，归还通道至空闲池。
 */
void dma_release_channel(struct dma_chan *chan);

/*********************************************************************************************************
  传输准备（对标 Linux dmaengine_prep_* 系列）

  调用方在拿到 desc 后，通过以下字段设置回调（无需在参数中传递）：
    desc->callback       = my_cb;
    desc->callback_param = my_arg;

  然后调用 dmaengine_submit() 提交，再调用 dma_async_issue_pending() 触发。
  desc 在完成/错误后由框架自动释放，调用方不得在回调返回后继续使用该指针。
*********************************************************************************************************/

/*
 * dmaengine_prep_slave_sg - 流式 SG 传输（AXI DMA / MCDMA）
 * nents=1 时等价于原 prep_simple（单段连续缓冲区）。
 * AXI DMA MM2S：sgl[0].buf 为源地址；S2MM：sgl[0].buf 为目标地址。
 */
struct dma_desc *dmaengine_prep_slave_sg(
    struct dma_chan *chan,
    struct dma_sg *sgl, int nents,
    unsigned long flags);

/*
 * dmaengine_prep_dma_memcpy - 内存到内存拷贝（CDMA 专用）
 * dst / src 为虚拟地址，驱动内部完成物理地址转换。
 */
struct dma_desc *dmaengine_prep_dma_memcpy(
    struct dma_chan *chan,
    void *dst, const void *src, size_t len,
    unsigned long flags);

/*
 * dmaengine_prep_interleaved_dma - 2D 帧传输（VDMA 专用）
 * cfg->frm_addrs[] 为帧缓冲区虚拟地址数组，驱动内部完成物理地址转换。
 */
struct dma_desc *dmaengine_prep_interleaved_dma(
    struct dma_chan *chan,
    const struct dma_vdma_config *cfg,
    unsigned long flags);

/*********************************************************************************************************
  提交与触发
*********************************************************************************************************/

/*
 * dmaengine_submit - 提交描述符，返回 cookie
 * 传输不会立即启动，须调用 dma_async_issue_pending()。
 * 连续 submit 多个再统一 issue 可实现流水线填充。
 */
dma_cookie_t dmaengine_submit(struct dma_desc *desc);

/*
 * dma_async_issue_pending - 触发通道上第一个 pending 传输
 * 后续传输在前一个完成时自动调度，无需再次调用本函数。
 */
void dma_async_issue_pending(struct dma_chan *chan);

/*********************************************************************************************************
  状态查询与控制
*********************************************************************************************************/

/*
 * dma_async_is_tx_complete - 查询 cookie 对应传输的状态
 * 返回 DMA_COMPLETE / DMA_IN_PROGRESS / DMA_ERROR。
 * 通常在回调触发（信号量 pend 返回）后调用以确认成功/失败。
 */
enum dma_status dma_async_is_tx_complete(struct dma_chan *chan, dma_cookie_t cookie);

/*
 * dmaengine_terminate_sync - 立即停止通道并等待当前传输结束
 * 排空所有 pending 和 active 传输，对每个触发 DMA_ERROR 回调。
 */
int dmaengine_terminate_sync(struct dma_chan *chan);

#endif                                                                  /*  DMA_CLIENT_H_               */
/*********************************************************************************************************
  END
*********************************************************************************************************/

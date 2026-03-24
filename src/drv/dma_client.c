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
** 文   件   名: dma_client.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA 引擎客户端 API 实现（对标 Linux dmaengine 客户端接口）
**               不直接操作任何硬件寄存器。参数校验在本层完成（原适配层职责合并）。
**
** 修改记录:
** 2026.03.24  对齐 Linux dmaengine：接口重命名，引入 cookie，删除 dma_adapter 依赖，
**             参数校验直接在本层执行，增加 MCDMA 专用通道申请接口。
**
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include "dma_types.h"
#include "dma_client.h"
#include "dma_core.h"

/*********************************************************************************************************
** 函数名称: dma_request_chan
** 功能描述: 按名称和方向申请通道。
**           找到设备并调用 device_alloc_chan_resources 初始化硬件通道。
** 输　入  : name          设备名（与 probe 时 dev->name 一致）
**           direction     DMA_DIR_* 或 DMA_DIR_ANY
** 输　出  : 成功返回通道指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_chan *dma_request_chan (const char *name, int direction)
{
    struct dma_device *dev;
    struct dma_chan   *chan;

    if (!name) {
        return  (LW_NULL);
    }

    dev = dma_find_device(name);
    if (!dev) {
        printk("dma_client: device '%s' not found\n", name);
        return  (LW_NULL);
    }

    chan = dma_core_alloc_chan_dir(dev, direction);
    if (!chan) {
        printk("dma_client: no free channel (dir=%d) on '%s'\n", direction, name);
        return  (LW_NULL);
    }

    if (chan->dev->ops && chan->dev->ops->device_alloc_chan_resources) {
        if (chan->dev->ops->device_alloc_chan_resources(chan) != 0) {
            dma_core_free_chan(chan);
            return  (LW_NULL);
        }
    }

    return  (chan);
}
/*********************************************************************************************************
** 函数名称: dma_request_mcdma_chan
** 功能描述: MCDMA 专用通道申请：按名称、方向和 TDEST 精确申请。
**           通道的 priv->tdest 由 mcdma_probe 预设，本函数匹配对应通道。
** 输　入  : name          设备名
**           direction     DMA_DIR_MM2S 或 DMA_DIR_S2MM
**           tdest         目标 TDEST（0 ~ 31）
** 输　出  : 成功返回通道指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_chan *dma_request_mcdma_chan (const char *name, int direction, int tdest)
{
    struct dma_device *dev;
    struct dma_chan   *chan;
    int                i;
    INTREG             ireg;

    if (!name) {
        return  (LW_NULL);
    }

    dev = dma_find_device(name);
    if (!dev || dev->ip_type != DMA_IP_MCDMA) {
        printk("dma_client: MCDMA device '%s' not found\n", name);
        return  (LW_NULL);
    }

    for (i = 0; i < dev->num_channels; i++) {
        chan = &dev->channels[i];

        LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
        if (!chan->in_use && chan->direction == direction) {
            /*  检查 priv->tdest（XDMA_CHAN 结构体偏移 0 为 tdest，见 xilinx_dma.h）  */
            int *p_tdest = (int *)chan->priv;                           /*  XDMA_CHAN 首字段为 tdest    */
            if (p_tdest && *p_tdest == tdest) {
                chan->in_use          = LW_TRUE;
                chan->pending_q       = LW_NULL;
                chan->active_q        = LW_NULL;
                chan->cookie          = DMA_MIN_COOKIE;
                chan->completed_cookie = 0;
                chan->last_status     = DMA_COMPLETE;
                LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

                if (chan->dev->ops && chan->dev->ops->device_alloc_chan_resources) {
                    if (chan->dev->ops->device_alloc_chan_resources(chan) != 0) {
                        chan->in_use = LW_FALSE;
                        return  (LW_NULL);
                    }
                }
                return  (chan);
            }
        }
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
    }

    printk("dma_client: MCDMA channel (dir=%d tdest=%d) not found on '%s'\n",
           direction, tdest, name);
    return  (LW_NULL);
}
/*********************************************************************************************************
** 函数名称: dma_release_channel
** 功能描述: 释放通道：终止所有传输（触发 DMA_ERROR 回调），归还通道到空闲池。
** 输　入  : chan          DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_release_channel (struct dma_chan *chan)
{
    if (!chan) {
        return;
    }

    if (chan->dev->ops && chan->dev->ops->device_free_chan_resources) {
        chan->dev->ops->device_free_chan_resources(chan);
    }

    dma_core_free_chan(chan);
}

/*********************************************************************************************************
  传输准备
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dmaengine_prep_slave_sg
** 功能描述: 流式 SG 传输准备（AXI DMA / MCDMA）。
**           nents=1 等价于单缓冲区简单模式。
**           调用方在返回的 desc 上设置 callback / callback_param，再调用 dmaengine_submit()。
** 输　入  : chan          目标通道
**           sgl           SG 列表（虚拟地址）
**           nents         条目数（≥1）
**           flags         DMA_PREP_INTERRUPT 等
** 输　出  : 成功返回 dma_desc 指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_desc *dmaengine_prep_slave_sg (struct dma_chan *chan,
                                          struct dma_sg *sgl, int nents,
                                          unsigned long flags)
{
    int i;

    if (!chan || !sgl || nents <= 0) {
        return  (LW_NULL);
    }

    for (i = 0; i < nents; i++) {                                      /*  参数校验（原适配层职责）    */
        if (!sgl[i].buf || !sgl[i].len) {
            printk("dma_client: prep_slave_sg invalid SG entry [%d]\n", i);
            return  (LW_NULL);
        }
    }

    if (!chan->dev || !chan->dev->ops || !chan->dev->ops->device_prep_slave_sg) {
        printk("dma_client: device_prep_slave_sg not implemented\n");
        return  (LW_NULL);
    }

    return  chan->dev->ops->device_prep_slave_sg(chan, sgl, nents, flags);
}
/*********************************************************************************************************
** 函数名称: dmaengine_prep_dma_memcpy
** 功能描述: 内存到内存拷贝准备（CDMA 专用）。
** 输　入  : chan          目标 CDMA 通道（direction == DMA_DIR_MEM2MEM）
**           dst / src     目标/源虚拟地址
**           len           字节数
**           flags         DMA_PREP_INTERRUPT 等
** 输　出  : 成功返回 dma_desc 指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_desc *dmaengine_prep_dma_memcpy (struct dma_chan *chan,
                                            void *dst, const void *src, size_t len,
                                            unsigned long flags)
{
    if (!chan || !dst || !src || !len) {
        return  (LW_NULL);
    }

    if (!chan->dev || !chan->dev->ops || !chan->dev->ops->device_prep_dma_memcpy) {
        printk("dma_client: device_prep_dma_memcpy not implemented\n");
        return  (LW_NULL);
    }

    return  chan->dev->ops->device_prep_dma_memcpy(chan, dst, (void *)src, len, flags);
}
/*********************************************************************************************************
** 函数名称: dmaengine_prep_interleaved_dma
** 功能描述: 2D 帧传输准备（VDMA 专用）。
** 输　入  : chan          目标 VDMA 通道
**           cfg           2D 传输配置（vsize/hsize/stride/帧地址等）
**           flags         DMA_PREP_INTERRUPT 等
** 输　出  : 成功返回 dma_desc 指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_desc *dmaengine_prep_interleaved_dma (struct dma_chan *chan,
                                                  const struct dma_vdma_config *cfg,
                                                  unsigned long flags)
{
    if (!chan || !cfg || !cfg->vsize || !cfg->hsize || cfg->frm_cnt <= 0) {
        return  (LW_NULL);
    }

    if (!chan->dev || !chan->dev->ops || !chan->dev->ops->device_prep_interleaved_dma) {
        printk("dma_client: device_prep_interleaved_dma not implemented\n");
        return  (LW_NULL);
    }

    return  chan->dev->ops->device_prep_interleaved_dma(chan, cfg, flags);
}

/*********************************************************************************************************
  提交与触发
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dmaengine_submit
** 功能描述: 为描述符分配 cookie 并加入 pending 队列，返回 cookie 供状态查询。
**           传输不会立即启动，须后续调用 dma_async_issue_pending()。
** 输　入  : desc          已由 dmaengine_prep_* 准备的描述符
** 输　出  : ≥1 有效 cookie；≤0 表示失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
dma_cookie_t dmaengine_submit (struct dma_desc *desc)
{
    struct dma_chan *chan;
    dma_cookie_t    cookie;
    INTREG          ireg;

    if (!desc || !desc->chan) {
        return  (-1);
    }
    chan = desc->chan;

    LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
    cookie = chan->cookie;
    if (++chan->cookie < DMA_MIN_COOKIE) {                              /*  防止溢出回绕到 0            */
        chan->cookie = DMA_MIN_COOKIE;
    }
    desc->cookie = cookie;
    LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

    dma_core_submit(desc);

    return  (cookie);
}
/*********************************************************************************************************
** 函数名称: dma_async_issue_pending
** 功能描述: 触发通道上第一个 pending 传输。后续传输在前一个完成时自动调度。
** 输　入  : chan          DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_async_issue_pending (struct dma_chan *chan)
{
    if (!chan) {
        return;
    }

    dma_core_issue_pending(chan);
}

/*********************************************************************************************************
  状态查询与控制
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_async_is_tx_complete
** 功能描述: 查询 cookie 对应传输的状态。
**           cookie <= completed_cookie：返回 last_status（COMPLETE 或 ERROR）；
**           否则返回 DMA_IN_PROGRESS。
**           若 ops->device_tx_status 存在，则通过硬件寄存器获取更精确状态。
** 输　入  : chan          通道指针
**           cookie        dmaengine_submit() 返回的 cookie
** 输　出  : DMA_COMPLETE / DMA_IN_PROGRESS / DMA_ERROR
** 全局变量:
** 调用模块:
*********************************************************************************************************/
enum dma_status dma_async_is_tx_complete (struct dma_chan *chan, dma_cookie_t cookie)
{
    INTREG          ireg;
    dma_cookie_t    completed;
    enum dma_status last;

    if (!chan || cookie <= 0) {
        return  (DMA_ERROR);
    }

    if (chan->dev && chan->dev->ops && chan->dev->ops->device_tx_status) {
        return  chan->dev->ops->device_tx_status(chan, cookie);
    }

    LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
    completed = chan->completed_cookie;
    last      = chan->last_status;
    LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

    if (cookie <= completed) {
        return  (last);
    }

    return  (DMA_IN_PROGRESS);
}
/*********************************************************************************************************
** 函数名称: dmaengine_terminate_sync
** 功能描述: 立即停止通道，排空所有 pending/active 传输，对每个触发回调后释放资源。
** 输　入  : chan          DMA 通道指针
** 输　出  : 0 成功；-1 参数无效
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dmaengine_terminate_sync (struct dma_chan *chan)
{
    if (!chan) {
        return  (-1);
    }

    dma_core_handle_error(chan);

    return  (0);
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

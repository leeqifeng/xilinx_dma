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
** 创   建   人: AutoGen
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA 客户端 API 实现
**               面向设备驱动和应用程序的顶层接口。每个函数完成以下工作：
**               1. 通过核心层定位通道/设备
**               2. 分配描述符并通过适配层填充硬件信息
**               3. 将提交和调度请求转发至核心层
**               本文件不直接操作任何硬件寄存器。
**
** BUG
** 2026.03.23  初始版本。
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include "dma_types.h"
#include "dma_client.h"
#include "dma_core.h"
#include "dma_adapter.h"

/*********************************************************************************************************
** 函数名称: dma_request_chan
** 功能描述: 按设备名称查找控制器，在其上分配第一个空闲通道并初始化硬件。
** 输　入  : name          设备名称（须与 axi_dma_probe 中 dev->name 一致）
** 输　出  : 成功返回通道指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_chan *dma_request_chan (const char *name)
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

    chan = dma_core_alloc_chan(dev);
    if (!chan) {
        printk("dma_client: no free channel on '%s'\n", name);
        return  (LW_NULL);
    }

    if (chan->dev->ops && chan->dev->ops->chan_init) {                   /*  通道硬件初始化              */
        if (chan->dev->ops->chan_init(chan) != 0) {
            dma_core_free_chan(chan);
            return  (LW_NULL);
        }
    }

    return  (chan);
}
/*********************************************************************************************************
** 函数名称: dma_release_chan
** 功能描述: 终止所有在途传输（发出错误回调），归还通道到空闲池
** 输　入  : chan          待释放的通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_release_chan (struct dma_chan *chan)
{
    if (!chan) {
        return;
    }

    dma_core_free_chan(chan);
}
/*********************************************************************************************************
** 函数名称: dma_prep_simple
** 功能描述: 为单段连续缓冲区传输准备描述符。
**           完成回调在中断上下文触发，必须轻量（如仅释放信号量），禁止阻塞。
**           回调触发后描述符由核心层自动释放，调用方不得继续使用返回的指针。
** 输　入  : chan          目标通道指针
**           dst           目标虚拟地址（S2MM 有效，MM2S 可传 NULL）
**           src           源虚拟地址（MM2S 有效，S2MM 可传 NULL）
**           len           传输字节数
**           cb            完成回调函数
**           arg           传递给回调的用户参数
** 输　出  : 成功返回描述符指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_desc *dma_prep_simple (struct dma_chan *chan,
                                  void *dst, void *src, size_t len,
                                  dma_complete_cb_t cb, void *arg)
{
    struct dma_desc *desc;

    if (!chan || !len) {
        return  (LW_NULL);
    }

    desc = dma_desc_alloc(chan);
    if (!desc) {
        return  (LW_NULL);
    }

    desc->cb     = cb;
    desc->cb_arg = arg;

    if (dma_adapter_prep_simple(desc, dst, src, len) != 0) {
        dma_desc_free(desc);
        return  (LW_NULL);
    }

    return  (desc);
}
/*********************************************************************************************************
** 函数名称: dma_prep_sg
** 功能描述: 为分散聚集传输准备描述符。
**           sgl[].buf 须指向 DMA 一致性内存（DMA zone）或已 Cache flush 的内存；
**           sgl[].len 须大于 0。
** 输　入  : chan          目标通道指针
**           sgl           分散聚集列表（虚拟地址）
**           sg_len        列表条目数
**           cb            完成回调函数
**           arg           传递给回调的用户参数
** 输　出  : 成功返回描述符指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_desc *dma_prep_sg (struct dma_chan *chan,
                               struct dma_sg *sgl, int sg_len,
                               dma_complete_cb_t cb, void *arg)
{
    struct dma_desc *desc;

    if (!chan || !sgl || sg_len <= 0) {
        return  (LW_NULL);
    }

    desc = dma_desc_alloc(chan);
    if (!desc) {
        return  (LW_NULL);
    }

    desc->cb     = cb;
    desc->cb_arg = arg;

    if (dma_adapter_prep_sg(desc, sgl, sg_len) != 0) {
        dma_desc_free(desc);
        return  (LW_NULL);
    }

    return  (desc);
}
/*********************************************************************************************************
** 函数名称: dma_submit
** 功能描述: 将已准备好的描述符加入 pending 队列。
**           传输不会立即启动，须后续调用 dma_issue_pending()。
** 输　入  : desc          已通过 dma_prep_* 准备的描述符指针
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_submit (struct dma_desc *desc)
{
    if (!desc || !desc->chan) {
        return  (-1);
    }

    return  dma_core_submit(desc);
}
/*********************************************************************************************************
** 函数名称: dma_issue_pending
** 功能描述: 启动通道上第一个 pending 描述符的硬件传输。
**           后续描述符在前一个完成中断中自动调度，无需再次调用本函数。
** 输　入  : chan          目标通道指针
** 输　出  : 0 成功；-1 参数无效
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_issue_pending (struct dma_chan *chan)
{
    if (!chan) {
        return  (-1);
    }

    dma_core_issue_pending(chan);

    return  (0);
}
/*********************************************************************************************************
** 函数名称: dma_tx_status
** 功能描述: 轮询查询描述符当前传输状态。
**           若控制器驱动提供了 tx_status ops，则通过硬件寄存器判断；
**           否则直接返回 desc->status。
**           仅在回调触发前（传输仍在进行时）调用有意义。
** 输　入  : chan          通道指针
**           desc          待查询的描述符指针（NULL 则返回 DMA_STATUS_IDLE）
** 输　出  : DMA_STATUS_* 状态码
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_tx_status (struct dma_chan *chan, struct dma_desc *desc)
{
    struct dma_ops *ops;

    if (!desc) {
        return  (DMA_STATUS_IDLE);
    }

    ops = (chan && chan->dev) ? chan->dev->ops : LW_NULL;
    if (ops && ops->tx_status) {
        return  ops->tx_status(chan, desc);
    }

    return  (desc->status);
}
/*********************************************************************************************************
** 函数名称: dma_terminate
** 功能描述: 立即停止通道，终止所有 pending/active 传输，
**           对每个在途描述符以 DMA_STATUS_ERROR 调用回调后释放资源。
** 输　入  : chan          目标通道指针
** 输　出  : 0 成功；-1 参数无效
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_terminate (struct dma_chan *chan)
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

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
** 文   件   名: dma_adapter.c
**
** 创   建   人: AutoGen
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA 平台适配层
**               在客户端 API 与硬件操作表之间插入参数校验和能力检测层，
**               屏蔽 Simple / SG 模式的接口差异，并提供统一的错误报告路径。
**
** BUG
** 2026.03.23  初始版本。
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include "dma_types.h"
#include "dma_adapter.h"
#include "dma_core.h"

/*********************************************************************************************************
** 函数名称: dma_adapter_register
** 功能描述: 将硬件操作表绑定到设备并向 DMA 核心注册。
**           设备的 name、channels、num_channels、priv 须由控制器驱动预先填充。
**           本函数供非 AXI DMA 的替代控制器驱动使用。
** 输　入  : dev           dma_device 指针
**           ops           硬件操作表指针
** 输　出  : 0 成功；-1 参数无效
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_adapter_register (struct dma_device *dev, struct dma_ops *ops)
{
    if (!dev || !ops) {
        return  (-1);
    }

    dev->ops = ops;

    return  dma_device_register(dev);
}
/*********************************************************************************************************
** 函数名称: dma_adapter_prep_simple
** 功能描述: 校验参数后，调用 ops->prep_simple 准备 Simple 模式描述符。
**           描述符须已通过 dma_desc_alloc() 分配并绑定通道。
** 输　入  : desc          已分配的描述符指针
**           dst           目标虚拟地址
**           src           源虚拟地址
**           len           传输字节数
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_adapter_prep_simple (struct dma_desc *desc, void *dst, void *src, size_t len)
{
    struct dma_ops *ops;

    if (!desc || !desc->chan || !desc->chan->dev) {
        return  (-1);
    }
    if (!dst && !src) {
        return  (-1);
    }
    if (!len) {
        return  (-1);
    }

    ops = desc->chan->dev->ops;
    if (!ops || !ops->prep_simple) {
        printk("dma_adapter: prep_simple not implemented\n");
        return  (-1);
    }

    return  ops->prep_simple(desc, dst, src, len);
}
/*********************************************************************************************************
** 函数名称: dma_adapter_prep_sg
** 功能描述: 校验参数（含各 SG 条目有效性）后，调用 ops->prep_sg 准备 SG 描述符。
** 输　入  : desc          已分配的描述符指针
**           sgl           分散聚集列表
**           sg_len        列表条目数
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_adapter_prep_sg (struct dma_desc *desc, struct dma_sg *sgl, int sg_len)
{
    struct dma_ops *ops;
    int             i;

    if (!desc || !desc->chan || !desc->chan->dev) {
        return  (-1);
    }
    if (!sgl || sg_len <= 0) {
        return  (-1);
    }

    for (i = 0; i < sg_len; i++) {                                     /*  验证每个 SG 条目            */
        if (!sgl[i].buf || !sgl[i].len) {
            printk("dma_adapter: invalid SG entry [%d]\n", i);
            return  (-1);
        }
    }

    ops = desc->chan->dev->ops;
    if (!ops || !ops->prep_sg) {
        printk("dma_adapter: prep_sg not implemented (SG mode enabled?)\n");
        return  (-1);
    }

    return  ops->prep_sg(desc, sgl, sg_len);
}
/*********************************************************************************************************
** 函数名称: dma_adapter_issue_pending
** 功能描述: 转发触发请求到 ops->issue_pending。
**           通常由 dma_core_schedule() 在将描述符移入 active_q 后调用。
** 输　入  : chan          目标通道指针
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_adapter_issue_pending (struct dma_chan *chan)
{
    struct dma_ops *ops;

    if (!chan || !chan->dev) {
        return  (-1);
    }

    ops = chan->dev->ops;
    if (!ops || !ops->issue_pending) {
        return  (-1);
    }

    return  ops->issue_pending(chan);
}
/*********************************************************************************************************
** 函数名称: dma_adapter_irq_handler
** 功能描述: 为轮询或软件触发场景提供的中断处理入口，转发至 ops->irq_handler。
**           正常中断路径下 ISR 直接调用 dma_core_complete()，本函数作为备用扩展点。
** 输　入  : chan          目标通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_adapter_irq_handler (struct dma_chan *chan)
{
    struct dma_ops *ops;

    if (!chan || !chan->dev) {
        return;
    }

    ops = chan->dev->ops;
    if (ops && ops->irq_handler) {
        ops->irq_handler(chan);
    }
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

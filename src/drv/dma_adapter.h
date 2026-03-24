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
** 文   件   名: dma_adapter.h
**
** 创   建   人: AutoGen
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA 平台适配层公共接口
**               屏蔽不同硬件操作表的差异，为客户端层提供统一的、带参数校验的入口。
*********************************************************************************************************/

#ifndef DMA_ADAPTER_H_
#define DMA_ADAPTER_H_

#include "dma_types.h"

/*********************************************************************************************************
  外部接口声明
*********************************************************************************************************/

/*
 * dma_adapter_register - 绑定硬件操作表并向核心层注册设备。
 * 控制器驱动若未在 probe 函数中自行调用 dma_device_register()，可使用本函数。
 */
int  dma_adapter_register(struct dma_device *dev, struct dma_ops *ops);

/*
 * 以下函数由客户端层（dma_client.c）调用，负责校验参数后转发至 dev->ops。
 */
int  dma_adapter_prep_simple(struct dma_desc *desc,
                             void *dst, void *src, size_t len);

int  dma_adapter_prep_sg(struct dma_desc *desc,
                         struct dma_sg *sgl, int sg_len);

int  dma_adapter_issue_pending(struct dma_chan *chan);

/*
 * dma_adapter_irq_handler - 为轮询或软件触发场景提供的中断处理入口。
 * 正常 ISR 路径下硬件 ISR 直接调用 dma_core_complete()，本函数备用。
 */
void dma_adapter_irq_handler(struct dma_chan *chan);

#endif                                                                  /*  DMA_ADAPTER_H_              */
/*********************************************************************************************************
  END
*********************************************************************************************************/

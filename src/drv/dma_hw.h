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
** 文   件   名: dma_hw.h
**
** 创   建   人: AutoGen
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: AXI DMA 控制器驱动公共接口（Xilinx PG021）
**               提供 axi_dma_probe / axi_dma_remove 及共享操作表 axi_dma_ops。
*********************************************************************************************************/

#ifndef DMA_HW_H_
#define DMA_HW_H_

#include "dma_types.h"

/*********************************************************************************************************
  AXI DMA IP 探测配置结构体

  调用 axi_dma_probe() 前填写本结构体，各字段含义如下：
    base_addr   — AXI DMA IP 的物理基地址（见 Vivado 地址编辑器）
    irq_mm2s    — MM2S 通道在 SylixOS 中的中断向量号
    irq_s2mm    — S2MM 通道在 SylixOS 中的中断向量号
    has_sg      — LW_TRUE：IP 使能了分散聚集功能
    has_mm2s    — LW_TRUE：IP 包含 MM2S 通道
    has_s2mm    — LW_TRUE：IP 包含 S2MM 通道
    mode        — DMA_MODE_SIMPLE 或 DMA_MODE_SG（须与 IP 构建选项一致）
    max_sg_len  — 单个 SG 描述符最大 BD 数量（仅 SG 模式有效）
*********************************************************************************************************/

struct axi_dma_config {
    addr_t  base_addr;                                                  /*  IP 物理基地址               */
    ULONG   irq_mm2s;                                                   /*  MM2S 中断向量号             */
    ULONG   irq_s2mm;                                                   /*  S2MM 中断向量号             */
    BOOL    has_sg;                                                     /*  是否包含 SG 功能            */
    BOOL    has_mm2s;                                                   /*  是否包含 MM2S 通道          */
    BOOL    has_s2mm;                                                   /*  是否包含 S2MM 通道          */
    int     mode;                                                       /*  传输模式                    */
    int     max_sg_len;                                                 /*  最大 BD 数量                */
};

/*********************************************************************************************************
  外部接口声明
*********************************************************************************************************/

/*
 * axi_dma_probe - 初始化一个 AXI DMA 控制器实例
 *
 * dev->name 必须在调用前设置。成功后 dev->channels、dev->num_channels、
 * dev->ops 和 dev->priv 均被填充，且设备自动注册到 DMA 核心。
 * 返回 0 表示成功，负值表示失败。
 */
int  axi_dma_probe(struct dma_device *dev, const struct axi_dma_config *cfg);

/*
 * axi_dma_remove - 卸载一个 AXI DMA 控制器实例
 *
 * 停止所有通道、断开中断连接，释放 axi_dma_probe() 分配的全部资源。
 */
void axi_dma_remove(struct dma_device *dev);

/*
 * 共享操作表，由每个被探测的设备实例引用
 */
extern struct dma_ops axi_dma_ops;

#endif                                                                  /*  DMA_HW_H_                   */
/*********************************************************************************************************
  END
*********************************************************************************************************/

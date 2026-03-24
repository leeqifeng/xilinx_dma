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
** 文   件   名: xilinx_dma.h
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 24 日
**
** 描        述: Xilinx DMA 统一硬件驱动公共接口（Xilinx vendor HW layer）
**               支持 AXI DMA (PG021)、AXI CDMA (PG034)、AXI VDMA (PG020)、
**               AXI MCDMA (PG288) 四种 IP 核，共用一张 ops 操作表，
**               内部通过 XDMA_CHAN.ip_type 和函数指针实现每 IP 分派。
**
**               重要约定：XDMA_CHAN.tdest 必须为第一个字段，以便
**               dma_request_mcdma_chan() 通过 *(int*)chan->priv 直接匹配 TDEST。
**
** 修改记录:
** 2026.03.24  初始版本，统一四种 Xilinx DMA IP 的硬件驱动。
**             删除四组独立 *_config 结构体及 *_probe/*_remove 声明，
**             改用 struct xilinx_dma_probe_config（含 ip_type 字段）+ 单一
**             xilinx_dma_probe() / xilinx_dma_remove() 入口，对齐 Linux
**             of_device_id / platform_driver.probe 设计。
**
*********************************************************************************************************/

#ifndef XILINX_DMA_H_
#define XILINX_DMA_H_

#include "../../dma_types.h"

/*********************************************************************************************************
  容量限制
*********************************************************************************************************/

#define XDMA_AXIDMA_MAX_CHANS   2                                       /*  AXI DMA 最多 2 通道         */
#define XDMA_CDMA_MAX_CHANS     1                                       /*  CDMA 仅 1 通道              */
#define XDMA_VDMA_MAX_CHANS     2                                       /*  VDMA 最多 2 通道            */
#define XDMA_MCDMA_MAX_CHANS    32                                      /*  MCDMA 每方向最多 32 通道    */

/*********************************************************************************************************
  hw_desc 模式
*********************************************************************************************************/

#define XDMA_MODE_SIMPLE    0                                           /*  Simple 模式（单缓冲区）     */
#define XDMA_MODE_SG        1                                           /*  SG 模式（BD 链）            */
#define XDMA_MODE_VDMA      2                                           /*  VDMA 2D 帧传输模式          */
#define XDMA_MODE_CDMA_SG   3                                           /*  CDMA SG 模式                */

/*********************************************************************************************************
  通道硬件私有数据（存储于 dma_chan.priv）

  CRITICAL: tdest 必须为第一个字段。
  dma_request_mcdma_chan() 在 dma_client.c 中通过以下方式匹配 TDEST：
      int *p_tdest = (int *)chan->priv;
      if (*p_tdest == tdest) ...
  其他 IP 的 tdest 置 0，不影响功能。
*********************************************************************************************************/

typedef struct xdma_chan {
    int              tdest;           /*  [FIRST] MCDMA TDEST（0~31）；其他 IP 置 0   */
    int              ip_type;         /*  DMA_IP_*                                    */
    addr_t           regs;            /*  寄存器窗口虚拟基地址                        */
    ULONG            irq;             /*  中断向量号                                  */
    int              direction;       /*  DMA_DIR_*                                   */
    int              chan_id;         /*  MCDMA 每方向通道索引（0-based）             */
    struct dma_chan  *core_chan;       /*  核心层通道反向指针                          */

    /*  硬件能力标志（probe 时从硬件/配置读取，对标 Linux xilinx_dma_chan.has_sg）  */
    BOOL             has_sg;          /*  IP 支持 SG 模式（AXIDMA/CDMA：读 DMASR_SG_MASK；
                                          MCDMA：恒为 LW_TRUE；VDMA：恒为 LW_FALSE）  */
    int              num_frms;        /*  VDMA：最大帧缓冲数量（来自 vdma_config.max_frm_cnt）；
                                          其他 IP 置 0                                */

    /*  每 IP 函数指针（probe 时按 IP 类型赋值）  */
    int  (*start_transfer)(struct dma_chan *chan);   /*  启动硬件传输（写触发寄存器） */
    int  (*stop_transfer )(struct dma_chan *chan);   /*  停止通道（清除 RS 位）       */
    int  (*alloc_resources)(struct dma_chan *chan);  /*  通道初始化（复位+使能中断）  */
    void (*free_resources )(struct dma_chan *chan);  /*  通道清理（停止+复位）        */
} XDMA_CHAN;

/*********************************************************************************************************
  per-IP 静态配置表（对标 Linux struct xilinx_dma_config）
  驱动内部定义静态实例（_axidma_hw_config 等），由 xilinx_dma_chan_probe 通过
  xdev->dma_config->ip_type 和 irq_handler 分派通道函数指针和 ISR。
*********************************************************************************************************/

struct xilinx_dma_config {
    int               ip_type;        /*  DMA_IP_*                                    */
    int               max_channels;   /*  该 IP 最大通道数（对标 Linux max_channels） */
    PINT_SVR_ROUTINE  irq_handler;    /*  统一 ISR（pvArg = XDMA_CHAN*）              */
    CPCHAR            irq_name;       /*  向量注册名称                                */
};

/*********************************************************************************************************
  设备硬件私有数据（存储于 dma_device.priv）
*********************************************************************************************************/

typedef struct xdma_device {
    addr_t                          regs_virt;   /*  寄存器区虚拟基地址              */
    int                             ip_type;     /*  DMA_IP_*                        */
    int                             n_chans;     /*  已初始化的通道总数              */
    XDMA_CHAN                      *hw_chans;    /*  通道数组（动态分配）            */
    const struct xilinx_dma_config *dma_config;  /*  per-IP 静态配置表指针           */
} XDMA_DEVICE;

/*********************************************************************************************************
  硬件描述符（存储于 dma_desc.hw_desc）
  由 device_prep_* 分配，由 desc->free_hw_desc 释放（通过 _xdesc_free_hw 钩子）。
*********************************************************************************************************/

typedef struct xdma_desc {
    int     mode;                     /*  XDMA_MODE_*                                 */
    int     ip_type;                  /*  DMA_IP_*                                    */

    /*  Simple 模式 / CDMA memcpy  */
    UINT32  simple_src;               /*  源物理地址                                  */
    UINT32  simple_dst;               /*  目标物理地址                                */
    UINT32  simple_len;               /*  传输字节数                                  */

    /*  SG / MCDMA / CDMA_SG  */
    void   *bd_virt;                  /*  BD 数组虚拟地址（DMA zone, 物理==虚拟）     */
    UINT32  bd_phys;                  /*  BD 数组物理地址                             */
    int     bd_num;                   /*  BD 数量                                     */

    /*  VDMA 2D 帧传输  */
    struct dma_vdma_config  vdma_cfg; /*  caller 配置副本                             */
    UINT32  frm_phys[32];             /*  各帧缓冲区物理地址（已转换）                */
} XDMA_DESC;

/*********************************************************************************************************
  统一探测配置（对标 Linux of_device_id / xilinx_dma_probe 入参）
  ip_type 字段对应 Linux 中通过 of_match_table compatible 字符串选出的 per-IP 配置；
  其余字段对应各 IP DT 属性，无关字段置 0 / LW_FALSE 即可。

  AXI DMA (DMA_IP_AXIDMA)：has_mm2s, has_s2mm, irq_mm2s, irq_s2mm, max_sg_len
  AXI CDMA (DMA_IP_CDMA) ：irq, has_sg
  AXI VDMA (DMA_IP_VDMA) ：has_mm2s, has_s2mm, irq_mm2s, irq_s2mm, max_frm_cnt
  AXI MCDMA(DMA_IP_MCDMA)：n_mm2s, n_s2mm, irqs_mm2s[], irqs_s2mm[]
*********************************************************************************************************/

struct xilinx_dma_probe_config {
    int     ip_type;                                    /*  DMA_IP_*（必填）          */
    addr_t  base_addr;                                  /*  IP 物理基地址（必填）     */

    /*  AXI DMA / VDMA 双向配置  */
    BOOL    has_mm2s;                                   /*  是否包含 MM2S 通道        */
    BOOL    has_s2mm;                                   /*  是否包含 S2MM 通道        */
    ULONG   irq_mm2s;                                   /*  MM2S 中断向量号           */
    ULONG   irq_s2mm;                                   /*  S2MM 中断向量号           */

    /*  AXI DMA 专用  */
    int     max_sg_len;                                 /*  每描述符最大 BD 数        */

    /*  AXI VDMA 专用  */
    int     max_frm_cnt;                                /*  最大帧缓冲数量（1~32）    */

    /*  AXI CDMA 专用  */
    ULONG   irq;                                        /*  CDMA 单通道中断向量号     */
    BOOL    has_sg;                                     /*  CDMA：LW_TRUE=SG 模式     */

    /*  AXI MCDMA 专用  */
    int     n_mm2s;                                     /*  MM2S 通道数（0~32）       */
    int     n_s2mm;                                     /*  S2MM 通道数（0~32）       */
    ULONG   irqs_mm2s[XDMA_MCDMA_MAX_CHANS];            /*  各 MM2S 通道中断向量      */
    ULONG   irqs_s2mm[XDMA_MCDMA_MAX_CHANS];            /*  各 S2MM 通道中断向量      */
};

/*********************************************************************************************************
  共享 ops 表
  所有 IP 实例共用一张操作表，内部通过 hw->ip_type 和函数指针分派。
  需要者在 probe 后将 dev->ops = &xilinx_dma_ops。
*********************************************************************************************************/

extern struct dma_ops xilinx_dma_ops;

/*********************************************************************************************************
  probe / remove 统一接口（对标 Linux platform_driver.probe/remove）
  调用前须填充 dev->name 及 cfg->ip_type 和对应 IP 字段；
  调用后 dev->channels、num_channels、ops、priv 均被填充，
  设备自动注册到 DMA 核心层（dma_core）。
  remove 停止所有通道、断开中断、释放全部资源并注销设备。
*********************************************************************************************************/

int  xilinx_dma_probe(struct dma_device *dev, const struct xilinx_dma_probe_config *cfg);
void xilinx_dma_remove(struct dma_device *dev);

#endif                                                                  /*  XILINX_DMA_H_               */
/*********************************************************************************************************
  END
*********************************************************************************************************/

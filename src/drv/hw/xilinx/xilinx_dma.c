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
** 文   件   名: xilinx_dma.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 24 日
**
** 描        述: Xilinx DMA 统一硬件驱动实现
**               支持 AXI DMA (PG021)、AXI CDMA (PG034)、AXI VDMA (PG020)、
**               AXI MCDMA (PG288) 四种 IP 核，共用一张 ops 操作表。
**
**               架构说明：
**               - 三层内部探测层次对标 Linux xilinx_dma_probe 内部结构：
**                 xilinx_dma_probe → xilinx_dma_child_probe → xilinx_dma_chan_probe；
**               - ip_type 字段对标 Linux of_match_table compatible 字符串选出的
**                 per-IP 静态配置（_axidma_hw_config 等），内部通过函数指针分派；
**               - XDMA_CHAN.start_transfer / stop_transfer / alloc_resources / free_resources
**                 在 xilinx_dma_chan_probe 时按 ip_type 赋值；
**               - 共享 ISR 底半部 _dma_complete_bh，投递到 CPU 0 defer 队列执行；
**               - 描述符释放钩子 _xdesc_free_hw 统一处理 BD 数组和 XDMA_DESC 本体。
**
**               ISR 线程安全模型：
**               - ISR 仅读寄存器/清中断/设置 desc->status，再投递底半部任务；
**               - 底半部在 defer 线程（非 ISR 上下文）中更新 cookie/last_status 并触发回调。
**
** 修改记录:
** 2026.03.24  初始版本：统一四种 IP 的 HW 驱动，对齐 Linux dmaengine 接口风格；
**             实现三层内部探测层次（xilinx_dma_chan_probe / xilinx_dma_child_probe /
**             xilinx_dma_probe），对外仅暴露单一 xilinx_dma_probe / xilinx_dma_remove；
**             BD 结构体改用 __attribute__((aligned(64))) 确保硬件所需的 64 字节对齐；
**             has_sg 移至通道层（XDMA_CHAN），probe 时从 DMASR_SG_MASK 读取。
**
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include "../../dma_types.h"
#include "../../dma_core.h"
#include "xilinx_dma.h"

/*********************************************************************************************************
  通用寄存器控制位（AXI DMA / CDMA / VDMA 共用）
*********************************************************************************************************/

#define DMACR_RS            (1u << 0)                                   /*  Run/Stop                    */
#define DMACR_RESET         (1u << 2)                                   /*  软复位（自清零）            */
#define DMACR_IOC_IRQEN     (1u << 12)                                  /*  完成中断使能                */
#define DMACR_ERR_IRQEN     (1u << 14)                                  /*  错误中断使能                */

#define DMASR_HALTED        (1u << 0)                                   /*  通道已停止                  */
#define DMASR_IDLE          (1u << 1)                                   /*  通道空闲                    */
#define DMASR_SG_MASK       (1u << 3)                                   /*  SG 模式已启用（IP 构建决定）*/
#define DMASR_IOC_IRQ       (1u << 12)                                  /*  完成中断标志                */
#define DMASR_DLY_IRQ       (1u << 13)                                  /*  延迟中断标志                */
#define DMASR_ERR_IRQ       (1u << 14)                                  /*  错误中断标志                */
#define DMASR_DMAINTERR     (1u << 4)
#define DMASR_DMASLVERR     (1u << 5)
#define DMASR_DMADECERR     (1u << 6)
#define DMASR_SGINTERR      (1u << 8)
#define DMASR_SGSLVERR      (1u << 9)
#define DMASR_SGDECERR      (1u << 10)
#define DMASR_ERR_MASK      (DMASR_DMAINTERR | DMASR_DMASLVERR | DMASR_DMADECERR | \
                             DMASR_SGINTERR  | DMASR_SGSLVERR  | DMASR_SGDECERR)

/*********************************************************************************************************
  AXI DMA (PG021) 通道寄存器偏移（相对 IP 基地址）
  MM2S 通道：偏移 0x00~0x28；S2MM 通道：偏移 0x30~0x58
*********************************************************************************************************/

#define MM2S_DMACR          0x00u
#define MM2S_DMASR          0x04u
#define MM2S_CURDESC        0x08u
#define MM2S_CURDESC_MSB    0x0Cu
#define MM2S_TAILDESC       0x10u
#define MM2S_TAILDESC_MSB   0x14u
#define MM2S_SA             0x18u
#define MM2S_SA_MSB         0x1Cu
#define MM2S_LENGTH         0x28u

#define S2MM_DMACR          0x30u
#define S2MM_DMASR          0x34u
#define S2MM_CURDESC        0x38u
#define S2MM_CURDESC_MSB    0x3Cu
#define S2MM_TAILDESC       0x40u
#define S2MM_TAILDESC_MSB   0x44u
#define S2MM_DA             0x48u
#define S2MM_DA_MSB         0x4Cu
#define S2MM_LENGTH         0x58u

/*********************************************************************************************************
  AXI DMA BD 位域（PG021）
  BD_CTRL_SOF/EOF 标记帧起止；BD_STS_CMPLT 由硬件置位表示该 BD 已完成。
*********************************************************************************************************/

#define BD_CTRL_LEN_MASK    0x03FFFFFFu
#define BD_CTRL_EOF         (1u << 26)                                  /*  End Of Frame                */
#define BD_CTRL_SOF         (1u << 27)                                  /*  Start Of Frame              */
#define BD_STS_CMPLT        (1u << 31)                                  /*  BD 完成标志                 */
#define BD_STS_ERR_MASK     ((1u<<28)|(1u<<29)|(1u<<30))                /*  BD 错误位掩码               */

/*********************************************************************************************************
  AXI DMA Buffer Descriptor（PG021，须 64 字节对齐）
*********************************************************************************************************/

#define AXIDMA_BD_ALIGN     64u

typedef struct axidma_bd {
    UINT32  next_desc;
    UINT32  next_desc_msb;
    UINT32  buf_addr;
    UINT32  buf_addr_msb;
    UINT32  reserved1;
    UINT32  reserved2;
    UINT32  control;
    UINT32  status;
    UINT32  app[5];
    UINT32  _pad[3];
} __attribute__((aligned(64))) AXIDMA_BD;

/*********************************************************************************************************
  AXI CDMA (PG034) 寄存器偏移（相对 IP 基地址）
  Simple 模式：写 SRCADDR + DSTADDR 后写 BTT 触发传输。
  SG 模式：写 CURDESC → RS=1 → 写 TAILDESC 触发。
*********************************************************************************************************/

#define CDMA_DMACR          0x00u
#define CDMA_DMASR          0x04u
#define CDMA_CURDESC        0x08u
#define CDMA_CURDESC_MSB    0x0Cu
#define CDMA_TAILDESC       0x10u
#define CDMA_TAILDESC_MSB   0x14u
#define CDMA_SRCADDR        0x18u
#define CDMA_SRCADDR_MSB    0x1Cu
#define CDMA_DSTADDR        0x20u
#define CDMA_DSTADDR_MSB    0x24u
#define CDMA_BTT            0x28u                                       /*  写入触发 Simple 传输        */

#define CDMA_DMACR_SGMODE   (1u << 3)                                   /*  使能 SG 模式                */

#define CDMA_DMASR_CMPLT    (1u << 1)                                   /*  Simple 模式完成位           */
#define CDMA_DMASR_INTERR   (1u << 4)
#define CDMA_DMASR_SLVERR   (1u << 5)
#define CDMA_DMASR_DECERR   (1u << 6)
#define CDMA_DMASR_ERR_MASK (CDMA_DMASR_INTERR | CDMA_DMASR_SLVERR | CDMA_DMASR_DECERR)

/*********************************************************************************************************
  AXI CDMA Buffer Descriptor（PG034，须 64 字节对齐）
*********************************************************************************************************/

#define CDMA_BD_ALIGN       64u

typedef struct cdma_bd {
    UINT32  next_desc;
    UINT32  next_desc_msb;
    UINT32  src_addr;
    UINT32  src_addr_msb;
    UINT32  dst_addr;
    UINT32  dst_addr_msb;
    UINT32  control;
    UINT32  status;
    UINT32  _pad[8];
} __attribute__((aligned(64))) CDMA_BD;

#define CDMA_BD_STS_CMPLT   (1u << 31)
#define CDMA_BD_STS_ERR     ((1u<<28)|(1u<<29)|(1u<<30))

/*********************************************************************************************************
  AXI VDMA (PG020) 寄存器偏移
  控制寄存器：MM2S 基偏移 0x0000，S2MM 基偏移 0x0030（相对 IP 基地址）
  描述寄存器：MM2S 基偏移 0x0050，S2MM 基偏移 0x00A0（相对 IP 基地址）
  VDMA_REG_DMACR / VDMA_REG_DMASR 相对各自控制基；
  VDMA_REG_VSIZE / HSIZE / STRIDE / STARTADDR 相对各自描述基。
  写 VSIZE 触发帧传输。
*********************************************************************************************************/

#define VDMA_MM2S_CTRL_BASE 0x0000u
#define VDMA_S2MM_CTRL_BASE 0x0030u
#define VDMA_MM2S_DESC_BASE 0x0050u
#define VDMA_S2MM_DESC_BASE 0x00A0u

#define VDMA_REG_DMACR      0x00u                                       /*  相对控制基                  */
#define VDMA_REG_DMASR      0x04u                                       /*  相对控制基                  */
#define VDMA_REG_PARK_PTR   0x28u                                       /*  相对 IP 基（绝对偏移）      */
#define VDMA_REG_FRMSTORE   0x18u                                       /*  相对 IP 基（绝对偏移）      */

#define VDMA_REG_VSIZE      0x00u                                       /*  相对描述基，写入触发传输    */
#define VDMA_REG_HSIZE      0x04u
#define VDMA_REG_STRIDE     0x08u
#define VDMA_REG_STARTADDR(n) (0x0Cu + (UINT32)(n) * 4u)               /*  帧 n 起始地址寄存器         */

#define VDMA_DMACR_CIRC_EN      (1u << 1)                              /*  循环（非 park）模式         */
#define VDMA_DMACR_FRAMECNT_EN  (1u << 4)                              /*  帧计数中断使能              */
#define VDMA_DMACR_FRM_CNT_SHIFT 16u                                   /*  帧计数字段起始位            */

#define VDMA_DMASR_FRM_CNT_IRQ  (1u << 12)                             /*  帧计数完成中断              */
#define VDMA_DMASR_ERR_IRQ      (1u << 14)                             /*  错误中断                    */
#define VDMA_DMASR_ERR_MASK     (DMASR_DMAINTERR | DMASR_DMASLVERR | DMASR_DMADECERR)

/*********************************************************************************************************
  AXI MCDMA (PG288) 寄存器偏移
  MM2S 全局基：IP 基 + 0x0000；S2MM 全局基：IP 基 + 0x0500。
  每通道偏移（x = 0-indexed 通道号）：
    CR    = 方向基 + 0x40 + x * 0x40
    SR    = 方向基 + 0x44 + x * 0x40
    CDESC = 方向基 + 0x48 + x * 0x40
    TDESC = 方向基 + 0x50 + x * 0x40
  CHEN / CH_ERR / TXINT_SER / RXINT_SER 相对各自方向全局基。
  写 TDESC 触发传输。
*********************************************************************************************************/

#define MCDMA_MM2S_BASE     0x0000u
#define MCDMA_S2MM_BASE     0x0500u

#define MCDMA_CHEN          0x0008u
#define MCDMA_CH_ERR        0x0010u
#define MCDMA_TXINT_SER     0x0028u
#define MCDMA_RXINT_SER     0x0020u

#define MCDMA_CH_CR(x)      (0x0040u + (UINT32)(x) * 0x40u)
#define MCDMA_CH_SR(x)      (0x0044u + (UINT32)(x) * 0x40u)
#define MCDMA_CH_CDESC(x)   (0x0048u + (UINT32)(x) * 0x40u)
#define MCDMA_CH_TDESC(x)   (0x0050u + (UINT32)(x) * 0x40u)

#define MCDMA_COALESCE_SHIFT    16u                                     /*  CR[23:16]：完成阈值位偏移   */

#define MCDMA_CR_RS         (1u << 0)
#define MCDMA_CR_IRQEN_ALL  ((1u<<5)|(1u<<6)|(1u<<7))                  /*  IOC + DLY + ERR 中断使能    */
#define MCDMA_SR_CMPLT      (1u << 4)
#define MCDMA_SR_IOC_IRQ    (1u << 5)
#define MCDMA_SR_DLY_IRQ    (1u << 6)
#define MCDMA_SR_ERR_IRQ    (1u << 7)
#define MCDMA_SR_INTERR     (1u << 8)
#define MCDMA_SR_SLVERR     (1u << 9)
#define MCDMA_SR_DECERR     (1u << 10)
#define MCDMA_SR_ERR_MASK   (MCDMA_SR_INTERR | MCDMA_SR_SLVERR | MCDMA_SR_DECERR)

#define MCDMA_BD_LEN_MASK   0x00003FFFu                                 /*  BD control[13:0]：长度      */
#define MCDMA_BD_SOP        (1u << 31)                                  /*  Start Of Packet             */
#define MCDMA_BD_EOP        (1u << 30)                                  /*  End Of Packet               */
#define MCDMA_BD_TUSER_SHIFT 16u                                        /*  control[23:16]：TUSER       */

#define MCDMA_BD_STS_CMPLT  (1u << 31)
#define MCDMA_BD_STS_ERR    ((1u<<4)|(1u<<5)|(1u<<6))

/*  MCDMA BD 须 64 字节对齐  */
#define MCDMA_BD_ALIGN      64u

typedef struct mcdma_bd {
    UINT32  next_desc;
    UINT32  next_desc_msb;
    UINT32  buf_addr;
    UINT32  buf_addr_msb;
    UINT32  rsvd;
    UINT32  control;
    UINT32  status;
    UINT32  sideband_status;
    UINT32  app[5];
    UINT32  _pad[3];
} __attribute__((aligned(64))) MCDMA_BD;

/*********************************************************************************************************
  杂项常量
*********************************************************************************************************/

#define XDMA_REG_MAP_SIZE   0x1000u                                     /*  一般 IP 寄存器区大小（4KB） */
#define XDMA_RESET_POLLS    10000                                       /*  复位/停止最大轮询次数       */
#define XDMA_MCDMA_REG_SIZE 0x2000u                                     /*  MCDMA 寄存器区大小（8KB）   */

/*********************************************************************************************************
  全局变量
*********************************************************************************************************/

static PLW_JOB_QUEUE  _G_defer_q = LW_NULL;                            /*  ISR 底半部延迟队列（CPU 0） */

/*********************************************************************************************************
  寄存器读写内联函数
*********************************************************************************************************/

static LW_INLINE UINT32 _xreg_rd (addr_t base, UINT32 off)
{
    return  read32(base + off);
}

static LW_INLINE VOID _xreg_wr (addr_t base, UINT32 off, UINT32 val)
{
    write32(val, base + off);
}

/*********************************************************************************************************
** 函数名称: _xdma_active_desc
** 功能描述: 返回通道 active_q 队首描述符，仅供 ISR 路径在已知 active_q 非空时调用。
** 输　入  : chan          通道指针
** 输　出  : active_q 首描述符；队列为空返回 NULL
*********************************************************************************************************/
static struct dma_desc *_xdma_active_desc (struct dma_chan *chan)
{
    if (chan->active_q == LW_NULL) {
        return  (LW_NULL);
    }
    return  _LIST_ENTRY(chan->active_q, struct dma_desc, node);
}
/*********************************************************************************************************
** 函数名称: _xdesc_free_hw
** 功能描述: dma_desc.free_hw_desc 钩子，释放 XDMA_DESC 内的 BD 数组及 XDMA_DESC 本体。
**           由 dma_core_complete / dma_core_handle_error 在 defer 线程中调用。
** 输　入  : desc          dma_desc 指针（hw_desc 字段指向 XDMA_DESC）
** 输　出  : NONE
*********************************************************************************************************/
static VOID _xdesc_free_hw (struct dma_desc *desc)
{
    XDMA_DESC *xdesc = (XDMA_DESC *)desc->hw_desc;

    if (!xdesc) {
        return;
    }

    if (xdesc->bd_virt) {
        API_VmmDmaFree(xdesc->bd_virt);
        xdesc->bd_virt = LW_NULL;
    }

    lib_free(xdesc);
    desc->hw_desc = LW_NULL;
}
/*********************************************************************************************************
** 函数名称: _dma_complete_bh
** 功能描述: ISR 底半部（defer 线程中执行）。
**           出错时先停止通道，再调用 dma_core_complete 完成状态更新和回调触发。
** 输　入  : pvHw          XDMA_CHAN 指针（ISR 投递时传入）
**           pvChan        struct dma_chan 指针
**           pvDesc        已完成的 struct dma_desc 指针（status 已由 ISR 设置）
**           pv3~pv5       保留（API_InterDeferJobAddEx 固定 6 参数）
** 输　出  : NONE
*********************************************************************************************************/
static VOID _dma_complete_bh (PVOID pvHw, PVOID pvChan, PVOID pvDesc,
                               PVOID pv3,  PVOID pv4,   PVOID pv5)
{
    XDMA_CHAN       *hw    = (XDMA_CHAN *)pvHw;
    struct dma_chan  *chan  = (struct dma_chan *)pvChan;
    struct dma_desc  *desc  = (struct dma_desc *)pvDesc;
    enum dma_status   status;

    if (!hw || !chan || !desc) {
        return;
    }

    status = desc->status;

    if (status == DMA_ERROR && hw->stop_transfer) {                     /*  出错时先停止通道            */
        hw->stop_transfer(chan);
    }

    dma_core_complete(chan, desc, status);
}
/*********************************************************************************************************
** 函数名称: _xdma_chan_reset_generic
** 功能描述: 通用通道软复位（AXI DMA / CDMA / VDMA 共用）。
**           写 RESET 位后轮询自清零，再确认 HALTED。
** 输　入  : base          IP 寄存器虚拟基地址
**           cr_off        控制寄存器偏移
**           sr_off        状态寄存器偏移
**           label         调试标签（仅用于 printk）
** 输　出  : 0 成功；-1 超时或未 HALTED
*********************************************************************************************************/
static int _xdma_chan_reset_generic (addr_t base, UINT32 cr_off, UINT32 sr_off,
                                     const char *label)
{
    int i;

    _xreg_wr(base, cr_off, DMACR_RESET);

    for (i = 0; i < XDMA_RESET_POLLS; i++) {
        if (!(_xreg_rd(base, cr_off) & DMACR_RESET)) {
            break;
        }
    }

    if (i >= XDMA_RESET_POLLS) {
        printk("xilinx_dma: %s reset timed out\n", label ? label : "");
        return  (-1);
    }

    if (!(_xreg_rd(base, sr_off) & DMASR_HALTED)) {
        printk("xilinx_dma: %s not halted after reset\n", label ? label : "");
        return  (-1);
    }

    return  (0);
}

/*********************************************************************************************************
  AXI DMA (PG021) 通道操作
  包含通道复位/启动/停止、资源分配释放、传输启动和 ISR 处理。
  Simple 模式（nents=1）：直接写地址和长度寄存器触发。
  SG 模式（nents>1）：BD 链，CURDESC 须在 RS=0 时写入（PG021 约束）。
*********************************************************************************************************/

/*  返回指定通道的 DMACR / DMASR 寄存器偏移  */
static LW_INLINE UINT32 _axidma_cr_off (XDMA_CHAN *hw)
{
    return  (hw->direction == DMA_DIR_MM2S) ? MM2S_DMACR : S2MM_DMACR;
}

static LW_INLINE UINT32 _axidma_sr_off (XDMA_CHAN *hw)
{
    return  (hw->direction == DMA_DIR_MM2S) ? MM2S_DMASR : S2MM_DMASR;
}

/*  软复位 AXI DMA 单通道  */
static int _axidma_chan_reset (XDMA_CHAN *hw)
{
    return  _xdma_chan_reset_generic(hw->regs, _axidma_cr_off(hw), _axidma_sr_off(hw),
                                     hw->direction == DMA_DIR_MM2S ? "MM2S" : "S2MM");
}

/*  Run/Stop=1，使能 IOC+ERR 中断  */
static VOID _axidma_chan_run (XDMA_CHAN *hw)
{
    UINT32 cr_off = _axidma_cr_off(hw);
    UINT32 val    = _xreg_rd(hw->regs, cr_off);
    val |= DMACR_RS | DMACR_IOC_IRQEN | DMACR_ERR_IRQEN;
    _xreg_wr(hw->regs, cr_off, val);
}

/*  Run/Stop=0  */
static VOID _axidma_chan_stop (XDMA_CHAN *hw)
{
    UINT32 cr_off = _axidma_cr_off(hw);
    UINT32 val    = _xreg_rd(hw->regs, cr_off);
    val &= ~DMACR_RS;
    _xreg_wr(hw->regs, cr_off, val);
}

/*********************************************************************************************************
** 函数名称: _axidma_alloc_resources
** 功能描述: AXI DMA 通道资源分配（device_alloc_chan_resources）。
**           复位通道并使能 IOC + ERR 中断，通道此时处于 HALTED + RS=0 状态，
**           等待 _axidma_start_transfer 写入地址/BD 后才启动。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 复位失败
*********************************************************************************************************/
static int _axidma_alloc_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;
    int        ret;

    ret = _axidma_chan_reset(hw);
    if (ret) {
        return  (ret);
    }

    _xreg_wr(hw->regs, _axidma_cr_off(hw), DMACR_IOC_IRQEN | DMACR_ERR_IRQEN);
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axidma_free_resources
** 功能描述: AXI DMA 通道资源释放（device_free_chan_resources）。
**           停止通道后执行软复位，确保硬件恢复到 HALTED 状态。
** 输　入  : chan          核心层通道指针
** 输　出  : NONE
*********************************************************************************************************/
static VOID _axidma_free_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;
    _axidma_chan_stop(hw);
    _axidma_chan_reset(hw);
}
/*********************************************************************************************************
** 函数名称: _axidma_start_transfer
** 功能描述: AXI DMA 传输启动（start_transfer 函数指针 / device_issue_pending）。
**           Simple 模式：RS=1 → 写 SA/DA + LENGTH 触发。
**           SG 模式：RS=0 → 写 CURDESC → RS=1 → 写 TAILDESC 触发（PG021 约束）。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 描述符无效
*********************************************************************************************************/
static int _axidma_start_transfer (struct dma_chan *chan)
{
    XDMA_CHAN       *hw    = (XDMA_CHAN *)chan->priv;
    struct dma_desc *desc  = _xdma_active_desc(chan);
    XDMA_DESC       *xdesc;
    UINT32           tail_phys;
    int              n;
    UINT32           sr_off;

    if (!desc) {
        return  (0);
    }
    xdesc = (XDMA_DESC *)desc->hw_desc;
    if (!xdesc) {
        return  (-1);
    }

    if (xdesc->mode == XDMA_MODE_SIMPLE) {                             /*  Simple 模式                 */
        _axidma_chan_run(hw);

        if (hw->direction == DMA_DIR_MM2S) {
            _xreg_wr(hw->regs, MM2S_SA,     xdesc->simple_src);
            _xreg_wr(hw->regs, MM2S_SA_MSB, 0);
            _xreg_wr(hw->regs, MM2S_LENGTH, xdesc->simple_len);
        } else {
            _xreg_wr(hw->regs, S2MM_DA,     xdesc->simple_dst);
            _xreg_wr(hw->regs, S2MM_DA_MSB, 0);
            _xreg_wr(hw->regs, S2MM_LENGTH, xdesc->simple_len);
        }

    } else {                                                            /*  SG 模式                     */
        tail_phys = xdesc->bd_phys +
                    (UINT32)((xdesc->bd_num - 1) * (int)sizeof(AXIDMA_BD));
        sr_off    = _axidma_sr_off(hw);

        if (hw->direction == DMA_DIR_MM2S) {
            _axidma_chan_stop(hw);                                      /*  CURDESC 须在 RS=0 时写入    */
            for (n = XDMA_RESET_POLLS;
                 n-- > 0 && !(_xreg_rd(hw->regs, sr_off) & DMASR_HALTED); ) {}
            _xreg_wr(hw->regs, MM2S_CURDESC,     xdesc->bd_phys);
            _xreg_wr(hw->regs, MM2S_CURDESC_MSB, 0);
            _axidma_chan_run(hw);
            _xreg_wr(hw->regs, MM2S_TAILDESC,     tail_phys);
            _xreg_wr(hw->regs, MM2S_TAILDESC_MSB, 0);
        } else {
            _axidma_chan_stop(hw);
            for (n = XDMA_RESET_POLLS;
                 n-- > 0 && !(_xreg_rd(hw->regs, sr_off) & DMASR_HALTED); ) {}
            _xreg_wr(hw->regs, S2MM_CURDESC,     xdesc->bd_phys);
            _xreg_wr(hw->regs, S2MM_CURDESC_MSB, 0);
            _axidma_chan_run(hw);
            _xreg_wr(hw->regs, S2MM_TAILDESC,     tail_phys);
            _xreg_wr(hw->regs, S2MM_TAILDESC_MSB, 0);
        }
    }

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axidma_stop_transfer
** 功能描述: AXI DMA 传输停止（stop_transfer 函数指针 / device_terminate_all）。
**           清除 DMACR.RS 位令通道停止接收新传输，当前 in-flight BD 完成后通道进入 HALTED。
** 输　入  : chan          核心层通道指针
** 输　出  : 0（始终成功）
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _axidma_stop_transfer (struct dma_chan *chan)
{
    _axidma_chan_stop((XDMA_CHAN *)chan->priv);
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axidma_prep_slave_sg
** 功能描述: AXI DMA SG/Simple 传输准备（device_prep_slave_sg）。
**           nents=1 → Simple 模式（直接存物理地址）；nents>1 → SG 模式（分配 AXIDMA_BD 链）。
** 输　入  : chan          核心层通道指针
**           sgl           SG 列表（虚拟地址）
**           nents         条目数（≥1）
**           flags         DMA_PREP_* 标志
** 输　出  : 成功返回 dma_desc 指针；失败返回 NULL
*********************************************************************************************************/
static struct dma_desc *_axidma_prep_slave_sg (struct dma_chan *chan,
                                                struct dma_sg *sgl, int nents,
                                                unsigned long flags)
{
    XDMA_CHAN       *hw = (XDMA_CHAN *)chan->priv;
    struct dma_desc *desc;
    XDMA_DESC       *xdesc;
    phys_addr_t      phys = 0;

    desc = dma_desc_alloc(chan);
    if (!desc) {
        return  (LW_NULL);
    }

    xdesc = (XDMA_DESC *)lib_malloc(sizeof(XDMA_DESC));
    if (!xdesc) {
        lib_free(desc);
        return  (LW_NULL);
    }
    lib_memset(xdesc, 0, sizeof(XDMA_DESC));
    xdesc->ip_type = DMA_IP_AXIDMA;

    if (nents == 1) {                                                   /*  Simple 模式                 */
        if (API_VmmVirtualToPhysical((addr_t)sgl[0].buf, &phys) != ERROR_NONE) {
            printk("xilinx_dma: axidma prep_sg VirtualToPhysical failed\n");
            lib_free(xdesc);
            lib_free(desc);
            return  (LW_NULL);
        }
        xdesc->mode       = XDMA_MODE_SIMPLE;
        xdesc->simple_len = (UINT32)sgl[0].len;
        if (hw->direction == DMA_DIR_MM2S) {
            xdesc->simple_src = (UINT32)phys;
        } else {
            xdesc->simple_dst = (UINT32)phys;
        }

    } else {                                                            /*  SG 模式                     */
        AXIDMA_BD  *bd_virt;
        UINT32      bd_phys;
        size_t      bd_bytes;
        int         i;

        if (!hw->has_sg) {                                              /*  硬件未启用 SG 模式          */
            printk("xilinx_dma: axidma SG not supported by this IP build\n");
            lib_free(xdesc);
            lib_free(desc);
            return  (LW_NULL);
        }

        bd_bytes = (size_t)nents * sizeof(AXIDMA_BD);
        bd_virt  = (AXIDMA_BD *)API_VmmDmaAllocAlign(bd_bytes, AXIDMA_BD_ALIGN);
        if (!bd_virt) {
            printk("xilinx_dma: axidma BD alloc failed (%u bytes)\n", (UINT)bd_bytes);
            lib_free(xdesc);
            lib_free(desc);
            return  (LW_NULL);
        }
        lib_memset(bd_virt, 0, bd_bytes);
        bd_phys = (UINT32)(addr_t)bd_virt;

        for (i = 0; i < nents; i++) {
            phys_addr_t buf_phys = 0;
            UINT32      next_p;

            if (API_VmmVirtualToPhysical((addr_t)sgl[i].buf, &buf_phys) != ERROR_NONE) {
                printk("xilinx_dma: axidma SG[%d] v2p failed\n", i);
                API_VmmDmaFree(bd_virt);
                lib_free(xdesc);
                lib_free(desc);
                return  (LW_NULL);
            }

            next_p = (i < nents - 1)
                   ? bd_phys + (UINT32)((i + 1) * (int)sizeof(AXIDMA_BD))
                   : bd_phys;                                           /*  末 BD 自指，形成环形        */

            bd_virt[i].next_desc     = next_p;
            bd_virt[i].next_desc_msb = 0;
            bd_virt[i].buf_addr      = (UINT32)buf_phys;
            bd_virt[i].buf_addr_msb  = 0;
            bd_virt[i].control       = (UINT32)(sgl[i].len & BD_CTRL_LEN_MASK);
            if (i == 0)        bd_virt[i].control |= BD_CTRL_SOF;
            if (i == nents-1)  bd_virt[i].control |= BD_CTRL_EOF;
            bd_virt[i].status = 0;
        }

        xdesc->mode    = XDMA_MODE_SG;
        xdesc->bd_virt = bd_virt;
        xdesc->bd_phys = bd_phys;
        xdesc->bd_num  = nents;
    }

    desc->hw_desc      = xdesc;
    desc->free_hw_desc = _xdesc_free_hw;
    desc->flags        = flags;

    return  (desc);
}
/*********************************************************************************************************
** 函数名称: _axidma_isr_core
** 功能描述: AXI DMA ISR 核心，MM2S 和 S2MM 共用。
**           读状态寄存器、清中断标志，判断完成/错误后设置 desc->status 并投递底半部任务。
** 输　入  : hw            XDMA_CHAN 指针（ISR 参数中传入）
** 输　出  : LW_IRQ_HANDLED 已处理；LW_IRQ_NONE 非本设备中断
*********************************************************************************************************/
static irqreturn_t _axidma_isr_core (XDMA_CHAN *hw)
{
    UINT32           sr_off = _axidma_sr_off(hw);
    UINT32           sr;
    struct dma_chan  *core  = hw->core_chan;
    struct dma_desc  *desc;
    XDMA_DESC        *xdesc;
    enum dma_status   status;

    sr = _xreg_rd(hw->regs, sr_off);
    if (!(sr & (DMASR_IOC_IRQ | DMASR_ERR_IRQ | DMASR_DLY_IRQ))) {
        return  (LW_IRQ_NONE);
    }
    _xreg_wr(hw->regs, sr_off, sr & (DMASR_IOC_IRQ | DMASR_ERR_IRQ | DMASR_DLY_IRQ));

    if (!core) {
        return  (LW_IRQ_HANDLED);
    }

    desc = _xdma_active_desc(core);
    if (!desc) {
        return  (LW_IRQ_HANDLED);
    }
    xdesc = (XDMA_DESC *)desc->hw_desc;

    if (sr & DMASR_ERR_MASK) {
        status = DMA_ERROR;
        _axidma_chan_stop(hw);

    } else if (xdesc && xdesc->mode == XDMA_MODE_SG) {
        AXIDMA_BD *last = &((AXIDMA_BD *)xdesc->bd_virt)[xdesc->bd_num - 1];
        if (last->status & BD_STS_ERR_MASK) {
            status = DMA_ERROR;
            _axidma_chan_stop(hw);
        } else if (last->status & BD_STS_CMPLT) {
            status = DMA_COMPLETE;
        } else {
            return  (LW_IRQ_HANDLED);
        }

    } else {
        status = DMA_COMPLETE;
    }

    desc->status = status;

    API_InterDeferJobAddEx(_G_defer_q, (VOIDFUNCPTR)_dma_complete_bh,
                           (PVOID)hw, (PVOID)core, (PVOID)desc,
                           LW_NULL, LW_NULL, LW_NULL);

    return  (LW_IRQ_HANDLED);
}
/*********************************************************************************************************
** 函数名称: _axidma_isr
** 功能描述: AXI DMA 统一中断服务函数（MM2S / S2MM 共用），委托给 _axidma_isr_core。
**           对标 Linux xilinx_dma_irq_handler；方向在 ISR core 内通过 hw->direction 识别。
** 输　入  : pvArg         XDMA_CHAN 指针（InterVectorConnect 注册时传入）
**           ulVector      中断向量号（未使用）
** 输　出  : irqreturn_t
*********************************************************************************************************/
static irqreturn_t _axidma_isr (PVOID pvArg, ULONG ulVector)
{
    return  _axidma_isr_core((XDMA_CHAN *)pvArg);
}

/*********************************************************************************************************
  AXI CDMA (PG034) 通道操作
  支持 Simple 模式（SRCADDR + DSTADDR + BTT 触发）和 SG 模式（CDMA_BD 链）。
  通道方向固定为 DMA_DIR_MEM2MEM；probe 时 has_sg 字段选择默认模式。
  _cdma_prep_dma_memcpy 统一使用 Simple 模式（等价于单 BD SG）。
*********************************************************************************************************/

/*  软复位 CDMA 通道：调用通用复位流程  */
static int _cdma_chan_reset (XDMA_CHAN *hw)
{
    return  _xdma_chan_reset_generic(hw->regs, CDMA_DMACR, CDMA_DMASR, "CDMA");
}

/*  Run/Stop=1，按 sg_mode 决定是否置 SGMODE 位，使能 IOC+ERR 中断  */
static VOID _cdma_chan_run (XDMA_CHAN *hw, BOOL sg_mode)
{
    UINT32 val = _xreg_rd(hw->regs, CDMA_DMACR);
    if (sg_mode) {
        val |= CDMA_DMACR_SGMODE;
    }
    val |= DMACR_RS | DMACR_IOC_IRQEN | DMACR_ERR_IRQEN;
    _xreg_wr(hw->regs, CDMA_DMACR, val);
}

/*  Run/Stop=0：停止 CDMA 通道  */
static VOID _cdma_chan_stop (XDMA_CHAN *hw)
{
    UINT32 val = _xreg_rd(hw->regs, CDMA_DMACR);
    val &= ~DMACR_RS;
    _xreg_wr(hw->regs, CDMA_DMACR, val);
}

/*********************************************************************************************************
** 函数名称: _cdma_alloc_resources
** 功能描述: CDMA 通道资源分配（device_alloc_chan_resources）。
**           复位通道并使能 IOC + ERR 中断。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 复位失败
*********************************************************************************************************/
static int _cdma_alloc_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;
    int        ret;

    ret = _cdma_chan_reset(hw);
    if (ret) {
        return  (ret);
    }
    _xreg_wr(hw->regs, CDMA_DMACR, DMACR_IOC_IRQEN | DMACR_ERR_IRQEN);
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _cdma_free_resources
** 功能描述: CDMA 通道资源释放（device_free_chan_resources）。停止并软复位通道。
** 输　入  : chan          核心层通道指针
** 输　出  : NONE
*********************************************************************************************************/
static VOID _cdma_free_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;
    _cdma_chan_stop(hw);
    _cdma_chan_reset(hw);
}
/*********************************************************************************************************
** 函数名称: _cdma_start_transfer
** 功能描述: CDMA 传输启动（start_transfer 函数指针）。
**           Simple 模式：RS=1 → 写 SRCADDR + DSTADDR → 写 BTT 触发传输。
**           SG 模式：RS=0 → 写 CURDESC → RS=1（SG）→ 写 TAILDESC 触发。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 描述符无效
*********************************************************************************************************/
static int _cdma_start_transfer (struct dma_chan *chan)
{
    XDMA_CHAN       *hw    = (XDMA_CHAN *)chan->priv;
    struct dma_desc *desc  = _xdma_active_desc(chan);
    XDMA_DESC       *xdesc;
    UINT32           tail_phys;
    int              n;

    if (!desc) {
        return  (0);
    }
    xdesc = (XDMA_DESC *)desc->hw_desc;
    if (!xdesc) {
        return  (-1);
    }

    if (xdesc->mode == XDMA_MODE_SIMPLE) {
        _cdma_chan_run(hw, LW_FALSE);
        _xreg_wr(hw->regs, CDMA_SRCADDR,     xdesc->simple_src);
        _xreg_wr(hw->regs, CDMA_SRCADDR_MSB, 0);
        _xreg_wr(hw->regs, CDMA_DSTADDR,     xdesc->simple_dst);
        _xreg_wr(hw->regs, CDMA_DSTADDR_MSB, 0);
        _xreg_wr(hw->regs, CDMA_BTT,         xdesc->simple_len); /*  写 BTT 触发传输  */

    } else {                                                            /*  CDMA SG 模式                */
        tail_phys = xdesc->bd_phys +
                    (UINT32)((xdesc->bd_num - 1) * (int)sizeof(CDMA_BD));

        _cdma_chan_stop(hw);
        for (n = XDMA_RESET_POLLS;
             n-- > 0 && !(_xreg_rd(hw->regs, CDMA_DMASR) & DMASR_HALTED); ) {}

        _xreg_wr(hw->regs, CDMA_CURDESC,     xdesc->bd_phys);
        _xreg_wr(hw->regs, CDMA_CURDESC_MSB, 0);
        _cdma_chan_run(hw, LW_TRUE);
        _xreg_wr(hw->regs, CDMA_TAILDESC,     tail_phys);
        _xreg_wr(hw->regs, CDMA_TAILDESC_MSB, 0);
    }

    return  (0);
}

/*********************************************************************************************************
** 函数名称: _cdma_stop_transfer
** 功能描述: CDMA 传输停止（stop_transfer 函数指针 / device_terminate_all）。
**           清除 DMACR.RS 位令通道停止，等待当前传输自然完成后进入 HALTED 状态。
** 输　入  : chan          核心层通道指针
** 输　出  : 0（始终成功）
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _cdma_stop_transfer (struct dma_chan *chan)
{
    _cdma_chan_stop((XDMA_CHAN *)chan->priv);
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _cdma_prep_dma_memcpy
** 功能描述: CDMA memcpy 传输准备（device_prep_dma_memcpy）。
**           转换虚拟地址为物理地址，填充 XDMA_DESC（Simple 模式）。
** 输　入  : chan          核心层通道指针（方向须为 DMA_DIR_MEM2MEM）
**           dst           目标虚拟地址
**           src           源虚拟地址
**           len           拷贝字节数
**           flags         DMA_PREP_* 标志
** 输　出  : 成功返回 dma_desc 指针；失败返回 NULL
*********************************************************************************************************/
static struct dma_desc *_cdma_prep_dma_memcpy (struct dma_chan *chan,
                                                void *dst, const void *src, size_t len,
                                                unsigned long flags)
{
    struct dma_desc *desc;
    XDMA_DESC       *xdesc;
    phys_addr_t      src_phys = 0, dst_phys = 0;

    desc = dma_desc_alloc(chan);
    if (!desc) {
        return  (LW_NULL);
    }

    xdesc = (XDMA_DESC *)lib_malloc(sizeof(XDMA_DESC));
    if (!xdesc) {
        lib_free(desc);
        return  (LW_NULL);
    }
    lib_memset(xdesc, 0, sizeof(XDMA_DESC));
    xdesc->ip_type = DMA_IP_CDMA;

    if (API_VmmVirtualToPhysical((addr_t)src, &src_phys) != ERROR_NONE ||
        API_VmmVirtualToPhysical((addr_t)dst, &dst_phys) != ERROR_NONE) {
        printk("xilinx_dma: cdma prep_memcpy v2p failed\n");
        lib_free(xdesc);
        lib_free(desc);
        return  (LW_NULL);
    }

    xdesc->mode       = XDMA_MODE_SIMPLE;
    xdesc->simple_src = (UINT32)src_phys;
    xdesc->simple_dst = (UINT32)dst_phys;
    xdesc->simple_len = (UINT32)len;

    desc->hw_desc      = xdesc;
    desc->free_hw_desc = _xdesc_free_hw;
    desc->flags        = flags;

    return  (desc);
}
/*********************************************************************************************************
** 函数名称: _cdma_isr
** 功能描述: CDMA 中断服务函数。
**           读 DMASR，清中断，判断完成/错误后设置 desc->status 并投递底半部任务。
** 输　入  : pvArg         XDMA_CHAN 指针
**           ulVector      中断向量号（未使用）
** 输　出  : LW_IRQ_HANDLED / LW_IRQ_NONE
*********************************************************************************************************/
static irqreturn_t _cdma_isr (PVOID pvArg, ULONG ulVector)
{
    XDMA_CHAN       *hw   = (XDMA_CHAN *)pvArg;
    UINT32           sr;
    struct dma_chan  *core = hw->core_chan;
    struct dma_desc  *desc;
    XDMA_DESC        *xdesc;
    enum dma_status   status;

    sr = _xreg_rd(hw->regs, CDMA_DMASR);
    if (!(sr & (DMASR_IOC_IRQ | DMASR_ERR_IRQ))) {
        return  (LW_IRQ_NONE);
    }
    _xreg_wr(hw->regs, CDMA_DMASR, sr & (DMASR_IOC_IRQ | DMASR_ERR_IRQ));

    if (!core) {
        return  (LW_IRQ_HANDLED);
    }
    desc = _xdma_active_desc(core);
    if (!desc) {
        return  (LW_IRQ_HANDLED);
    }
    xdesc = (XDMA_DESC *)desc->hw_desc;

    if (sr & CDMA_DMASR_ERR_MASK) {
        status = DMA_ERROR;
        _cdma_chan_stop(hw);

    } else if (xdesc && xdesc->mode == XDMA_MODE_CDMA_SG) {
        CDMA_BD *last = &((CDMA_BD *)xdesc->bd_virt)[xdesc->bd_num - 1];
        if (last->status & CDMA_BD_STS_ERR) {
            status = DMA_ERROR;
            _cdma_chan_stop(hw);
        } else if (last->status & CDMA_BD_STS_CMPLT) {
            status = DMA_COMPLETE;
        } else {
            return  (LW_IRQ_HANDLED);
        }

    } else {
        status = DMA_COMPLETE;
    }

    desc->status = status;

    API_InterDeferJobAddEx(_G_defer_q, (VOIDFUNCPTR)_dma_complete_bh,
                           (PVOID)hw, (PVOID)core, (PVOID)desc,
                           LW_NULL, LW_NULL, LW_NULL);

    return  (LW_IRQ_HANDLED);
}

/*********************************************************************************************************
  AXI VDMA (PG020) 通道操作
  VDMA 帧传输固定为 XDMA_MODE_VDMA；通道分 MM2S（读帧）和 S2MM（写帧）两个方向。
  写 VSIZE 寄存器触发传输；FRMCNT_IRQ 表示一轮（frm_cnt 帧）传输完成。
  park_frm ≥ 0：锁定到指定帧（PARK 模式）；park_frm < 0：帧循环（CIRC）模式。
*********************************************************************************************************/

/*  VDMA 通道控制基偏移（相对 IP 基地址）  */
static LW_INLINE UINT32 _vdma_ctrl_base (int direction)
{
    return  (direction == DMA_DIR_MM2S) ? VDMA_MM2S_CTRL_BASE : VDMA_S2MM_CTRL_BASE;
}

/*  VDMA 帧描述寄存器基偏移（相对 IP 基地址）  */
static LW_INLINE UINT32 _vdma_desc_base (int direction)
{
    return  (direction == DMA_DIR_MM2S) ? VDMA_MM2S_DESC_BASE : VDMA_S2MM_DESC_BASE;
}

/*********************************************************************************************************
** 函数名称: _vdma_alloc_resources
** 功能描述: VDMA 通道资源分配（device_alloc_chan_resources）。复位并使能 IOC + ERR 中断。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 复位失败
*********************************************************************************************************/
static int _vdma_alloc_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw  = (XDMA_CHAN *)chan->priv;
    UINT32     cb  = _vdma_ctrl_base(hw->direction);
    int        ret;

    ret = _xdma_chan_reset_generic(hw->regs, cb + VDMA_REG_DMACR,
                                   cb + VDMA_REG_DMASR,
                                   hw->direction == DMA_DIR_MM2S ? "VDMA-MM2S" : "VDMA-S2MM");
    if (ret) {
        return  (ret);
    }

    _xreg_wr(hw->regs, cb + VDMA_REG_DMACR, DMACR_IOC_IRQEN | DMACR_ERR_IRQEN);
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _vdma_free_resources
** 功能描述: VDMA 通道资源释放（device_free_chan_resources）。清 RS 位停止传输。
** 输　入  : chan          核心层通道指针
** 输　出  : NONE
*********************************************************************************************************/
static VOID _vdma_free_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw  = (XDMA_CHAN *)chan->priv;
    UINT32     cb  = _vdma_ctrl_base(hw->direction);
    UINT32     val = _xreg_rd(hw->regs, cb + VDMA_REG_DMACR);
    val &= ~DMACR_RS;
    _xreg_wr(hw->regs, cb + VDMA_REG_DMACR, val);
}
/*********************************************************************************************************
** 函数名称: _vdma_start_transfer
** 功能描述: VDMA 帧传输启动（start_transfer 函数指针）。
**           依次写 FRMSTORE → 帧地址寄存器组 → STRIDE → HSIZE → DMACR → VSIZE（触发）。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 描述符无效
*********************************************************************************************************/
static int _vdma_start_transfer (struct dma_chan *chan)
{
    XDMA_CHAN       *hw    = (XDMA_CHAN *)chan->priv;
    struct dma_desc *desc  = _xdma_active_desc(chan);
    XDMA_DESC       *xdesc;
    UINT32           cb, db;
    UINT32           dmacr;
    int              i;

    if (!desc) {
        return  (0);
    }
    xdesc = (XDMA_DESC *)desc->hw_desc;
    if (!xdesc || xdesc->mode != XDMA_MODE_VDMA) {
        return  (-1);
    }

    cb = _vdma_ctrl_base(hw->direction);
    db = _vdma_desc_base(hw->direction);

    /*  写帧缓冲数量  */
    _xreg_wr(hw->regs, VDMA_REG_FRMSTORE, (UINT32)xdesc->vdma_cfg.frm_cnt);

    /*  写各帧起始物理地址  */
    for (i = 0; i < xdesc->vdma_cfg.frm_cnt; i++) {
        _xreg_wr(hw->regs, db + VDMA_REG_STARTADDR(i), xdesc->frm_phys[i]);
    }

    /*  写 STRIDE 和 HSIZE  */
    _xreg_wr(hw->regs, db + VDMA_REG_STRIDE, xdesc->vdma_cfg.stride);
    _xreg_wr(hw->regs, db + VDMA_REG_HSIZE,  xdesc->vdma_cfg.hsize);

    /*  设置 PARK_PTR（park 模式 vs 循环模式）  */
    if (xdesc->vdma_cfg.park_frm >= 0) {
        UINT32 park = _xreg_rd(hw->regs, VDMA_REG_PARK_PTR);
        if (hw->direction == DMA_DIR_MM2S) {
            park = (park & ~0x1Fu) | ((UINT32)xdesc->vdma_cfg.park_frm & 0x1Fu);
        } else {
            park = (park & ~0x1F00u) | (((UINT32)xdesc->vdma_cfg.park_frm & 0x1Fu) << 8);
        }
        _xreg_wr(hw->regs, VDMA_REG_PARK_PTR, park);
    }

    /*  配置 DMACR，最后写 VSIZE 触发传输  */
    dmacr = DMACR_RS | DMACR_IOC_IRQEN | DMACR_ERR_IRQEN;
    if (xdesc->vdma_cfg.park_frm < 0) {
        dmacr |= VDMA_DMACR_CIRC_EN;                                   /*  循环模式                    */
    }
    dmacr |= VDMA_DMACR_FRAMECNT_EN |
             ((UINT32)xdesc->vdma_cfg.frm_cnt << VDMA_DMACR_FRM_CNT_SHIFT);
    _xreg_wr(hw->regs, cb + VDMA_REG_DMACR, dmacr);

    _xreg_wr(hw->regs, db + VDMA_REG_VSIZE, xdesc->vdma_cfg.vsize);   /*  写 VSIZE 触发               */

    return  (0);
}

/*********************************************************************************************************
** 函数名称: _vdma_stop_transfer
** 功能描述: VDMA 传输停止（stop_transfer 函数指针 / device_terminate_all）。
**           清除对应方向 DMACR.RS 位停止帧传输；下一帧不再启动。
** 输　入  : chan          核心层通道指针
** 输　出  : 0（始终成功）
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _vdma_stop_transfer (struct dma_chan *chan)
{
    XDMA_CHAN *hw  = (XDMA_CHAN *)chan->priv;
    UINT32     cb  = _vdma_ctrl_base(hw->direction);
    UINT32     val = _xreg_rd(hw->regs, cb + VDMA_REG_DMACR);
    val &= ~DMACR_RS;
    _xreg_wr(hw->regs, cb + VDMA_REG_DMACR, val);
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _vdma_prep_interleaved_dma
** 功能描述: VDMA 2D 帧传输准备（device_prep_interleaved_dma）。
**           将 dma_vdma_config 复制到 XDMA_DESC，并将各帧虚拟地址转换为物理地址。
** 输　入  : chan          核心层通道指针
**           cfg           VDMA 配置（帧数、尺寸、步幅、帧地址数组等）
**           flags         DMA_PREP_* 标志
** 输　出  : 成功返回 dma_desc 指针；失败返回 NULL
*********************************************************************************************************/
static struct dma_desc *_vdma_prep_interleaved_dma (struct dma_chan *chan,
                                                     const struct dma_vdma_config *cfg,
                                                     unsigned long flags)
{
    struct dma_desc *desc;
    XDMA_DESC       *xdesc;
    int              i;

    desc = dma_desc_alloc(chan);
    if (!desc) {
        return  (LW_NULL);
    }

    xdesc = (XDMA_DESC *)lib_malloc(sizeof(XDMA_DESC));
    if (!xdesc) {
        lib_free(desc);
        return  (LW_NULL);
    }
    lib_memset(xdesc, 0, sizeof(XDMA_DESC));
    xdesc->ip_type  = DMA_IP_VDMA;
    xdesc->mode     = XDMA_MODE_VDMA;
    xdesc->vdma_cfg = *cfg;

    for (i = 0; i < cfg->frm_cnt; i++) {
        phys_addr_t phys = 0;
        if (API_VmmVirtualToPhysical((addr_t)cfg->frm_addrs[i], &phys) != ERROR_NONE) {
            printk("xilinx_dma: vdma frm[%d] v2p failed\n", i);
            lib_free(xdesc);
            lib_free(desc);
            return  (LW_NULL);
        }
        xdesc->frm_phys[i] = (UINT32)phys;
    }

    desc->hw_desc      = xdesc;
    desc->free_hw_desc = _xdesc_free_hw;
    desc->flags        = flags;

    return  (desc);
}
/*********************************************************************************************************
** 函数名称: _vdma_isr_core
** 功能描述: VDMA ISR 核心，MM2S 和 S2MM 共用。
**           读状态寄存器、清中断，判断帧完成/错误后设置 desc->status 并投递底半部。
** 输　入  : hw            XDMA_CHAN 指针
** 输　出  : LW_IRQ_HANDLED / LW_IRQ_NONE
*********************************************************************************************************/
static irqreturn_t _vdma_isr_core (XDMA_CHAN *hw)
{
    UINT32           cb   = _vdma_ctrl_base(hw->direction);
    UINT32           sr;
    struct dma_chan  *core = hw->core_chan;
    struct dma_desc  *desc;
    enum dma_status   status;

    sr = _xreg_rd(hw->regs, cb + VDMA_REG_DMASR);
    if (!(sr & (VDMA_DMASR_FRM_CNT_IRQ | VDMA_DMASR_ERR_IRQ))) {
        return  (LW_IRQ_NONE);
    }
    _xreg_wr(hw->regs, cb + VDMA_REG_DMASR,
             sr & (VDMA_DMASR_FRM_CNT_IRQ | VDMA_DMASR_ERR_IRQ));

    if (!core) {
        return  (LW_IRQ_HANDLED);
    }
    desc = _xdma_active_desc(core);
    if (!desc) {
        return  (LW_IRQ_HANDLED);
    }

    if (sr & VDMA_DMASR_ERR_MASK) {
        status = DMA_ERROR;
        _vdma_stop_transfer(core);
    } else {
        status = DMA_COMPLETE;
    }

    desc->status = status;

    API_InterDeferJobAddEx(_G_defer_q, (VOIDFUNCPTR)_dma_complete_bh,
                           (PVOID)hw, (PVOID)core, (PVOID)desc,
                           LW_NULL, LW_NULL, LW_NULL);

    return  (LW_IRQ_HANDLED);
}
/*********************************************************************************************************
** 函数名称: _vdma_isr
** 功能描述: VDMA 统一中断服务函数（MM2S / S2MM 共用），委托给 _vdma_isr_core。
**           对标 Linux xilinx_dma_irq_handler；方向在 ISR core 内通过 hw->direction 识别。
** 输　入  : pvArg         XDMA_CHAN 指针
**           ulVector      中断向量号（未使用）
** 输　出  : irqreturn_t
*********************************************************************************************************/
static irqreturn_t _vdma_isr (PVOID pvArg, ULONG ulVector)
{
    return  _vdma_isr_core((XDMA_CHAN *)pvArg);
}

/*********************************************************************************************************
  AXI MCDMA (PG288) 通道操作
  MCDMA 为 SG-only 模式；每通道对应唯一 TDEST，在 BD control 字段的 TUSER[23:16] 中设置。
  XDMA_CHAN.tdest 为该通道的 TDEST 值，由 mcdma_probe 按通道索引赋值。
  每通道独立 CR/SR，写 TDESC 触发传输。
*********************************************************************************************************/

/*  返回通道对应的方向全局基地址（虚拟）  */
static LW_INLINE addr_t _mcdma_dir_base (XDMA_CHAN *hw)
{
    return  hw->regs + ((hw->direction == DMA_DIR_MM2S)
                        ? MCDMA_MM2S_BASE : MCDMA_S2MM_BASE);
}

/*********************************************************************************************************
** 函数名称: _mcdma_alloc_resources
** 功能描述: MCDMA 通道资源分配（device_alloc_chan_resources）。
**           使能通道位（CHEN），配置 CR（IOC+DLY+ERR 中断使能，coalesce 阈值=1）。
** 输　入  : chan          核心层通道指针
** 输　出  : 0（始终成功）
*********************************************************************************************************/
static int _mcdma_alloc_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw   = (XDMA_CHAN *)chan->priv;
    addr_t     db   = _mcdma_dir_base(hw);
    int        x    = hw->chan_id;
    UINT32     chen;

    chen  = _xreg_rd(db, MCDMA_CHEN);
    chen |= (1u << x);
    _xreg_wr(db, MCDMA_CHEN, chen);

    _xreg_wr(db, MCDMA_CH_CR(x),
             MCDMA_CR_IRQEN_ALL | (1u << MCDMA_COALESCE_SHIFT));       /*  coalesce 阈值 = 1           */

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _mcdma_free_resources
** 功能描述: MCDMA 通道资源释放（device_free_chan_resources）。清 CR[RS] 位停止通道。
** 输　入  : chan          核心层通道指针
** 输　出  : NONE
*********************************************************************************************************/
static VOID _mcdma_free_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw  = (XDMA_CHAN *)chan->priv;
    addr_t     db  = _mcdma_dir_base(hw);
    int        x   = hw->chan_id;
    UINT32     val = _xreg_rd(db, MCDMA_CH_CR(x));
    val &= ~MCDMA_CR_RS;
    _xreg_wr(db, MCDMA_CH_CR(x), val);
}
/*********************************************************************************************************
** 函数名称: _mcdma_start_transfer
** 功能描述: MCDMA 传输启动（start_transfer 函数指针）。
**           写 CDESC → CR[RS]=1 → 写 TDESC 触发传输（SG-only）。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 描述符无效
*********************************************************************************************************/
static int _mcdma_start_transfer (struct dma_chan *chan)
{
    XDMA_CHAN       *hw    = (XDMA_CHAN *)chan->priv;
    struct dma_desc *desc  = _xdma_active_desc(chan);
    XDMA_DESC       *xdesc;
    addr_t           db    = _mcdma_dir_base(hw);
    int              x     = hw->chan_id;
    UINT32           tail_phys;

    if (!desc) {
        return  (0);
    }
    xdesc = (XDMA_DESC *)desc->hw_desc;
    if (!xdesc) {
        return  (-1);
    }

    tail_phys = xdesc->bd_phys +
                (UINT32)((xdesc->bd_num - 1) * (int)sizeof(MCDMA_BD));

    _xreg_wr(db, MCDMA_CH_CDESC(x), xdesc->bd_phys);
    {
        UINT32 val = _xreg_rd(db, MCDMA_CH_CR(x));
        val |= MCDMA_CR_RS;
        _xreg_wr(db, MCDMA_CH_CR(x), val);
    }
    _xreg_wr(db, MCDMA_CH_TDESC(x), tail_phys);                        /*  写 TDESC 触发传输           */

    return  (0);
}

/*********************************************************************************************************
** 函数名称: _mcdma_stop_transfer
** 功能描述: MCDMA 通道传输停止（stop_transfer 函数指针 / device_terminate_all）。
**           清除通道 CR.RS 位停止传输；当前 BD 完成后通道进入停止状态。
** 输　入  : chan          核心层通道指针
** 输　出  : 0（始终成功）
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _mcdma_stop_transfer (struct dma_chan *chan)
{
    XDMA_CHAN *hw  = (XDMA_CHAN *)chan->priv;
    addr_t     db  = _mcdma_dir_base(hw);
    int        x   = hw->chan_id;
    UINT32     val = _xreg_rd(db, MCDMA_CH_CR(x));
    val &= ~MCDMA_CR_RS;
    _xreg_wr(db, MCDMA_CH_CR(x), val);
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _mcdma_prep_slave_sg
** 功能描述: MCDMA SG 传输准备（device_prep_slave_sg，SG-only）。
**           分配 MCDMA_BD 链，设置 SOP/EOP 和 TUSER（= hw->tdest）。
** 输　入  : chan          核心层通道指针
**           sgl           SG 列表（虚拟地址）
**           nents         条目数（≥1）
**           flags         DMA_PREP_* 标志
** 输　出  : 成功返回 dma_desc 指针；失败返回 NULL
*********************************************************************************************************/
static struct dma_desc *_mcdma_prep_slave_sg (struct dma_chan *chan,
                                               struct dma_sg *sgl, int nents,
                                               unsigned long flags)
{
    XDMA_CHAN       *hw = (XDMA_CHAN *)chan->priv;
    struct dma_desc *desc;
    XDMA_DESC       *xdesc;
    MCDMA_BD        *bd_virt;
    UINT32           bd_phys;
    size_t           bd_bytes;
    int              i;

    desc = dma_desc_alloc(chan);
    if (!desc) {
        return  (LW_NULL);
    }

    xdesc = (XDMA_DESC *)lib_malloc(sizeof(XDMA_DESC));
    if (!xdesc) {
        lib_free(desc);
        return  (LW_NULL);
    }
    lib_memset(xdesc, 0, sizeof(XDMA_DESC));
    xdesc->ip_type = DMA_IP_MCDMA;
    xdesc->mode    = XDMA_MODE_SG;
    xdesc->bd_num  = nents;

    bd_bytes = (size_t)nents * sizeof(MCDMA_BD);
    bd_virt  = (MCDMA_BD *)API_VmmDmaAllocAlign(bd_bytes, MCDMA_BD_ALIGN);
    if (!bd_virt) {
        printk("xilinx_dma: mcdma BD alloc failed (%u bytes)\n", (UINT)bd_bytes);
        lib_free(xdesc);
        lib_free(desc);
        return  (LW_NULL);
    }
    lib_memset(bd_virt, 0, bd_bytes);
    bd_phys = (UINT32)(addr_t)bd_virt;

    for (i = 0; i < nents; i++) {
        phys_addr_t buf_phys = 0;
        UINT32      next_p;

        if (API_VmmVirtualToPhysical((addr_t)sgl[i].buf, &buf_phys) != ERROR_NONE) {
            printk("xilinx_dma: mcdma SG[%d] v2p failed\n", i);
            API_VmmDmaFree(bd_virt);
            lib_free(xdesc);
            lib_free(desc);
            return  (LW_NULL);
        }

        next_p = (i < nents - 1)
               ? bd_phys + (UINT32)((i + 1) * (int)sizeof(MCDMA_BD))
               : bd_phys;                                               /*  末 BD 自指，形成环形        */

        bd_virt[i].next_desc = next_p;
        bd_virt[i].buf_addr  = (UINT32)buf_phys;
        bd_virt[i].control   = (UINT32)(sgl[i].len & MCDMA_BD_LEN_MASK)
                             | ((UINT32)hw->tdest << MCDMA_BD_TUSER_SHIFT);
        if (i == 0)        bd_virt[i].control |= MCDMA_BD_SOP;
        if (i == nents-1)  bd_virt[i].control |= MCDMA_BD_EOP;
        bd_virt[i].status = 0;
    }

    xdesc->bd_virt = bd_virt;
    xdesc->bd_phys = bd_phys;

    desc->hw_desc      = xdesc;
    desc->free_hw_desc = _xdesc_free_hw;
    desc->flags        = flags;

    return  (desc);
}
/*********************************************************************************************************
** 函数名称: _mcdma_chan_isr
** 功能描述: MCDMA 单通道中断服务函数（每通道独立注册）。
**           读通道 SR，清中断，检查完成/错误后投递底半部任务。
** 输　入  : pvArg         XDMA_CHAN 指针
**           ulVector      中断向量号（未使用）
** 输　出  : LW_IRQ_HANDLED / LW_IRQ_NONE
*********************************************************************************************************/
static irqreturn_t _mcdma_chan_isr (PVOID pvArg, ULONG ulVector)
{
    XDMA_CHAN       *hw   = (XDMA_CHAN *)pvArg;
    addr_t           db   = _mcdma_dir_base(hw);
    int              x    = hw->chan_id;
    UINT32           sr;
    struct dma_chan  *core = hw->core_chan;
    struct dma_desc  *desc;
    XDMA_DESC        *xdesc;
    enum dma_status   status;

    sr = _xreg_rd(db, MCDMA_CH_SR(x));
    if (!(sr & (MCDMA_SR_IOC_IRQ | MCDMA_SR_ERR_IRQ | MCDMA_SR_DLY_IRQ))) {
        return  (LW_IRQ_NONE);
    }
    _xreg_wr(db, MCDMA_CH_SR(x),
             sr & (MCDMA_SR_IOC_IRQ | MCDMA_SR_ERR_IRQ | MCDMA_SR_DLY_IRQ));

    if (!core) {
        return  (LW_IRQ_HANDLED);
    }
    desc = _xdma_active_desc(core);
    if (!desc) {
        return  (LW_IRQ_HANDLED);
    }
    xdesc = (XDMA_DESC *)desc->hw_desc;

    if (sr & MCDMA_SR_ERR_MASK) {
        status = DMA_ERROR;
        _mcdma_stop_transfer(core);

    } else {
        MCDMA_BD *last = &((MCDMA_BD *)xdesc->bd_virt)[xdesc->bd_num - 1];
        if (last->status & MCDMA_BD_STS_ERR) {
            status = DMA_ERROR;
            _mcdma_stop_transfer(core);
        } else if (last->status & MCDMA_BD_STS_CMPLT) {
            status = DMA_COMPLETE;
        } else {
            return  (LW_IRQ_HANDLED);
        }
    }

    desc->status = status;

    API_InterDeferJobAddEx(_G_defer_q, (VOIDFUNCPTR)_dma_complete_bh,
                           (PVOID)hw, (PVOID)core, (PVOID)desc,
                           LW_NULL, LW_NULL, LW_NULL);

    return  (LW_IRQ_HANDLED);
}

/*********************************************************************************************************
  统一 ops 表包装函数
  所有 IP 共用一张 xilinx_dma_ops。包装层通过 XDMA_CHAN.ip_type 或函数指针分派到具体实现。
*********************************************************************************************************/

/*********************************************************************************************************
** 函数名称: _xdma_device_alloc_chan_resources
** 功能描述: ops.device_alloc_chan_resources 包装：调用通道的 alloc_resources 函数指针。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；非 0 失败
*********************************************************************************************************/
static int _xdma_device_alloc_chan_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;

    if (hw->alloc_resources) {
        return  hw->alloc_resources(chan);
    }
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _xdma_device_free_chan_resources
** 功能描述: ops.device_free_chan_resources 包装：调用通道的 free_resources 函数指针。
** 输　入  : chan          核心层通道指针
** 输　出  : NONE
*********************************************************************************************************/
static VOID _xdma_device_free_chan_resources (struct dma_chan *chan)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;

    if (hw->free_resources) {
        hw->free_resources(chan);
    }
}
/*********************************************************************************************************
** 函数名称: _xdma_device_prep_dma_memcpy
** 功能描述: ops.device_prep_dma_memcpy 包装：仅 CDMA 支持，其余 IP 返回 NULL。
** 输　入  : chan          核心层通道指针
**           dst           目标虚拟地址
**           src           源虚拟地址
**           len           字节数
**           flags         DMA_PREP_* 标志
** 输　出  : 成功返回 dma_desc 指针；不支持或失败返回 NULL
*********************************************************************************************************/
static struct dma_desc *_xdma_device_prep_dma_memcpy (struct dma_chan *chan,
                                                        void *dst, const void *src,
                                                        size_t len, unsigned long flags)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;

    if (hw->ip_type == DMA_IP_CDMA) {
        return  _cdma_prep_dma_memcpy(chan, dst, src, len, flags);
    }

    printk("xilinx_dma: prep_dma_memcpy not supported for ip_type=%d\n", hw->ip_type);
    return  (LW_NULL);
}
/*********************************************************************************************************
** 函数名称: _xdma_device_prep_slave_sg
** 功能描述: ops.device_prep_slave_sg 包装：AXI DMA 和 MCDMA 支持，其余返回 NULL。
** 输　入  : chan          核心层通道指针
**           sgl           SG 列表（虚拟地址）
**           nents         条目数
**           flags         DMA_PREP_* 标志
** 输　出  : 成功返回 dma_desc 指针；不支持或失败返回 NULL
*********************************************************************************************************/
static struct dma_desc *_xdma_device_prep_slave_sg (struct dma_chan *chan,
                                                      struct dma_sg *sgl, int nents,
                                                      unsigned long flags)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;

    switch (hw->ip_type) {
    case DMA_IP_AXIDMA:
        return  _axidma_prep_slave_sg(chan, sgl, nents, flags);
    case DMA_IP_MCDMA:
        return  _mcdma_prep_slave_sg(chan, sgl, nents, flags);
    default:
        printk("xilinx_dma: prep_slave_sg not supported for ip_type=%d\n", hw->ip_type);
        return  (LW_NULL);
    }
}
/*********************************************************************************************************
** 函数名称: _xdma_device_prep_interleaved_dma
** 功能描述: ops.device_prep_interleaved_dma 包装：仅 VDMA 支持，其余返回 NULL。
** 输　入  : chan          核心层通道指针
**           cfg           VDMA 2D 传输配置
**           flags         DMA_PREP_* 标志
** 输　出  : 成功返回 dma_desc 指针；不支持或失败返回 NULL
*********************************************************************************************************/
static struct dma_desc *_xdma_device_prep_interleaved_dma (struct dma_chan *chan,
                                                             const struct dma_vdma_config *cfg,
                                                             unsigned long flags)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;

    if (hw->ip_type == DMA_IP_VDMA) {
        return  _vdma_prep_interleaved_dma(chan, cfg, flags);
    }

    printk("xilinx_dma: prep_interleaved not supported for ip_type=%d\n", hw->ip_type);
    return  (LW_NULL);
}
/*********************************************************************************************************
** 函数名称: _xdma_device_issue_pending
** 功能描述: ops.device_issue_pending 包装：调用通道的 start_transfer 函数指针。
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/
static int _xdma_device_issue_pending (struct dma_chan *chan)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;

    if (hw->start_transfer) {
        return  hw->start_transfer(chan);
    }
    return  (0);
}
/*********************************************************************************************************
** 函数名称: _xdma_device_terminate_all
** 功能描述: ops.device_terminate_all 包装：调用通道的 stop_transfer 函数指针。
** 输　入  : chan          核心层通道指针
** 输　出  : 0（始终）
*********************************************************************************************************/
static int _xdma_device_terminate_all (struct dma_chan *chan)
{
    XDMA_CHAN *hw = (XDMA_CHAN *)chan->priv;

    if (hw->stop_transfer) {
        hw->stop_transfer(chan);
    }
    return  (0);
}

/*********************************************************************************************************
  共享操作表（所有 Xilinx DMA IP 实例公用）
*********************************************************************************************************/

struct dma_ops xilinx_dma_ops = {
    .device_alloc_chan_resources  = _xdma_device_alloc_chan_resources,
    .device_free_chan_resources   = _xdma_device_free_chan_resources,
    .device_prep_dma_memcpy       = _xdma_device_prep_dma_memcpy,
    .device_prep_slave_sg         = _xdma_device_prep_slave_sg,
    .device_prep_interleaved_dma  = _xdma_device_prep_interleaved_dma,
    .device_issue_pending         = _xdma_device_issue_pending,
    .device_tx_status             = LW_NULL,                            /*  cookie 比较已足够            */
    .device_terminate_all         = _xdma_device_terminate_all,
};

/*********************************************************************************************************
  通用 probe / remove 辅助 + 三层探测层次

  对标 Linux xilinx_dma.c 探测结构：
    xilinx_dma_probe      → 外部入口（对标 platform_driver.probe；ioremap、分配设备/通道数组）
    xilinx_dma_child_probe → 按方向批量探测（等价 Linux child_probe per 子节点）
    xilinx_dma_chan_probe  → 单通道初始化（分配函数指针、读 SG 能力、注册中断）

  SylixOS 无设备树，IP 类型由 xilinx_dma_probe_config.ip_type 区分
  （对标 Linux of_match_table compatible 字符串选出的 per-IP 静态配置表）。
  内部 per-IP 静态实例（_axidma_hw_config 等）包含 ISR 和通道数量，
  由 _xdma_probe_common 写入 xdev->dma_config，供三层层次按 IP 分派。
*********************************************************************************************************/

/*********************************************************************************************************
  per-IP 静态配置表实例（对标 Linux static const struct xilinx_dma_config axidma_config = {...}）
  irq_handler：统一 ISR，pvArg 为 XDMA_CHAN*，内部通过 hw->direction 区分通道。
*********************************************************************************************************/

static const struct xilinx_dma_config _axidma_hw_config = {
    .ip_type      = DMA_IP_AXIDMA,
    .max_channels = XDMA_AXIDMA_MAX_CHANS,
    .irq_handler  = _axidma_isr,
    .irq_name     = "axidma",
};

static const struct xilinx_dma_config _cdma_hw_config = {
    .ip_type      = DMA_IP_CDMA,
    .max_channels = XDMA_CDMA_MAX_CHANS,
    .irq_handler  = _cdma_isr,
    .irq_name     = "cdma",
};

static const struct xilinx_dma_config _vdma_hw_config = {
    .ip_type      = DMA_IP_VDMA,
    .max_channels = XDMA_VDMA_MAX_CHANS,
    .irq_handler  = _vdma_isr,
    .irq_name     = "vdma",
};

static const struct xilinx_dma_config _mcdma_hw_config = {
    .ip_type      = DMA_IP_MCDMA,
    .max_channels = XDMA_MCDMA_MAX_CHANS * 2,                          /*  MM2S + S2MM 总数            */
    .irq_handler  = _mcdma_chan_isr,
    .irq_name     = "mcdma",
};

/*********************************************************************************************************
** 函数名称: _xdma_probe_common
** 功能描述: 顶层 probe 辅助（对标 Linux xilinx_dma_probe 前半段）。
**           ioremap 寄存器区，分配 XDMA_DEVICE 及通道数组，填充 dev->priv/ops/ip_type/dma_config。
** 输　入  : dev           dma_device 指针（调用前须设置 name）
**           phys_base     IP 物理基地址
**           reg_size      寄存器区字节数
**           n_chans       通道总数
**           cfg           per-IP 静态配置表指针
** 输　出  : 0 成功；-1 失败（已释放所有已分配资源）
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _xdma_probe_common (struct dma_device *dev, addr_t phys_base,
                                size_t reg_size, int n_chans,
                                const struct xilinx_dma_config *cfg)
{
    XDMA_DEVICE *xdev;
    PVOID        regs_virt;

    if (_G_defer_q == LW_NULL) {
        _G_defer_q = API_InterDeferGet(0);
    }

    xdev = (XDMA_DEVICE *)lib_malloc(sizeof(XDMA_DEVICE));
    if (!xdev) {
        return  (-1);
    }
    lib_memset(xdev, 0, sizeof(XDMA_DEVICE));
    xdev->ip_type    = cfg->ip_type;
    xdev->dma_config = cfg;

    regs_virt = API_VmmMallocAlign(reg_size, LW_CFG_VMM_PAGE_SIZE, LW_VMM_FLAG_DMA);
    if (!regs_virt) {
        printk("xilinx_dma: ioremap alloc failed\n");
        lib_free(xdev);
        return  (-1);
    }

    if (API_VmmMap(regs_virt, (PVOID)(size_t)phys_base, reg_size,
                   LW_VMM_FLAG_DMA) != ERROR_NONE) {
        printk("xilinx_dma: VmmMap failed @ 0x%08lX\n", (ULONG)phys_base);
        API_VmmFree(regs_virt);
        lib_free(xdev);
        return  (-1);
    }
    xdev->regs_virt = (addr_t)regs_virt;

    xdev->hw_chans = (XDMA_CHAN *)lib_malloc((size_t)n_chans * sizeof(XDMA_CHAN));
    if (!xdev->hw_chans) {
        API_VmmFree(regs_virt);
        lib_free(xdev);
        return  (-1);
    }
    lib_memset(xdev->hw_chans, 0, (size_t)n_chans * sizeof(XDMA_CHAN));

    dev->channels = (struct dma_chan *)lib_malloc((size_t)n_chans * sizeof(struct dma_chan));
    if (!dev->channels) {
        lib_free(xdev->hw_chans);
        API_VmmFree(regs_virt);
        lib_free(xdev);
        return  (-1);
    }
    lib_memset(dev->channels, 0, (size_t)n_chans * sizeof(struct dma_chan));

    xdev->n_chans = n_chans;
    dev->priv     = xdev;
    dev->ip_type  = cfg->ip_type;
    dev->ops      = &xilinx_dma_ops;

    return  (0);
}

/*********************************************************************************************************
** 函数名称: _xdma_remove_common
** 功能描述: 通用 remove 辅助：注销设备，释放通道数组、寄存器映射和 XDMA_DEVICE。
** 输　入  : dev           dma_device 指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _xdma_remove_common (struct dma_device *dev)
{
    XDMA_DEVICE *xdev;

    if (!dev || !dev->priv) {
        return;
    }
    xdev = (XDMA_DEVICE *)dev->priv;

    dma_device_unregister(dev);

    lib_free(xdev->hw_chans);
    API_VmmFree((PVOID)xdev->regs_virt);
    lib_free(dev->channels);
    dev->channels     = LW_NULL;
    dev->num_channels = 0;
    lib_free(xdev);
    dev->priv = LW_NULL;
}

/*********************************************************************************************************
** 函数名称: _xdma_init_core_chan
** 功能描述: 初始化 struct dma_chan 字段，建立与 XDMA_CHAN 的双向关联。
**           调用前 hw->direction 须已赋值。
** 输　入  : cc            struct dma_chan 指针（dev->channels[id]）
**           dev           所属 dma_device
**           hw            XDMA_CHAN 指针
**           id            通道编号（在 dev->channels 数组中的索引）
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _xdma_init_core_chan (struct dma_chan *cc, struct dma_device *dev,
                                   XDMA_CHAN *hw, int id)
{
    cc->dev       = dev;
    cc->id        = id;
    cc->priv      = hw;
    cc->in_use    = LW_FALSE;
    cc->direction = hw->direction;
    LW_SPIN_INIT(&cc->lock);
    hw->core_chan = cc;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_chan_probe
** 功能描述: 单通道探测初始化（对标 Linux xilinx_dma_chan_probe）。
**           根据 xdev->dma_config->ip_type 分配函数指针（start/stop/alloc/free），
**           读取 SG 能力（AXIDMA 从 DMASR bit3；CDMA 来自参数；MCDMA 恒为 LW_TRUE），
**           注册中断向量并使能，调用 alloc_resources 完成通道硬件初始化。
** 输　入  : xdev          XDMA_DEVICE 指针
**           dev           dma_device 指针
**           chan_idx      在 dev->channels 数组中的下标
**           direction     DMA_DIR_MM2S / DMA_DIR_S2MM / DMA_DIR_MEM2MEM
**           chan_id       MCDMA 每方向通道索引（其他 IP 传 0）
**           tdest         MCDMA TDEST 值（其他 IP 传 0）
**           irq           中断向量号
**           has_sg        CDMA 专用：cfg->has_sg；其他 IP 内部自动决定
**           num_frms      VDMA 专用：cfg->max_frm_cnt；其他 IP 传 0
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int xilinx_dma_chan_probe (XDMA_DEVICE *xdev, struct dma_device *dev,
                                   int chan_idx, int direction, int chan_id,
                                   int tdest, ULONG irq,
                                   BOOL has_sg, int num_frms)
{
    XDMA_CHAN      *hw = &xdev->hw_chans[chan_idx];
    struct dma_chan *cc = &dev->channels[chan_idx];
    int             ip  = xdev->dma_config->ip_type;

    hw->tdest     = tdest;
    hw->ip_type   = ip;
    hw->regs      = xdev->regs_virt;
    hw->irq       = irq;
    hw->direction = direction;
    hw->chan_id   = chan_id;

    /*  按 IP 类型分配函数指针（对标 Linux xilinx_dma_chan_probe 中的 switch on dmatype）  */
    switch (ip) {
    case DMA_IP_AXIDMA:
        hw->start_transfer  = _axidma_start_transfer;
        hw->stop_transfer   = _axidma_stop_transfer;
        hw->alloc_resources = _axidma_alloc_resources;
        hw->free_resources  = _axidma_free_resources;
        /*  SG 能力从硬件读取（对标 Linux 读 DMASR_SG_MASK bit）  */
        if (direction == DMA_DIR_MM2S) {
            hw->has_sg = (_xreg_rd(xdev->regs_virt, MM2S_DMASR) & DMASR_SG_MASK)
                         ? LW_TRUE : LW_FALSE;
        } else {
            hw->has_sg = (_xreg_rd(xdev->regs_virt, S2MM_DMASR) & DMASR_SG_MASK)
                         ? LW_TRUE : LW_FALSE;
        }
        hw->num_frms = 0;
        break;

    case DMA_IP_CDMA:
        hw->start_transfer  = _cdma_start_transfer;
        hw->stop_transfer   = _cdma_stop_transfer;
        hw->alloc_resources = _cdma_alloc_resources;
        hw->free_resources  = _cdma_free_resources;
        hw->has_sg   = has_sg;                                          /*  来自 cfg->has_sg            */
        hw->num_frms = 0;
        break;

    case DMA_IP_VDMA:
        hw->start_transfer  = _vdma_start_transfer;
        hw->stop_transfer   = _vdma_stop_transfer;
        hw->alloc_resources = _vdma_alloc_resources;
        hw->free_resources  = _vdma_free_resources;
        hw->has_sg   = LW_FALSE;                                        /*  VDMA 无 BD 链               */
        hw->num_frms = num_frms;                                        /*  来自 cfg->max_frm_cnt       */
        break;

    case DMA_IP_MCDMA:
        hw->start_transfer  = _mcdma_start_transfer;
        hw->stop_transfer   = _mcdma_stop_transfer;
        hw->alloc_resources = _mcdma_alloc_resources;
        hw->free_resources  = _mcdma_free_resources;
        hw->has_sg   = LW_TRUE;                                         /*  MCDMA 固定 SG-only          */
        hw->num_frms = 0;
        break;

    default:
        return  (-1);
    }

    _xdma_init_core_chan(cc, dev, hw, chan_idx);
    {
        #define IRQ_TYPE_LEVEL_HIGH             (0x00000004)
        extern VOID  bspIntVectorTypeSet(ULONG  ulVector, INT  iType);
        bspIntVectorTypeSet(irq, IRQ_TYPE_LEVEL_HIGH);
    }

    API_InterVectorConnect(irq, xdev->dma_config->irq_handler, (PVOID)hw,
                           xdev->dma_config->irq_name);
    API_InterVectorEnable(irq);

    return  (0);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_child_probe
** 功能描述: 按方向批量探测通道（对标 Linux xilinx_dma_child_probe）。
**           遍历 nr_channels，逐一调用 xilinx_dma_chan_probe；
**           MCDMA 中 tdest == chan_id（通道索引），其他 IP tdest 为 0。
** 输　入  : xdev          XDMA_DEVICE 指针
**           dev           dma_device 指针
**           p_chan_idx    当前通道下标（in/out，每成功探测一个通道 +1）
**           direction     DMA_DIR_MM2S / DMA_DIR_S2MM / DMA_DIR_MEM2MEM
**           nr_channels   本次探测的通道数
**           irqs          各通道中断向量数组（长度 nr_channels）
**           has_sg        透传给 xilinx_dma_chan_probe（CDMA 专用）
**           num_frms      透传给 xilinx_dma_chan_probe（VDMA 专用）
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int xilinx_dma_child_probe (XDMA_DEVICE *xdev, struct dma_device *dev,
                                    int *p_chan_idx, int direction,
                                    int nr_channels, const ULONG *irqs,
                                    BOOL has_sg, int num_frms)
{
    int ip  = xdev->dma_config->ip_type;
    int ret;
    int i;

    for (i = 0; i < nr_channels; i++) {
        int tdest = (ip == DMA_IP_MCDMA) ? i : 0;

        ret = xilinx_dma_chan_probe(xdev, dev, *p_chan_idx,
                                     direction, i, tdest, irqs[i],
                                     has_sg, num_frms);
        if (ret) {
            return  (ret);
        }
        (*p_chan_idx)++;
    }
    return  (0);
}

/*********************************************************************************************************
  xilinx_dma_probe / xilinx_dma_remove
  统一入口，对标 Linux platform_driver.probe / remove。
  内部通过 cfg->ip_type 选择 per-IP 静态配置表，再调用三层内部探测层次。
*********************************************************************************************************/

/*********************************************************************************************************
** 函数名称: xilinx_dma_probe
** 功能描述: Xilinx DMA 统一探测入口（对标 Linux xilinx_dma_probe）。
**           通过 cfg->ip_type 选择对应的 per-IP 静态配置表（_axidma_hw_config 等），
**           依次调用 _xdma_probe_common → xilinx_dma_child_probe → xilinx_dma_chan_probe，
**           完成寄存器映射、通道初始化、中断注册和设备注册。
** 输　入  : dev           dma_device 指针（调用前须设置 name）
**           cfg           统一板级配置（ip_type + 对应 IP 专属字段）
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int xilinx_dma_probe (struct dma_device *dev, const struct xilinx_dma_probe_config *cfg)
{
    static const char *ip_names[] = { "axidma", "cdma", "vdma", "mcdma" };
    XDMA_DEVICE       *xdev;
    int                chan_idx  = 0;
    int                max_chans = 0;
    size_t             reg_size  = XDMA_REG_MAP_SIZE;
    const struct xilinx_dma_config *hw_cfg;
    int                ret;
    ULONG              irq;

    if (!dev || !cfg) {
        return  (-1);
    }

    /*  第一层：选择 per-IP 静态配置表，计算通道总数和寄存器窗口大小  */
    switch (cfg->ip_type) {

    case DMA_IP_AXIDMA:
        max_chans = (cfg->has_mm2s ? 1 : 0) + (cfg->has_s2mm ? 1 : 0);
        if (max_chans == 0) {
            return  (-1);
        }
        hw_cfg   = &_axidma_hw_config;
        reg_size = XDMA_REG_MAP_SIZE;
        break;

    case DMA_IP_CDMA:
        max_chans = 1;
        hw_cfg    = &_cdma_hw_config;
        reg_size  = XDMA_REG_MAP_SIZE;
        break;

    case DMA_IP_VDMA:
        max_chans = (cfg->has_mm2s ? 1 : 0) + (cfg->has_s2mm ? 1 : 0);
        if (max_chans == 0) {
            return  (-1);
        }
        hw_cfg   = &_vdma_hw_config;
        reg_size = XDMA_REG_MAP_SIZE;
        break;

    case DMA_IP_MCDMA:
        if (cfg->n_mm2s < 0 || cfg->n_mm2s > XDMA_MCDMA_MAX_CHANS ||
            cfg->n_s2mm < 0 || cfg->n_s2mm > XDMA_MCDMA_MAX_CHANS) {
            return  (-1);
        }
        max_chans = cfg->n_mm2s + cfg->n_s2mm;
        if (max_chans == 0) {
            return  (-1);
        }
        hw_cfg   = &_mcdma_hw_config;
        reg_size = XDMA_MCDMA_REG_SIZE;
        break;

    default:
        return  (-1);
    }

    /*  第二层：公共初始化（寄存器映射、XDMA_DEVICE 分配、通道数组分配）  */
    ret = _xdma_probe_common(dev, cfg->base_addr, reg_size, max_chans, hw_cfg);
    if (ret) {
        return  (ret);
    }
    xdev = (XDMA_DEVICE *)dev->priv;

    /*  第三层：按 IP 类型和方向批量探测通道（xilinx_dma_child_probe）  */
    switch (cfg->ip_type) {

    case DMA_IP_AXIDMA:
        if (cfg->has_mm2s) {
            irq = cfg->irq_mm2s;
            ret = xilinx_dma_child_probe(xdev, dev, &chan_idx,
                                          DMA_DIR_MM2S, 1, &irq, LW_FALSE, 0);
            if (ret) {
                goto err;
            }
        }
        if (cfg->has_s2mm) {
            irq = cfg->irq_s2mm;
            ret = xilinx_dma_child_probe(xdev, dev, &chan_idx,
                                          DMA_DIR_S2MM, 1, &irq, LW_FALSE, 0);
            if (ret) {
                goto err;
            }
        }
        break;

    case DMA_IP_CDMA:
        irq = cfg->irq;
        ret = xilinx_dma_child_probe(xdev, dev, &chan_idx,
                                      DMA_DIR_MEM2MEM, 1, &irq, cfg->has_sg, 0);
        if (ret) {
            goto err;
        }
        break;

    case DMA_IP_VDMA:
        if (cfg->has_mm2s) {
            irq = cfg->irq_mm2s;
            ret = xilinx_dma_child_probe(xdev, dev, &chan_idx,
                                          DMA_DIR_MM2S, 1, &irq,
                                          LW_FALSE, cfg->max_frm_cnt);
            if (ret) {
                goto err;
            }
        }
        if (cfg->has_s2mm) {
            irq = cfg->irq_s2mm;
            ret = xilinx_dma_child_probe(xdev, dev, &chan_idx,
                                          DMA_DIR_S2MM, 1, &irq,
                                          LW_FALSE, cfg->max_frm_cnt);
            if (ret) {
                goto err;
            }
        }
        break;

    case DMA_IP_MCDMA:
        if (cfg->n_mm2s > 0) {
            ret = xilinx_dma_child_probe(xdev, dev, &chan_idx,
                                          DMA_DIR_MM2S, cfg->n_mm2s,
                                          cfg->irqs_mm2s, LW_TRUE, 0);
            if (ret) {
                goto err;
            }
        }
        if (cfg->n_s2mm > 0) {
            ret = xilinx_dma_child_probe(xdev, dev, &chan_idx,
                                          DMA_DIR_S2MM, cfg->n_s2mm,
                                          cfg->irqs_s2mm, LW_TRUE, 0);
            if (ret) {
                goto err;
            }
        }
        break;
    }

    dev->num_channels = chan_idx;
    ret = dma_device_register(dev);
    if (ret) {
        printk("xilinx_dma: %s device_register failed\n",
               (cfg->ip_type >= 0 && cfg->ip_type <= 3) ? ip_names[cfg->ip_type] : "?");
        goto err;
    }

    printk("xilinx_dma: %s '%s' @ 0x%08lX, %d chan(s)\n",
           (cfg->ip_type >= 0 && cfg->ip_type <= 3) ? ip_names[cfg->ip_type] : "?",
           dev->name, (ULONG)cfg->base_addr, chan_idx);
    return  (0);

err:
    for (int i = 0; i < chan_idx; i++) {
        API_InterVectorDisable(xdev->hw_chans[i].irq);
    }
    _xdma_remove_common(dev);
    return  (-1);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_remove
** 功能描述: Xilinx DMA 统一卸载入口（对标 Linux platform_driver.remove）。
**           遍历所有通道，禁用中断并调用各通道 free_resources 钩子完成停止+复位，
**           最后释放寄存器映射和 XDMA_DEVICE/通道数组。
** 输　入  : dev           dma_device 指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void xilinx_dma_remove (struct dma_device *dev)
{
    XDMA_DEVICE *xdev;
    int          i;

    if (!dev || !dev->priv) {
        return;
    }
    xdev = (XDMA_DEVICE *)dev->priv;

    for (i = 0; i < dev->num_channels; i++) {
        XDMA_CHAN *hw = &xdev->hw_chans[i];
        API_InterVectorDisable(hw->irq);
        if (hw->free_resources) {
            hw->free_resources(hw->core_chan);
        }
    }
    _xdma_remove_common(dev);
}

/*********************************************************************************************************
  END
**********************************************************************************************************/


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
** 文   件   名: dma_hw.c
**
** 创   建   人: AutoGen
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: AXI DMA 控制器驱动（Xilinx PG021）
**               实现 dma_ops 操作表，支持 Simple 和 Scatter-Gather 两种传输模式。
**               寄存器布局遵循 PG021 v7.1+。
**
** BUG
** 2026.03.23  初始版本。
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include "dma_types.h"
#include "dma_hw.h"
#include "dma_core.h"

/*********************************************************************************************************
  MM2S 通道寄存器偏移（相对 IP 基地址，PG021 Table 2-5）
*********************************************************************************************************/

#define MM2S_DMACR          0x00u                                       /*  DMA 控制寄存器              */
#define MM2S_DMASR          0x04u                                       /*  DMA 状态寄存器              */
#define MM2S_CURDESC        0x08u                                       /*  当前 BD 指针（SG）          */
#define MM2S_CURDESC_MSB    0x0Cu                                       /*  当前 BD 指针高32位          */
#define MM2S_TAILDESC       0x10u                                       /*  尾部 BD 指针（SG）          */
#define MM2S_TAILDESC_MSB   0x14u                                       /*  尾部 BD 指针高32位          */
#define MM2S_SA             0x18u                                       /*  源地址（Simple）            */
#define MM2S_SA_MSB         0x1Cu                                       /*  源地址高32位                */
#define MM2S_LENGTH         0x28u                                       /*  传输长度（写入触发）        */

/*********************************************************************************************************
  S2MM 通道寄存器偏移
*********************************************************************************************************/

#define S2MM_DMACR          0x30u
#define S2MM_DMASR          0x34u
#define S2MM_CURDESC        0x38u
#define S2MM_CURDESC_MSB    0x3Cu
#define S2MM_TAILDESC       0x40u
#define S2MM_TAILDESC_MSB   0x44u
#define S2MM_DA             0x48u                                       /*  目标地址（Simple）          */
#define S2MM_DA_MSB         0x4Cu
#define S2MM_LENGTH         0x58u                                       /*  缓冲区长度（写入触发）      */

/*********************************************************************************************************
  DMACR 位域定义
*********************************************************************************************************/

#define DMACR_RS            (1u << 0)                                   /*  Run/Stop：1=运行，0=停止    */
#define DMACR_RESET         (1u << 2)                                   /*  软复位（自清零）            */
#define DMACR_KEYHOLE       (1u << 3)                                   /*  Keyhole 读写                */
#define DMACR_CYCLIC        (1u << 4)                                   /*  循环 BD 模式                */
#define DMACR_IOC_IRQEN     (1u << 12)                                  /*  完成中断使能                */
#define DMACR_DLY_IRQEN     (1u << 13)                                  /*  延迟中断使能                */
#define DMACR_ERR_IRQEN     (1u << 14)                                  /*  错误中断使能                */

/*********************************************************************************************************
  DMASR 位域定义
*********************************************************************************************************/

#define DMASR_HALTED        (1u << 0)                                   /*  通道已停止                  */
#define DMASR_IDLE          (1u << 1)                                   /*  通道空闲（无待处理 BD）     */
#define DMASR_SGINCLD       (1u << 3)                                   /*  IP 包含 SG 功能             */
#define DMASR_DMAINTERR     (1u << 4)                                   /*  DMA 内部错误                */
#define DMASR_DMASLVERR     (1u << 5)                                   /*  DMA 从设备错误              */
#define DMASR_DMADECERR     (1u << 6)                                   /*  DMA 解码错误                */
#define DMASR_SGINTERR      (1u << 8)                                   /*  SG 内部错误                 */
#define DMASR_SGSLVERR      (1u << 9)                                   /*  SG 从设备错误               */
#define DMASR_SGDECERR      (1u << 10)                                  /*  SG 解码错误                 */
#define DMASR_IOC_IRQ       (1u << 12)                                  /*  完成中断挂起                */
#define DMASR_DLY_IRQ       (1u << 13)                                  /*  延迟中断挂起                */
#define DMASR_ERR_IRQ       (1u << 14)                                  /*  错误中断挂起                */
#define DMASR_ERR_MASK      (DMASR_DMAINTERR | DMASR_DMASLVERR | DMASR_DMADECERR | \
                             DMASR_SGINTERR  | DMASR_SGSLVERR  | DMASR_SGDECERR)

/*********************************************************************************************************
  BD 控制字位域（PG021 Table 2-12）
*********************************************************************************************************/

#define BD_CTRL_LEN_MASK    0x03FFFFFFu                                 /*  位[25:0]：缓冲区长度        */
#define BD_CTRL_EOF         (1u << 26)                                  /*  帧结束标志                  */
#define BD_CTRL_SOF         (1u << 27)                                  /*  帧起始标志                  */

/*********************************************************************************************************
  BD 状态字位域（由 DMA 硬件写入）
*********************************************************************************************************/

#define BD_STS_LEN_MASK     0x03FFFFFFu                                 /*  实际传输字节数              */
#define BD_STS_DMAINTERR    (1u << 28)
#define BD_STS_DMASLVERR    (1u << 29)
#define BD_STS_DMADECERR    (1u << 30)
#define BD_STS_CMPLT        (1u << 31)                                  /*  BD 处理完成                 */
#define BD_STS_ERR_MASK     (BD_STS_DMAINTERR | BD_STS_DMASLVERR | BD_STS_DMADECERR)

/*********************************************************************************************************
  杂项常量
*********************************************************************************************************/

#define AXI_DMA_BD_ALIGN        64u                                     /*  BD 数组须 64 字节对齐       */
#define AXI_DMA_REG_MAP_SIZE    0x1000u                                 /*  寄存器窗口大小（4 KB）      */
#define AXI_DMA_RESET_POLLS     10000                                   /*  软复位最大轮询次数          */
#define AXI_DMA_MAX_CHANS       2                                       /*  最多 2 个通道（MM2S+S2MM）  */

/*********************************************************************************************************
  AXI DMA Buffer Descriptor 硬件布局（packed，64 字节）
*********************************************************************************************************/

typedef struct axi_dma_bd {
    UINT32  next_desc;                                                  /*  0x00：下一 BD 物理地址      */
    UINT32  next_desc_msb;                                              /*  0x04：高32位（32位系统置0） */
    UINT32  buf_addr;                                                   /*  0x08：数据缓冲区物理地址    */
    UINT32  buf_addr_msb;                                               /*  0x0C                        */
    UINT32  reserved1;                                                  /*  0x10                        */
    UINT32  reserved2;                                                  /*  0x14                        */
    UINT32  control;                                                    /*  0x18：SOF | EOF | 长度      */
    UINT32  status;                                                     /*  0x1C：Cmplt | 错误 | 字节数 */
    UINT32  app[5];                                                     /*  0x20-0x33：用户应用字段     */
    UINT32  _pad[3];                                                    /*  0x34-0x3F：填充至 64 字节   */
} __attribute__((packed)) AXI_DMA_BD;

/*********************************************************************************************************
  硬件级描述符（存储于 dma_desc.hw_desc）
*********************************************************************************************************/

typedef struct axi_dma_desc {
    int         mode;                                                   /*  DMA_MODE_SIMPLE / SG        */

    UINT32      simple_addr;                                            /*  Simple 模式物理地址         */
    UINT32      simple_len;                                             /*  Simple 模式字节数           */

    AXI_DMA_BD *bd_virt;                                               /*  BD 数组虚拟地址             */
    UINT32      bd_phys;                                               /*  BD 数组物理地址（=虚拟）    */
    int         bd_num;                                                 /*  BD 数量                     */
} AXI_DMA_DESC;

/*********************************************************************************************************
  通道硬件私有数据（存储于 dma_chan.priv）
*********************************************************************************************************/

typedef struct axi_dma_chan {
    addr_t           regs;                                              /*  寄存器窗口虚拟基地址        */
    ULONG            irq;                                               /*  中断向量号                  */
    int              direction;                                         /*  DMA_DIR_MM2S / S2MM         */
    int              mode;                                              /*  DMA_MODE_SIMPLE / SG        */
    struct dma_chan *core_chan;                                          /*  指向核心层通道的反向指针    */
} AXI_DMA_CHAN;

/*********************************************************************************************************
  设备硬件私有数据（存储于 dma_device.priv）
*********************************************************************************************************/

typedef struct axi_dma_device {
    addr_t          regs_virt;                                          /*  寄存器虚拟基地址            */
    BOOL            has_sg;
    BOOL            has_mm2s;
    BOOL            has_s2mm;
    int             mode;
    int             max_sg_len;
    AXI_DMA_CHAN    hw_chans[AXI_DMA_MAX_CHANS];                        /*  通道硬件私有数组            */
} AXI_DMA_DEVICE;

/*********************************************************************************************************
  模块全局变量
*********************************************************************************************************/

static PLW_JOB_QUEUE  _G_defer_q = LW_NULL;                            /*  中断底半部延迟队列（CPU 0） */

/*********************************************************************************************************
  寄存器访问内联函数
  所有寄存器读写均经过 os_common.h 提供的 read32/write32，保证 ARM IO 屏障。
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: _chan_rd
** 功能描述: 读取通道寄存器（含读屏障）
** 输　入  : hw            通道硬件私有结构指针
**           off           寄存器偏移量
** 输　出  : 寄存器值
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static LW_INLINE UINT32 _chan_rd (AXI_DMA_CHAN *hw, UINT32 off)
{
    return  read32(hw->regs + off);
}
/*********************************************************************************************************
** 函数名称: _chan_wr
** 功能描述: 写入通道寄存器（含写屏障）
** 输　入  : hw            通道硬件私有结构指针
**           off           寄存器偏移量
**           val           写入值
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static LW_INLINE VOID _chan_wr (AXI_DMA_CHAN *hw, UINT32 off, UINT32 val)
{
    write32(val, hw->regs + off);
}
/*********************************************************************************************************
** 函数名称: _dmacr_off
** 功能描述: 根据通道方向返回 DMACR 寄存器偏移
** 输　入  : hw            通道硬件私有结构指针
** 输　出  : 寄存器偏移量
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static LW_INLINE UINT32 _dmacr_off (AXI_DMA_CHAN *hw)
{
    return  (hw->direction == DMA_DIR_MM2S) ? MM2S_DMACR : S2MM_DMACR;
}
/*********************************************************************************************************
** 函数名称: _dmasr_off
** 功能描述: 根据通道方向返回 DMASR 寄存器偏移
** 输　入  : hw            通道硬件私有结构指针
** 输　出  : 寄存器偏移量
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static LW_INLINE UINT32 _dmasr_off (AXI_DMA_CHAN *hw)
{
    return  (hw->direction == DMA_DIR_MM2S) ? MM2S_DMASR : S2MM_DMASR;
}

/*********************************************************************************************************
  内部辅助函数
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: _chan_reset
** 功能描述: 对 AXI DMA 通道执行软复位。
**           写 DMACR.RESET=1，轮询等待其自清零，验证通道进入 HALTED 状态。
**           PG021 要求在 SG 模式首次启动前执行复位以清除寄存器状态。
** 输　入  : hw            通道硬件私有结构指针
** 输　出  : 0 成功；-1 超时或复位后通道未进入停止状态
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _chan_reset (AXI_DMA_CHAN *hw)
{
    UINT32  cr_off = _dmacr_off(hw);
    UINT32  sr_off = _dmasr_off(hw);
    int     i;

    _chan_wr(hw, cr_off, DMACR_RESET);                                  /*  写入复位位                  */

    for (i = 0; i < AXI_DMA_RESET_POLLS; i++) {                        /*  等待复位完成（自清零）      */
        if (!(_chan_rd(hw, cr_off) & DMACR_RESET)) {
            break;
        }
    }

    if (i >= AXI_DMA_RESET_POLLS) {
        printk("axi_dma: reset timed out (%s)\n",
               hw->direction == DMA_DIR_MM2S ? "MM2S" : "S2MM");
        return  (-1);
    }

    if (!(_chan_rd(hw, sr_off) & DMASR_HALTED)) {                       /*  验证通道已停止              */
        printk("axi_dma: not halted after reset\n");
        return  (-1);
    }

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _chan_run
** 功能描述: 使能通道运行：置位 RS，同时使能完成中断与错误中断。
**           SG 模式下须在写入 CURDESC 之后、写入 TAILDESC 之前调用。
** 输　入  : hw            通道硬件私有结构指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _chan_run (AXI_DMA_CHAN *hw)
{
    UINT32 cr_off = _dmacr_off(hw);
    UINT32 val;

    val  = _chan_rd(hw, cr_off);
    val |= DMACR_RS | DMACR_IOC_IRQEN | DMACR_ERR_IRQEN;
    _chan_wr(hw, cr_off, val);
}
/*********************************************************************************************************
** 函数名称: _chan_stop_hw
** 功能描述: 清除 RS 位，停止通道运行
** 输　入  : hw            通道硬件私有结构指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _chan_stop_hw (AXI_DMA_CHAN *hw)
{
    UINT32 cr_off = _dmacr_off(hw);
    UINT32 val;

    val  = _chan_rd(hw, cr_off);
    val &= ~DMACR_RS;
    _chan_wr(hw, cr_off, val);
}
/*********************************************************************************************************
** 函数名称: _active_desc
** 功能描述: 获取通道 active_q 队头描述符（不加锁，调用者须持有 chan->lock）
** 输　入  : chan          核心层通道指针
** 输　出  : 描述符指针，队列为空时返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static struct dma_desc *_active_desc (struct dma_chan *chan)
{
    if (chan->active_q == LW_NULL) {
        return  (LW_NULL);
    }

    return  _LIST_ENTRY(chan->active_q, struct dma_desc, node);
}

/*********************************************************************************************************
  中断底半部处理函数
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: _dma_complete_bh
** 功能描述: 传输完成底半部处理（由 API_InterDeferJobAddEx 在 defer 线程中调用）。
**           ISR 仅读状态和投递本任务；free() 等耗时或需持锁的操作在此完成。
**           错误情形下先执行通道软复位，再通知核心层更新状态并触发回调。
** 输　入  : pvHw          AXI_DMA_CHAN 指针（用于复位）
**           pvChan        核心层 dma_chan 指针
**           pvDesc        已完成的 dma_desc 指针（status 字段已由 ISR 填写）
**           pv3~pv5       保留（API_InterDeferJobAddEx 固定 6 个参数）
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _dma_complete_bh (PVOID pvHw, PVOID pvChan, PVOID pvDesc,
                               PVOID pv3,  PVOID pv4,   PVOID pv5)
{
    AXI_DMA_CHAN    *hw   = (AXI_DMA_CHAN    *)pvHw;
    struct dma_chan  *chan = (struct dma_chan  *)pvChan;
    struct dma_desc  *desc = (struct dma_desc  *)pvDesc;
    int               status;

    if (!hw || !chan || !desc) {
        return;
    }

    status = desc->status;                                              /*  ISR 已写入，此处读取        */

    if (status == DMA_STATUS_ERROR) {
        _chan_reset(hw);                                                 /*  轮询复位移到底半部          */
    }

    dma_core_complete(chan, desc, status);                              /*  更新状态、回调、free、调度  */
}

/*********************************************************************************************************
  中断服务函数
*********************************************************************************************************/
#define IRQ_TYPE_LEVEL_HIGH             (0x00000004)
extern VOID  bspIntVectorTypeSet(ULONG  ulVector, INT  iType);
/*********************************************************************************************************
** 函数名称: _axi_dma_isr
** 功能描述: AXI DMA 通道中断处理核心逻辑。
**           读取并清除 DMASR 中的中断标志，判断传输结果，通知 DMA 核心层。
**           对于 SG 模式，还需验证末尾 BD 的 Cmplt 位。
** 输　入  : hw            触发中断的通道硬件私有结构指针
** 输　出  : LW_IRQ_HANDLED 或 LW_IRQ_NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static irqreturn_t _axi_dma_isr (AXI_DMA_CHAN *hw)
{
    UINT32           sr_off = _dmasr_off(hw);
    UINT32           sr;
    struct dma_chan  *core  = hw->core_chan;
    struct dma_desc  *desc;
    AXI_DMA_DESC     *adesc;
    int               status;

    sr = _chan_rd(hw, sr_off);
    if (!(sr & (DMASR_IOC_IRQ | DMASR_ERR_IRQ | DMASR_DLY_IRQ))) {
        return  (LW_IRQ_NONE);
    }

    _chan_wr(hw, sr_off, sr & (DMASR_IOC_IRQ | DMASR_ERR_IRQ | DMASR_DLY_IRQ));
                                                                        /*  写1清除中断标志             */
    if (core == LW_NULL) {
        return  (LW_IRQ_HANDLED);
    }

    desc = _active_desc(core);
    if (desc == LW_NULL) {
        return  (LW_IRQ_HANDLED);
    }
    adesc = (AXI_DMA_DESC *)desc->hw_desc;

    if (sr & DMASR_ERR_MASK) {                                          /*  硬件报告错误                */
        status = DMA_STATUS_ERROR;
        /*  _chan_reset 移至底半部，ISR 仅停止通道防止继续写内存        */
        _chan_stop_hw(hw);

    } else if (adesc && adesc->mode == DMA_MODE_SG) {                   /*  SG 模式验证末尾 BD          */
        AXI_DMA_BD *last = &adesc->bd_virt[adesc->bd_num - 1];
        if (last->status & BD_STS_ERR_MASK) {
            status = DMA_STATUS_ERROR;
            _chan_stop_hw(hw);
        } else if (last->status & BD_STS_CMPLT) {
            status = DMA_STATUS_COMPLETE;
        } else {
            return  (LW_IRQ_HANDLED);                                   /*  延迟中断，传输未结束        */
        }

    } else {
        status = DMA_STATUS_COMPLETE;                                   /*  Simple 模式：IOC 即完成     */
    }

    desc->status = status;                                              /*  提前写入，底半部使用        */

    API_InterDeferJobAddEx(_G_defer_q,                                  /*  投递底半部任务              */
                           (VOIDFUNCPTR)_dma_complete_bh,
                           (PVOID)hw,
                           (PVOID)core,
                           (PVOID)desc,
                           LW_NULL, LW_NULL, LW_NULL);

    return  (LW_IRQ_HANDLED);
}
/*********************************************************************************************************
** 函数名称: _axi_dma_mm2s_isr
** 功能描述: MM2S 通道 SylixOS 中断入口（符合 PINT_SVR_ROUTINE 签名）
** 输　入  : pvArg         中断注册时传入的 AXI_DMA_CHAN 指针
**           ulVector      中断向量号（未使用）
** 输　出  : IRQ 处理结果
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static irqreturn_t _axi_dma_mm2s_isr (PVOID pvArg, ULONG ulVector)
{
    return  _axi_dma_isr((AXI_DMA_CHAN *)pvArg);
}
/*********************************************************************************************************
** 函数名称: _axi_dma_s2mm_isr
** 功能描述: S2MM 通道 SylixOS 中断入口（符合 PINT_SVR_ROUTINE 签名）
** 输　入  : pvArg         中断注册时传入的 AXI_DMA_CHAN 指针
**           ulVector      中断向量号（未使用）
** 输　出  : IRQ 处理结果
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static irqreturn_t _axi_dma_s2mm_isr (PVOID pvArg, ULONG ulVector)
{
    return  _axi_dma_isr((AXI_DMA_CHAN *)pvArg);
}

/*********************************************************************************************************
  dma_ops 实现
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: _axi_chan_init
** 功能描述: 通道初始化：执行软复位，配置中断使能位，保持通道停止状态（RS=0）
** 输　入  : chan          核心层通道指针
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _axi_chan_init (struct dma_chan *chan)
{
    AXI_DMA_CHAN *hw = (AXI_DMA_CHAN *)chan->priv;
    int           ret;

    ret = _chan_reset(hw);
    if (ret) {
        return  (ret);
    }

    _chan_wr(hw, _dmacr_off(hw), DMACR_IOC_IRQEN | DMACR_ERR_IRQEN);   /*  使能中断，RS 保持 0        */

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axi_chan_start
** 功能描述: 启动通道（置位 RS=1，使能中断）
** 输　入  : chan          核心层通道指针
** 输　出  : 0
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _axi_chan_start (struct dma_chan *chan)
{
    _chan_run((AXI_DMA_CHAN *)chan->priv);

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axi_chan_stop
** 功能描述: 停止通道（清除 RS）
** 输　入  : chan          核心层通道指针
** 输　出  : 0
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _axi_chan_stop (struct dma_chan *chan)
{
    _chan_stop_hw((AXI_DMA_CHAN *)chan->priv);

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axi_prep_simple
** 功能描述: 准备 Simple 模式描述符。
**           将虚拟源/目标地址转换为物理地址保存到 adesc，实际触发（写 LENGTH）
**           延迟到 _axi_issue_pending() 中执行。
** 输　入  : desc          待填充的描述符指针
**           dst           目标虚拟地址（S2MM 通道有效）
**           src           源虚拟地址（MM2S 通道有效）
**           len           传输字节数
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _axi_prep_simple (struct dma_desc *desc, void *dst, void *src, size_t len)
{
    AXI_DMA_CHAN *hw    = (AXI_DMA_CHAN *)desc->chan->priv;
    AXI_DMA_DESC *adesc;
    phys_addr_t   phys  = 0;
    void         *vaddr;

    if (!len) {
        return  (-1);
    }

    adesc = (AXI_DMA_DESC *)lib_malloc(sizeof(AXI_DMA_DESC));
    if (!adesc) {
        return  (-1);
    }
    lib_memset(adesc, 0, sizeof(AXI_DMA_DESC));
    adesc->mode       = DMA_MODE_SIMPLE;
    adesc->simple_len = (UINT32)len;

    vaddr = (hw->direction == DMA_DIR_MM2S) ? src : dst;               /*  MM2S 读内存，S2MM 写内存    */

    if (API_VmmVirtualToPhysical((addr_t)vaddr, &phys) != ERROR_NONE) {
        printk("axi_dma: VirtualToPhysical failed for %p\n", vaddr);
        free(adesc);
        return  (-1);
    }
    adesc->simple_addr = (UINT32)phys;

    desc->hw_desc = adesc;

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axi_prep_sg
** 功能描述: 准备 SG 模式描述符。
**           从 DMA zone 分配 BD 数组（物理连续、无 Cache），填写 BD 链。
**           SylixOS DMA zone 采用平板映射，物理地址 == 虚拟地址。
** 输　入  : desc          待填充的描述符指针
**           sgl           分散聚集列表（虚拟地址）
**           sg_len        列表条目数
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _axi_prep_sg (struct dma_desc *desc, struct dma_sg *sgl, int sg_len)
{
    AXI_DMA_DESC *adesc;
    AXI_DMA_BD   *bd_virt;
    size_t        bd_bytes;
    UINT32        bd_phys;
    int           i;

    if (!sgl || sg_len <= 0) {
        return  (-1);
    }

    adesc = (AXI_DMA_DESC *)lib_malloc(sizeof(AXI_DMA_DESC));
    if (!adesc) {
        return  (-1);
    }
    lib_memset(adesc, 0, sizeof(AXI_DMA_DESC));
    adesc->mode   = DMA_MODE_SG;
    adesc->bd_num = sg_len;

    bd_bytes = (size_t)sg_len * sizeof(AXI_DMA_BD);
    bd_virt  = (AXI_DMA_BD *)API_VmmDmaAllocAlign(bd_bytes, AXI_DMA_BD_ALIGN);
    if (!bd_virt) {
        printk("axi_dma: BD alloc failed (%u bytes)\n", (UINT)bd_bytes);
        free(adesc);
        return  (-1);
    }
    lib_memset(bd_virt, 0, bd_bytes);

    bd_phys        = (UINT32)(addr_t)bd_virt;                          /*  DMA zone: 物理 == 虚拟      */
    adesc->bd_virt = bd_virt;
    adesc->bd_phys = bd_phys;

    for (i = 0; i < sg_len; i++) {                                      /*  填写 BD 链                  */
        phys_addr_t buf_phys = 0;
        UINT32      next_phys;

        if (API_VmmVirtualToPhysical((addr_t)sgl[i].buf, &buf_phys) != ERROR_NONE) {
            printk("axi_dma: SG[%d] VirtualToPhysical failed\n", i);
            API_VmmDmaFree(bd_virt);
            free(adesc);
            return  (-1);
        }

        next_phys = (i < sg_len - 1)                                   /*  末尾 BD 指向首 BD（循环）   */
                  ? (bd_phys + (UINT32)((i + 1) * (int)sizeof(AXI_DMA_BD)))
                  : bd_phys;

        bd_virt[i].next_desc     = next_phys;
        bd_virt[i].next_desc_msb = 0;
        bd_virt[i].buf_addr      = (UINT32)buf_phys;
        bd_virt[i].buf_addr_msb  = 0;
        bd_virt[i].control       = (UINT32)(sgl[i].len & BD_CTRL_LEN_MASK);

        if (i == 0)          bd_virt[i].control |= BD_CTRL_SOF;        /*  帧起始                      */
        if (i == sg_len - 1) bd_virt[i].control |= BD_CTRL_EOF;        /*  帧结束                      */

        bd_virt[i].status = 0;
    }

    desc->hw_desc = adesc;

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axi_issue_pending
** 功能描述: 触发硬件传输。
**           Simple 模式：RS=1 → 写 SA/DA → 写 LENGTH（触发）。
**           SG 模式：写 CURDESC（RS=0 时）→ RS=1 → 写 TAILDESC（触发）。
**           PG021 要求 CURDESC 必须在 RS=0 时写入，本函数严格遵循该时序。
** 输　入  : chan          核心层通道指针（active_q 头部为待启动描述符）
** 输　出  : 0 成功；-1 无活动描述符
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _axi_issue_pending (struct dma_chan *chan)
{
    AXI_DMA_CHAN    *hw    = (AXI_DMA_CHAN *)chan->priv;
    struct dma_desc *desc;
    AXI_DMA_DESC    *adesc;
    UINT32           tail_phys;

    desc = _active_desc(chan);
    if (!desc) {
        return  (0);
    }

    adesc = (AXI_DMA_DESC *)desc->hw_desc;
    if (!adesc) {
        return  (-1);
    }

    if (adesc->mode == DMA_MODE_SIMPLE) {
        _chan_run(hw);                                                   /*  1. RS=1                     */

        if (hw->direction == DMA_DIR_MM2S) {
            _chan_wr(hw, MM2S_SA,     adesc->simple_addr);
            _chan_wr(hw, MM2S_SA_MSB, 0);
            _chan_wr(hw, MM2S_LENGTH, adesc->simple_len);               /*  2. 写 LENGTH 触发           */
        } else {
            _chan_wr(hw, S2MM_DA,     adesc->simple_addr);
            _chan_wr(hw, S2MM_DA_MSB, 0);
            _chan_wr(hw, S2MM_LENGTH, adesc->simple_len);
        }

    } else {
        tail_phys = adesc->bd_phys +
                    (UINT32)((adesc->bd_num - 1) * (int)sizeof(AXI_DMA_BD));

        if (hw->direction == DMA_DIR_MM2S) {
            _chan_stop_hw(hw);                                           /*  PG021 要求 RS=0 才能写      */
            {   int n = AXI_DMA_RESET_POLLS;                           /*  CURDESC；等待通道停止       */
                UINT32 sr_off = _dmasr_off(hw);
                while (n-- > 0 && !(_chan_rd(hw, sr_off) & DMASR_HALTED)) {}
            }
            _chan_wr(hw, MM2S_CURDESC,     adesc->bd_phys);             /*  1. CURDESC（RS=0 时）       */
            _chan_wr(hw, MM2S_CURDESC_MSB, 0);
            _chan_run(hw);                                               /*  2. RS=1                     */
            _chan_wr(hw, MM2S_TAILDESC,     tail_phys);                 /*  3. TAILDESC 触发            */
            _chan_wr(hw, MM2S_TAILDESC_MSB, 0);
        } else {
            _chan_stop_hw(hw);
            {   int n = AXI_DMA_RESET_POLLS;
                UINT32 sr_off = _dmasr_off(hw);
                while (n-- > 0 && !(_chan_rd(hw, sr_off) & DMASR_HALTED)) {}
            }
            _chan_wr(hw, S2MM_CURDESC,     adesc->bd_phys);
            _chan_wr(hw, S2MM_CURDESC_MSB, 0);
            _chan_run(hw);
            _chan_wr(hw, S2MM_TAILDESC,     tail_phys);
            _chan_wr(hw, S2MM_TAILDESC_MSB, 0);
        }
    }

    return  (0);
}
/*********************************************************************************************************
** 函数名称: _axi_tx_status
** 功能描述: 查询描述符传输状态。
**           若描述符处于 ACTIVE 状态，则读取硬件寄存器（Simple 模式）
**           或末尾 BD 状态字（SG 模式）进行判断。
** 输　入  : chan          核心层通道指针
**           desc          待查询的描述符指针
** 输　出  : DMA_STATUS_* 状态码
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static int _axi_tx_status (struct dma_chan *chan, struct dma_desc *desc)
{
    AXI_DMA_CHAN *hw;
    AXI_DMA_DESC *adesc;
    UINT32        sr;

    if (!desc) {
        return  (DMA_STATUS_IDLE);
    }

    if (desc->status != DMA_STATUS_ACTIVE) {
        return  (desc->status);
    }

    hw    = (AXI_DMA_CHAN *)chan->priv;
    adesc = (AXI_DMA_DESC *)desc->hw_desc;
    sr    = _chan_rd(hw, _dmasr_off(hw));

    if (sr & DMASR_ERR_MASK) {
        return  (DMA_STATUS_ERROR);
    }

    if (adesc && adesc->mode == DMA_MODE_SG) {
        AXI_DMA_BD *last = &adesc->bd_virt[adesc->bd_num - 1];
        if (last->status & BD_STS_ERR_MASK) return  (DMA_STATUS_ERROR);
        if (last->status & BD_STS_CMPLT)    return  (DMA_STATUS_COMPLETE);
    } else {
        if (sr & DMASR_IDLE) return  (DMA_STATUS_COMPLETE);
    }

    return  (DMA_STATUS_ACTIVE);
}
/*********************************************************************************************************
** 函数名称: _axi_irq_handler
** 功能描述: ops->irq_handler 占位实现。
**           实际中断处理由 ISR 直接调用 dma_core_complete()，本函数保留扩展用。
** 输　入  : chan          核心层通道指针（未使用）
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _axi_irq_handler (struct dma_chan *chan)
{
    (VOID)chan;
}
/*********************************************************************************************************
** 函数名称: _axi_desc_free
** 功能描述: 释放描述符关联的硬件资源。
**           SG 模式：释放 BD 数组（DMA zone 内存）。
**           Simple 模式：仅释放 adesc 结构体。
** 输　入  : desc          待释放资源的描述符指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID _axi_desc_free (struct dma_desc *desc)
{
    AXI_DMA_DESC *adesc = (AXI_DMA_DESC *)desc->hw_desc;

    if (!adesc) {
        return;
    }

    if (adesc->mode == DMA_MODE_SG && adesc->bd_virt) {
        API_VmmDmaFree(adesc->bd_virt);                                 /*  释放 BD 数组                */
        adesc->bd_virt = LW_NULL;
    }

    free(adesc);
    desc->hw_desc = LW_NULL;
}

/*********************************************************************************************************
  共享操作表
*********************************************************************************************************/

struct dma_ops axi_dma_ops = {
    .chan_init      = _axi_chan_init,
    .chan_start     = _axi_chan_start,
    .chan_stop      = _axi_chan_stop,
    .prep_simple    = _axi_prep_simple,
    .prep_sg        = _axi_prep_sg,
    .issue_pending  = _axi_issue_pending,
    .tx_status      = _axi_tx_status,
    .irq_handler    = _axi_irq_handler,
    .desc_free      = _axi_desc_free,
};

/*********************************************************************************************************
  probe / remove
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: axi_dma_probe
** 功能描述: 初始化一个 AXI DMA 控制器实例。
**           映射寄存器、初始化通道、连接中断，向 DMA 核心注册设备。
** 输　入  : dev           已分配的 dma_device 结构（name 字段须已设置）
**           cfg           硬件配置参数
** 输　出  : 0 成功；-1 失败（已自行清理资源）
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int axi_dma_probe (struct dma_device *dev, const struct axi_dma_config *cfg)
{
    AXI_DMA_DEVICE *adev;
    PVOID           regs_virt;
    int             chan_idx = 0;
    int             ret;
    int             i;

    if (!dev || !cfg) {
        return  (-1);
    }

    if (_G_defer_q == LW_NULL) {
        _G_defer_q = API_InterDeferGet(0);                              /*  获取 CPU 0 底半部队列       */
    }

    adev = (AXI_DMA_DEVICE *)lib_malloc(sizeof(AXI_DMA_DEVICE));
    if (!adev) {
        return  (-1);
    }
    lib_memset(adev, 0, sizeof(AXI_DMA_DEVICE));
    adev->has_sg     = cfg->has_sg;
    adev->has_mm2s   = cfg->has_mm2s;
    adev->has_s2mm   = cfg->has_s2mm;
    adev->mode       = cfg->mode;
    adev->max_sg_len = cfg->max_sg_len > 0 ? cfg->max_sg_len : 16;

    regs_virt = API_VmmMallocAlign(AXI_DMA_REG_MAP_SIZE,                /*  分配虚拟地址窗口            */
                                   LW_CFG_VMM_PAGE_SIZE,
                                   LW_VMM_FLAG_DMA);
    if (!regs_virt) {
        printk("axi_dma: ioremap alloc failed\n");
        free(adev);
        return  (-1);
    }

    if (API_VmmMap(regs_virt,                                           /*  映射物理寄存器地址          */
                   (PVOID)(size_t)cfg->base_addr,
                   AXI_DMA_REG_MAP_SIZE,
                   LW_VMM_FLAG_DMA) != ERROR_NONE) {
        printk("axi_dma: VmmMap failed\n");
        API_VmmFree(regs_virt);
        free(adev);
        return  (-1);
    }
    adev->regs_virt = (addr_t)regs_virt;

    dev->channels = (struct dma_chan *)lib_malloc(                          /*  分配通道数组                */
                        AXI_DMA_MAX_CHANS * sizeof(struct dma_chan));
    if (!dev->channels) {
        API_VmmFree(regs_virt);
        free(adev);
        return  (-1);
    }
    lib_memset(dev->channels, 0, AXI_DMA_MAX_CHANS * sizeof(struct dma_chan));

    if (cfg->has_mm2s) {                                                /*  初始化 MM2S 通道            */
        AXI_DMA_CHAN   *hw = &adev->hw_chans[chan_idx];
        struct dma_chan *cc = &dev->channels[chan_idx];

        hw->regs      = adev->regs_virt;
        hw->irq       = cfg->irq_mm2s;
        hw->direction = DMA_DIR_MM2S;
        hw->mode      = cfg->mode;
        hw->core_chan = cc;

        cc->dev    = dev;
        cc->id     = chan_idx;
        cc->priv   = hw;
        cc->in_use = LW_FALSE;
        LW_SPIN_INIT(&cc->lock);

        ret = _axi_chan_init(cc);
        if (ret) {
            printk("axi_dma: MM2S init failed\n");
            goto  _err_free;
        }

        bspIntVectorTypeSet(cfg->irq_mm2s, IRQ_TYPE_LEVEL_HIGH);
        API_InterVectorConnect(cfg->irq_mm2s, _axi_dma_mm2s_isr,
                               (PVOID)hw, "axi_dma_mm2s");
        API_InterVectorEnable(cfg->irq_mm2s);

        chan_idx++;
    }

    if (cfg->has_s2mm) {                                                /*  初始化 S2MM 通道            */
        AXI_DMA_CHAN   *hw = &adev->hw_chans[chan_idx];
        struct dma_chan *cc = &dev->channels[chan_idx];

        hw->regs      = adev->regs_virt;
        hw->irq       = cfg->irq_s2mm;
        hw->direction = DMA_DIR_S2MM;
        hw->mode      = cfg->mode;
        hw->core_chan = cc;

        cc->dev    = dev;
        cc->id     = chan_idx;
        cc->priv   = hw;
        cc->in_use = LW_FALSE;
        LW_SPIN_INIT(&cc->lock);

        ret = _axi_chan_init(cc);
        if (ret) {
            printk("axi_dma: S2MM init failed\n");
            goto  _err_free;
        }

        bspIntVectorTypeSet(cfg->irq_s2mm, IRQ_TYPE_LEVEL_HIGH);
        API_InterVectorConnect(cfg->irq_s2mm, _axi_dma_s2mm_isr,
                               (PVOID)hw, "axi_dma_s2mm");
        API_InterVectorEnable(cfg->irq_s2mm);

        chan_idx++;
    }

    dev->num_channels = chan_idx;
    dev->ops          = &axi_dma_ops;
    dev->priv         = adev;

    ret = dma_device_register(dev);                                     /*  注册到 DMA 核心             */
    if (ret) {
        printk("axi_dma: device register failed\n");
        goto  _err_free;
    }

    printk("axi_dma: probed '%s' @ 0x%08lX, %d ch, %s mode\n",
           dev->name, (ULONG)cfg->base_addr, chan_idx,
           cfg->mode == DMA_MODE_SG ? "SG" : "Simple");
    return  (0);

_err_free:
    for (i = 0; i < chan_idx; i++) {
        API_InterVectorDisable(adev->hw_chans[i].irq);
    }
    API_VmmFree(regs_virt);
    free(dev->channels);
    dev->channels = LW_NULL;
    free(adev);
    return  (-1);
}
/*********************************************************************************************************
** 函数名称: axi_dma_remove
** 功能描述: 卸载一个 AXI DMA 控制器实例。
**           停止并复位所有通道，断开中断，释放所有资源，从核心注销设备。
** 输　入  : dev           dma_device 指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void axi_dma_remove (struct dma_device *dev)
{
    AXI_DMA_DEVICE *adev;
    int             i;

    if (!dev || !dev->priv) {
        return;
    }
    adev = (AXI_DMA_DEVICE *)dev->priv;

    dma_device_unregister(dev);

    for (i = 0; i < dev->num_channels; i++) {
        AXI_DMA_CHAN *hw = &adev->hw_chans[i];

        API_InterVectorDisable(hw->irq);
        _chan_stop_hw(hw);
        _chan_reset(hw);
    }

    API_VmmFree((PVOID)adev->regs_virt);

    free(dev->channels);
    dev->channels     = LW_NULL;
    dev->num_channels = 0;

    free(adev);
    dev->priv = LW_NULL;
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

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
** 文   件   名: xilinx.c
**
** 创   建   人: Assistant
**
** 文件创建日期: 2026 年 03 月 26 日
**
** 描        述: Xilinx DMA 驱动实现（阶段1：基础框架）
**
**               支持的 IP 类型：
**                 - AXI DMA    : 标准 DMA，2 通道（MM2S + S2MM）
**                 - AXI CDMA   : 内存到内存 DMA，1 通道
**                 - AXI VDMA   : 视频 DMA，2 通道，支持 stride
**                 - AXI MCDMA  : 多通道 DMA，最多 32 通道
**
**               阶段1实现内容：
**                 - 数据结构定义（所有 IP 类型）
**                 - 寄存器访问封装
**                 - Probe 机制（主 probe + child probe + chan probe）
**                 - Config 匹配机制
**                 - 注册到 DMAengine 框架
**
*********************************************************************************************************/
#define  __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../dmaengine.h"
#include "xilinx.h"

#define IRQ_TYPE_LEVEL_HIGH             (0x00000004)
extern VOID  bspIntVectorTypeSet(ULONG ulVector, INT iType);

/*********************************************************************************************************
  寄存器偏移定义
*********************************************************************************************************/
#define XILINX_DMA_MM2S_CTRL_OFFSET         0x0000
#define XILINX_DMA_S2MM_CTRL_OFFSET         0x0030
#define XILINX_VDMA_MM2S_DESC_OFFSET        0x0050
#define XILINX_VDMA_S2MM_DESC_OFFSET        0x00A0

#define XILINX_MCDMA_MM2S_CTRL_OFFSET       0x0000
#define XILINX_MCDMA_S2MM_CTRL_OFFSET       0x0500

/*********************************************************************************************************
  控制/状态寄存器偏移（相对于通道基地址）
*********************************************************************************************************/
#define XILINX_DMA_REG_DMACR                0x0000
#define XILINX_DMA_REG_DMASR                0x0004
#define XILINX_DMA_REG_CURDESC              0x0008
#define XILINX_DMA_REG_CURDESC_MSB          0x000C
#define XILINX_DMA_REG_TAILDESC             0x0010
#define XILINX_DMA_REG_TAILDESC_MSB         0x0014
#define XILINX_DMA_REG_SRCDSTADDR           0x0018
#define XILINX_DMA_REG_BTT                  0x0028

/*********************************************************************************************************
  DMACR 控制寄存器位定义
*********************************************************************************************************/
#define XILINX_DMA_DMACR_RUNSTOP            (1 << 0)
#define XILINX_DMA_DMACR_CIRC_EN            (1 << 1)
#define XILINX_DMA_DMACR_RESET              (1 << 2)
#define XILINX_DMA_DMACR_GENLOCK_EN         (1 << 3)
#define XILINX_DMA_DMACR_FRM_CNT_IRQ        (1 << 12)
#define XILINX_DMA_DMACR_DLY_CNT_IRQ        (1 << 13)
#define XILINX_DMA_DMACR_ERR_IRQ            (1 << 14)
#define XILINX_DMA_CR_CYCLIC_BD_EN_MASK     (1 << 4)

/*********************************************************************************************************
  DMASR 状态寄存器位定义
*********************************************************************************************************/
#define XILINX_DMA_DMASR_HALTED             (1 << 0)
#define XILINX_DMA_DMASR_IDLE               (1 << 1)
#define XILINX_DMA_DMASR_SG_MASK            (1 << 3)
#define XILINX_DMA_DMASR_DMA_INT_ERR        (1 << 4)
#define XILINX_DMA_DMASR_DMA_SLAVE_ERR      (1 << 5)
#define XILINX_DMA_DMASR_DMA_DEC_ERR        (1 << 6)
#define XILINX_DMA_DMASR_FRM_CNT_IRQ        (1 << 12)
#define XILINX_DMA_DMASR_DLY_CNT_IRQ        (1 << 13)
#define XILINX_DMA_DMASR_ERR_IRQ            (1 << 14)

#define XILINX_DMA_DMAXR_ALL_IRQ_MASK       \
    (XILINX_DMA_DMASR_FRM_CNT_IRQ | XILINX_DMA_DMASR_DLY_CNT_IRQ | XILINX_DMA_DMASR_ERR_IRQ)

#define XILINX_DMA_DMASR_ALL_ERR_MASK       \
    (XILINX_DMA_DMASR_DMA_INT_ERR | XILINX_DMA_DMASR_DMA_SLAVE_ERR | XILINX_DMA_DMASR_DMA_DEC_ERR)

#define XILINX_DMA_DMASR_ERR_RECOVER_MASK   \
    (XILINX_DMA_DMASR_DMA_INT_ERR)

/*********************************************************************************************************
  描述符控制字段位定义
*********************************************************************************************************/
#define XILINX_DMA_BD_SOP                   (1 << 27)
#define XILINX_DMA_BD_EOP                   (1 << 26)

/*********************************************************************************************************
  硬件限制
*********************************************************************************************************/
#define GENMASK(h, l) \
    (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (31 - (h))))

#define XILINX_DMA_LOOP_COUNT               1000000         /*  轮询超时计数（约1秒 @ 1MHz）*/
#define XILINX_DMA_MAX_CHANS_PER_DEVICE     2
#define XILINX_CDMA_MAX_CHANS_PER_DEVICE    1
#define XILINX_MCDMA_MAX_CHANS_PER_DEVICE   32

/*********************************************************************************************************
  中断合并配置
*********************************************************************************************************/
#define XILINX_DMA_COALESCE_MAX             255
#define XILINX_DMA_DELAY_MAX                255
#define XILINX_DMA_CR_COALESCE_MAX          GENMASK(23, 16)
#define XILINX_DMA_CR_COALESCE_SHIFT        16

/*********************************************************************************************************
  调试开关
*********************************************************************************************************/
// #define XILINX_DMA_DEBUG                    1

/*********************************************************************************************************
  IP 类型枚举
*********************************************************************************************************/
typedef enum {
    XDMA_TYPE_AXIDMA = 0,
    XDMA_TYPE_CDMA,
    XDMA_TYPE_VDMA,
    XDMA_TYPE_AXIMCDMA,
} xdma_ip_type_t;

/*********************************************************************************************************
  硬件描述符结构（阶段1：定义所有IP类型）
*********************************************************************************************************/

/*
 *  VDMA 硬件描述符（支持二维视频帧传输）
 */
typedef struct xilinx_vdma_desc_hw {
    UINT32  next_desc;
    UINT32  pad1;
    UINT32  buf_addr;
    UINT32  buf_addr_msb;
    UINT32  vsize;
    UINT32  hsize;
    UINT32  stride;
} __attribute__((aligned(64))) xilinx_vdma_desc_hw_t;

/*
 *  AXI DMA 硬件描述符（标准DMA）
 */
typedef struct xilinx_axidma_desc_hw {
    UINT32  next_desc;
    UINT32  next_desc_msb;
    UINT32  buf_addr;
    UINT32  buf_addr_msb;
    UINT32  reserved1;
    UINT32  reserved2;
    UINT32  control;
    UINT32  status;
    UINT32  app[5];
} __attribute__((aligned(64))) xilinx_axidma_desc_hw_t;

/*
 *  CDMA 硬件描述符（内存到内存）
 */
typedef struct xilinx_cdma_desc_hw {
    UINT32  next_desc;
    UINT32  next_desc_msb;
    UINT32  src_addr;
    UINT32  src_addr_msb;
    UINT32  dest_addr;
    UINT32  dest_addr_msb;
    UINT32  control;
    UINT32  status;
} __attribute__((aligned(64))) xilinx_cdma_desc_hw_t;

/*
 *  MCDMA 硬件描述符（多通道DMA）
 */
typedef struct xilinx_aximcdma_desc_hw {
    UINT32  next_desc;
    UINT32  next_desc_msb;
    UINT32  buf_addr;
    UINT32  buf_addr_msb;
    UINT32  rsvd;
    UINT32  control;
    UINT32  status;
    UINT32  sideband_status;
    UINT32  app[5];
} __attribute__((aligned(64))) xilinx_aximcdma_desc_hw_t;

/*********************************************************************************************************
  TX 段结构（软件描述符，包含硬件描述符 + 链表节点 + 物理地址）
*********************************************************************************************************/

typedef struct xilinx_axidma_tx_segment {
    xilinx_axidma_desc_hw_t  hw;
    LW_LIST_LINE             node;
    phys_addr_t              phys;
} __attribute__((aligned(64))) xilinx_axidma_tx_segment_t;

typedef struct xilinx_cdma_tx_segment {
    xilinx_cdma_desc_hw_t    hw;
    LW_LIST_LINE             node;
    phys_addr_t              phys;
} __attribute__((aligned(64))) xilinx_cdma_tx_segment_t;

typedef struct xilinx_vdma_tx_segment {
    xilinx_vdma_desc_hw_t    hw;
    LW_LIST_LINE             node;
    phys_addr_t              phys;
} __attribute__((aligned(64))) xilinx_vdma_tx_segment_t;

/*********************************************************************************************************
  TX 描述符（软件管理的传输描述符）
*********************************************************************************************************/
typedef struct xilinx_dma_tx_descriptor {
    dma_async_tx_descriptor_t  async_tx;
    phys_addr_t                phys;                                    /*  链表头段的物理地址          */
    phys_addr_t                tail_phys;                               /*  链表尾段的物理地址          */
    PLW_LIST_LINE              segments;
    BOOL                       cyclic;
    BOOL                       err;
    UINT32                     residue;
} xilinx_dma_tx_descriptor_t;

/*********************************************************************************************************
  前向声明
*********************************************************************************************************/
struct xilinx_dma_device;
struct xilinx_dma_chan;

static dma_cookie_t xilinx_dma_tx_submit (dma_async_tx_descriptor_t *tx);
static INT xilinx_dma_reset (struct xilinx_dma_chan *chan);

/*********************************************************************************************************
** 函数名称: _list_line_get_last_node
** 功能描述: 获取链表最后一个节点
** 输　入  : head      - 链表头指针
** 输　出  : 最后一个节点指针；head 为 NULL 时返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static LW_INLINE PLW_LIST_LINE _list_line_get_last_node (PLW_LIST_LINE head)
{
    PLW_LIST_LINE pline = head;

    if (!pline) {
        return LW_NULL;
    }
    while (_list_line_get_next(pline)) {
        pline = _list_line_get_next(pline);
    }
    return pline;
}

/*********************************************************************************************************
  IP 配置结构（每种 IP 类型对应一个配置）
*********************************************************************************************************/
typedef struct xilinx_dma_config {
    xdma_ip_type_t  dmatype;
    INT             max_channels;
} xilinx_dma_config_t;

/*********************************************************************************************************
  通道结构
*********************************************************************************************************/
typedef struct xilinx_dma_chan {
    dma_chan_t                      common;
    struct xilinx_dma_device       *xdev;
    UINT32                          ctrl_offset;
    spinlock_t                      lock;
    PLW_LIST_LINE                   pending_list;
    PLW_LIST_LINE                   active_list;
    PLW_LIST_LINE                   done_list;
    PLW_LIST_LINE                   free_seg_list;
    xilinx_axidma_tx_segment_t     *cyclic_seg_v;
    INT                             id;
    dma_transfer_direction_t        direction;
    BOOL                            has_sg;
    BOOL                            cyclic;
    BOOL                            err;
    BOOL                            idle;
    BOOL                            terminating;
    INT                             irq;
    dma_slave_config_t              slave_cfg;
    PLW_JOB_QUEUE                   defer_job;
    UINT32                          desc_pendingcount;
    xilinx_axidma_tx_segment_t     *seg_v;                              /*  DMA 段内存虚拟起始地址      */
    phys_addr_t                     seg_p;                              /*  DMA 段内存物理起始地址      */
    VOID                            (*start_transfer)(struct xilinx_dma_chan *chan);
    INT                             (*stop_transfer)(struct xilinx_dma_chan *chan);
} xilinx_dma_chan_t;

/*********************************************************************************************************
  设备结构
*********************************************************************************************************/
typedef struct xilinx_dma_device {
    dma_device_t                    common;
    PVOID                           regs;
    const xilinx_dma_config_t      *dma_config;
    xilinx_dma_chan_t              *chan[XILINX_MCDMA_MAX_CHANS_PER_DEVICE];
    BOOL                            ext_addr;
    UINT32                          max_buffer_len;
} xilinx_dma_device_t;

/*********************************************************************************************************
  IP 配置表
*********************************************************************************************************/
static const xilinx_dma_config_t axidma_config = {
    .dmatype      = XDMA_TYPE_AXIDMA,
    .max_channels = XILINX_DMA_MAX_CHANS_PER_DEVICE,
};

static const xilinx_dma_config_t axicdma_config = {
    .dmatype      = XDMA_TYPE_CDMA,
    .max_channels = XILINX_CDMA_MAX_CHANS_PER_DEVICE,
};

static const xilinx_dma_config_t axivdma_config = {
    .dmatype      = XDMA_TYPE_VDMA,
    .max_channels = XILINX_DMA_MAX_CHANS_PER_DEVICE,
};

static const xilinx_dma_config_t aximcdma_config = {
    .dmatype      = XDMA_TYPE_AXIMCDMA,
    .max_channels = XILINX_MCDMA_MAX_CHANS_PER_DEVICE,
};

/*********************************************************************************************************
** 函数名称: dma_read
** 功能描述: 读取 DMA 通道寄存器
** 输　入  : chan      - 通道指针
**           reg       - 寄存器偏移
** 输　出  : 寄存器值
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static inline UINT32 dma_read (xilinx_dma_chan_t *chan, UINT32 reg)
{
    return read32((addr_t)chan->xdev->regs + chan->ctrl_offset + reg);
}
/*********************************************************************************************************
** 函数名称: dma_write
** 功能描述: 写入 DMA 通道寄存器
** 输　入  : chan      - 通道指针
**           reg       - 寄存器偏移
**           val       - 写入值
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static inline VOID dma_write (xilinx_dma_chan_t *chan, UINT32 reg, UINT32 val)
{
    write32(val, (addr_t)chan->xdev->regs + chan->ctrl_offset + reg);
}
/*********************************************************************************************************
** 函数名称: xilinx_match_config
** 功能描述: 根据 compatible 字符串匹配 IP 配置
** 输　入  : compatible - 设备兼容字符串
** 输　出  : IP 配置指针；LW_NULL 表示不支持
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static const xilinx_dma_config_t *xilinx_match_config (CPCHAR compatible)
{
    if (lib_strcmp(compatible, "xlnx,axi-dma-1.00.a") == 0) {
        return &axidma_config;
    }
    if (lib_strcmp(compatible, "xlnx,axi-cdma-1.00.a") == 0) {
        return &axicdma_config;
    }
    if (lib_strcmp(compatible, "xlnx,axi-vdma-1.00.a") == 0) {
        return &axivdma_config;
    }
    if (lib_strcmp(compatible, "xlnx,axi-mcdma-1.00.a") == 0) {
        return &aximcdma_config;
    }
    return LW_NULL;
}

/*********************************************************************************************************
  描述符内存管理
*********************************************************************************************************/
#define XILINX_DMA_NUM_DESCS                255
#define XILINX_DMA_NUM_APP_WORDS            5

/*********************************************************************************************************
** 函数名称: xilinx_dma_alloc_tx_descriptor
** 功能描述: 分配传输描述符
** 输　入  : chan      - 通道指针
** 输　出  : 描述符指针；LW_NULL 表示失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static xilinx_dma_tx_descriptor_t *xilinx_dma_alloc_tx_descriptor (xilinx_dma_chan_t *chan)
{
    xilinx_dma_tx_descriptor_t *desc;

    desc = (xilinx_dma_tx_descriptor_t *)sys_malloc(sizeof(xilinx_dma_tx_descriptor_t));
    if (!desc) {
        return LW_NULL;
    }
    lib_bzero(desc, sizeof(xilinx_dma_tx_descriptor_t));
    desc->segments = LW_NULL;
    return desc;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_set_coalesce
** 功能描述: 配置中断合并参数
** 输　入  : chan      - 通道指针
**           frame_cnt - 帧计数阈值（0 禁用）
**           delay_cnt - 延迟计数阈值（0 禁用）
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID xilinx_dma_set_coalesce (xilinx_dma_chan_t *chan, UINT32 frame_cnt, UINT32 delay_cnt)
{
    UINT32 reg;

    reg = dma_read(chan, XILINX_DMA_REG_DMACR);
    reg &= ~((XILINX_DMA_COALESCE_MAX << 16) | (XILINX_DMA_DELAY_MAX << 24));

    if (frame_cnt > 0) {
        reg |= XILINX_DMA_DMACR_FRM_CNT_IRQ;
        reg |= (frame_cnt & XILINX_DMA_COALESCE_MAX) << 16;
    }

    if (delay_cnt > 0) {
        reg |= XILINX_DMA_DMACR_DLY_CNT_IRQ;
        reg |= (delay_cnt & XILINX_DMA_DELAY_MAX) << 24;
    }

    dma_write(chan, XILINX_DMA_REG_DMACR, reg);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_free_tx_descriptor
** 功能描述: 释放传输描述符并归还段到空闲链表
** 输　入  : chan      - 通道指针
**           desc      - 描述符指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID xilinx_dma_free_tx_descriptor (xilinx_dma_chan_t *chan, xilinx_dma_tx_descriptor_t *desc)
{
    xilinx_axidma_tx_segment_t *segment;
    PLW_LIST_LINE pline;

    if (!desc) {
        return;
    }

    pline = desc->segments;
    while (pline) {
        segment = _LIST_ENTRY(pline, xilinx_axidma_tx_segment_t, node);
        pline = _list_line_get_next(pline);
        _List_Line_Add_Tail(&segment->node, &chan->free_seg_list);
    }

    sys_free(desc);
}

/*********************************************************************************************************
** 函数名称: xilinx_axidma_alloc_tx_segment
** 功能描述: 从空闲链表分配传输段
** 输　入  : chan      - 通道指针
** 输　出  : 段指针；LW_NULL 表示无可用段
** 全局变量:
** 调用模块:
** 注意    : 函数内部加锁保护 free_seg_list
*********************************************************************************************************/
static xilinx_axidma_tx_segment_t *xilinx_axidma_alloc_tx_segment (xilinx_dma_chan_t *chan)
{
    xilinx_axidma_tx_segment_t *segment;
    INTREG ireg;

    LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);
    if (!chan->free_seg_list) {
        LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);
        return LW_NULL;
    }
    segment = _LIST_ENTRY(chan->free_seg_list, xilinx_axidma_tx_segment_t, node);
    _List_Line_Del(&segment->node, &chan->free_seg_list);
    LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);

    lib_bzero(&segment->hw, sizeof(segment->hw));

#if XILINX_DMA_DEBUG
    printk("[xilinx_dma] alloc_seg: vaddr=%p phys=0x%x next_after_bzero=0x%x\n",
           segment, (UINT32)segment->phys, segment->hw.next_desc);
#endif

    return segment;
}

/*********************************************************************************************************
  DMAengine ops 实现
*********************************************************************************************************/

/*********************************************************************************************************
** 函数名称: xilinx_dma_alloc_chan_resources
** 功能描述: 分配通道资源（预分配描述符段）
** 输　入  : dchan     - DMA 通道指针
** 输　出  : 分配的描述符数量；-1 表示失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static INT xilinx_dma_alloc_chan_resources (dma_chan_t *dchan)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);
    xilinx_axidma_tx_segment_t *seg_v;
    phys_addr_t seg_p;
    INT i;

    if (chan->free_seg_list) {
        return XILINX_DMA_NUM_DESCS;
    }

    if (chan->xdev->dma_config->dmatype != XDMA_TYPE_AXIDMA) {
        printk("[xilinx_dma] alloc_chan_resources: only AXIDMA supported in phase 2\n");
        return -1;
    }

    seg_v = (xilinx_axidma_tx_segment_t *)API_VmmDmaAlloc(
        sizeof(xilinx_axidma_tx_segment_t) * XILINX_DMA_NUM_DESCS);
    if (!seg_v) {
        printk("[xilinx_dma] failed to allocate descriptor memory\n");
        return -1;
    }

    API_VmmVirtualToPhysical((addr_t)seg_v, (phys_addr_t *)&seg_p);

    chan->cyclic_seg_v = (xilinx_axidma_tx_segment_t *)API_VmmDmaAlloc(
        sizeof(xilinx_axidma_tx_segment_t));
    if (!chan->cyclic_seg_v) {
        printk("[xilinx_dma] failed to allocate cyclic segment\n");
        API_VmmDmaFree(seg_v);
        return -1;
    }
    {
        phys_addr_t cyc_p;
        API_VmmVirtualToPhysical((addr_t)chan->cyclic_seg_v, (phys_addr_t *)&cyc_p);
        chan->cyclic_seg_v->phys = cyc_p;
    }

    chan->seg_v         = seg_v;
    chan->seg_p         = seg_p;
    chan->pending_list  = LW_NULL;
    chan->active_list   = LW_NULL;
    chan->done_list     = LW_NULL;
    chan->free_seg_list = LW_NULL;
    chan->idle          = LW_TRUE;

    for (i = 0; i < XILINX_DMA_NUM_DESCS; i++) {
        seg_v[i].phys = seg_p + (phys_addr_t)i * sizeof(xilinx_axidma_tx_segment_t);
        /*  next_desc 不预设，由 prep 函数在构建链时填写  */
        _LIST_LINE_INIT_IN_CODE(seg_v[i].node);
        _List_Line_Add_Tail(&seg_v[i].node, &chan->free_seg_list);
    }

#if XILINX_DMA_DEBUG
    printk("[xilinx_dma] alloc_chan_resources: seg_v=%p seg_p=0x%x stride=%d\n",
           seg_v, (UINT32)seg_p, (INT)sizeof(xilinx_axidma_tx_segment_t));
    printk("[xilinx_dma]   seg[0]: vaddr=%p phys=0x%x\n", &seg_v[0], (UINT32)seg_v[0].phys);
    printk("[xilinx_dma]   seg[1]: vaddr=%p phys=0x%x\n", &seg_v[1], (UINT32)seg_v[1].phys);
#endif

    dma_cookie_init(dchan);

    if (chan->xdev->dma_config->dmatype == XDMA_TYPE_AXIDMA) {
        UINT32 reg = dma_read(chan, XILINX_DMA_REG_DMACR);
        reg |= XILINX_DMA_DMACR_ERR_IRQ | XILINX_DMA_DMACR_DLY_CNT_IRQ | XILINX_DMA_DMACR_FRM_CNT_IRQ;
        dma_write(chan, XILINX_DMA_REG_DMACR, reg);
    }

    printk("[xilinx_dma] channel '%s' resources allocated (%d segments)\n",
           dchan->chan_name, XILINX_DMA_NUM_DESCS);
    return XILINX_DMA_NUM_DESCS;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_free_chan_resources
** 功能描述: 释放通道资源（清空队列并释放描述符内存）
** 输　入  : dchan     - DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: xilinx_dma_free_descriptors
** 功能描述: 释放通道所有描述符
** 输　入  : chan      - 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID xilinx_dma_free_descriptors (xilinx_dma_chan_t *chan)
{
    xilinx_dma_tx_descriptor_t *desc;
    PLW_LIST_LINE pline;
    INTREG ireg;

    LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);

    while (chan->pending_list) {
        pline = chan->pending_list;
        desc = _LIST_ENTRY(pline, xilinx_dma_tx_descriptor_t, async_tx.node);
        _List_Line_Del(pline, &chan->pending_list);
        xilinx_dma_free_tx_descriptor(chan, desc);
    }

    while (chan->active_list) {
        pline = chan->active_list;
        desc = _LIST_ENTRY(pline, xilinx_dma_tx_descriptor_t, async_tx.node);
        _List_Line_Del(pline, &chan->active_list);
        xilinx_dma_free_tx_descriptor(chan, desc);
    }

    while (chan->done_list) {
        pline = chan->done_list;
        desc = _LIST_ENTRY(pline, xilinx_dma_tx_descriptor_t, async_tx.node);
        _List_Line_Del(pline, &chan->done_list);
        xilinx_dma_free_tx_descriptor(chan, desc);
    }

    LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_free_chan_resources
** 功能描述: 释放通道资源（清空队列并释放描述符内存）
** 输　入  : dchan     - DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID xilinx_dma_free_chan_resources (dma_chan_t *dchan)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);
    INTREG ireg;

    printk("[xilinx_dma] Free all channel resources.\n");

    xilinx_dma_free_descriptors(chan);

    if (chan->xdev->dma_config->dmatype == XDMA_TYPE_AXIDMA) {
        LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);
        chan->free_seg_list = LW_NULL;
        LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);

        if (chan->seg_v) {
            API_VmmDmaFree(chan->seg_v);
            chan->seg_v = LW_NULL;
        }

        if (chan->cyclic_seg_v) {
            API_VmmDmaFree(chan->cyclic_seg_v);
            chan->cyclic_seg_v = LW_NULL;
        }
    }

    printk("[xilinx_dma] channel '%s' resources freed\n", dchan->chan_name);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_start_transfer
** 功能描述: 启动 DMA 传输
** 输　入  : chan      - 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
** 注意    : 调用者必须已持有 chan->lock
*********************************************************************************************************/
static VOID xilinx_dma_start_transfer (xilinx_dma_chan_t *chan)
{
    xilinx_dma_tx_descriptor_t *head_desc, *tail_desc;
    xilinx_axidma_tx_segment_t *head_segment, *tail_segment;
    UINT32 reg;

    if (chan->err) {
        return;
    }

    if (!chan->pending_list) {
        return;
    }

    if (!chan->idle) {
        return;
    }

    head_desc = _LIST_ENTRY(chan->pending_list, xilinx_dma_tx_descriptor_t, async_tx.node);
    tail_desc = _LIST_ENTRY(_list_line_get_last_node(chan->pending_list),
                            xilinx_dma_tx_descriptor_t, async_tx.node);
    head_segment = _LIST_ENTRY(head_desc->segments, xilinx_axidma_tx_segment_t, node);
    tail_segment = (xilinx_axidma_tx_segment_t *)(addr_t)tail_desc->tail_phys;

    /*  根据 pending 描述符数量动态配置中断合并  */
    if (chan->desc_pendingcount > 0) {
        xilinx_dma_set_coalesce(chan, chan->desc_pendingcount, chan->desc_pendingcount);
    }

#if XILINX_DMA_DEBUG
    printk("[xilinx_dma] start_transfer ch%d: head_phys=0x%x tail_phys=0x%x\n",
           chan->id, (UINT32)head_segment->phys, (UINT32)tail_segment->phys);
    printk("[xilinx_dma]   head_seg: buf=0x%x ctrl=0x%x next=0x%x\n",
           head_segment->hw.buf_addr, head_segment->hw.control, head_segment->hw.next_desc);
    printk("[xilinx_dma]   tail_seg: buf=0x%x ctrl=0x%x next=0x%x\n",
           tail_segment->hw.buf_addr, tail_segment->hw.control, tail_segment->hw.next_desc);
    reg = dma_read(chan, XILINX_DMA_REG_DMASR);
    printk("[xilinx_dma]   DMASR before start: 0x%x (idle=%d halted=%d)\n",
           reg, !!(reg & XILINX_DMA_DMASR_IDLE), !!(reg & XILINX_DMA_DMASR_HALTED));
#endif

    if (chan->has_sg) {
        /*
         *  AXI DMA PG021: CURDESC 只能在 DMACR.RS=0（通道停止）时写入。
         *  上一次传输完成后通道处于 IDLE 但仍 RUNNING（RS=1）状态；
         *  此时写 CURDESC 会被硬件静默忽略，导致 CURDESC=0，触发 DMA_INT_ERR。
         *  因此，写 CURDESC 前须先停止通道，等待 Halted=1。
         */
        reg = dma_read(chan, XILINX_DMA_REG_DMASR);
        if (!(reg & XILINX_DMA_DMASR_HALTED)) {
            if (chan->stop_transfer) {
                chan->stop_transfer(chan);
            }
        }
        dma_write(chan, XILINX_DMA_REG_CURDESC, (UINT32)(head_desc->phys & 0xFFFFFFFF));
        if (chan->xdev->ext_addr) {
            dma_write(chan, XILINX_DMA_REG_CURDESC_MSB, (UINT32)((UINT64)head_desc->phys >> 32));
        }
    }

    reg = dma_read(chan, XILINX_DMA_REG_DMACR);
    reg |= XILINX_DMA_DMACR_RUNSTOP;
    dma_write(chan, XILINX_DMA_REG_DMACR, reg);

    if (chan->err) {
        return;
    }

    /*  内存屏障：确保描述符写入对硬件可见  */
    KN_SMP_WMB();

    if (chan->has_sg) {
        if (chan->cyclic) {
            dma_write(chan, XILINX_DMA_REG_TAILDESC, (UINT32)(chan->cyclic_seg_v->phys & 0xFFFFFFFF));
            if (chan->xdev->ext_addr) {
                dma_write(chan, XILINX_DMA_REG_TAILDESC_MSB,
                          (UINT32)((UINT64)chan->cyclic_seg_v->phys >> 32));
            }
        } else {
            dma_write(chan, XILINX_DMA_REG_TAILDESC, (UINT32)(tail_segment->phys & 0xFFFFFFFF));
            if (chan->xdev->ext_addr) {
                dma_write(chan, XILINX_DMA_REG_TAILDESC_MSB,
                          (UINT32)((UINT64)tail_segment->phys >> 32));
            }
        }
    } else {
        xilinx_axidma_tx_segment_t *segment;
        xilinx_axidma_desc_hw_t *hw;

        segment = _LIST_ENTRY(head_desc->segments, xilinx_axidma_tx_segment_t, node);
        hw = &segment->hw;

        dma_write(chan, XILINX_DMA_REG_SRCDSTADDR, hw->buf_addr);
        if (chan->xdev->ext_addr) {
            dma_write(chan, XILINX_DMA_REG_SRCDSTADDR + 4, hw->buf_addr_msb);
        }

        dma_write(chan, XILINX_DMA_REG_BTT, hw->control & chan->xdev->max_buffer_len);
    }

    while (chan->pending_list) {
        PLW_LIST_LINE pline = chan->pending_list;
        _List_Line_Del(pline, &chan->pending_list);
        _List_Line_Add_Tail(pline, &chan->active_list);
    }
    chan->desc_pendingcount = 0;
    chan->idle = LW_FALSE;

#if XILINX_DMA_DEBUG
    reg = dma_read(chan, XILINX_DMA_REG_DMASR);
    printk("[xilinx_dma]   DMASR after start: 0x%x\n", reg);
    printk("[xilinx_dma]   CURDESC=0x%x TAILDESC=0x%x\n",
           dma_read(chan, XILINX_DMA_REG_CURDESC),
           dma_read(chan, XILINX_DMA_REG_TAILDESC));
#endif
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_reset
** 功能描述: 复位 DMA 通道
** 输　入  : chan      - 通道指针
** 输　出  : 0 成功；-1 超时
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static INT xilinx_dma_reset (xilinx_dma_chan_t *chan)
{
    UINT32 reg;
    INT timeout = XILINX_DMA_LOOP_COUNT;

    dma_write(chan, XILINX_DMA_REG_DMACR, XILINX_DMA_DMACR_RESET);

    /*  等待复位完成  */
    while (timeout--) {
        reg = dma_read(chan, XILINX_DMA_REG_DMACR);
        if (!(reg & XILINX_DMA_DMACR_RESET)) {
            return 0;
        }
    }

    printk("[xilinx_dma] reset timeout\n");
    return -1;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_stop_transfer
** 功能描述: 停止 DMA 传输
** 输　入  : chan      - 通道指针
** 输　出  : 0 成功；-1 超时
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static INT xilinx_dma_stop_transfer (xilinx_dma_chan_t *chan)
{
    UINT32 reg;
    INT timeout = XILINX_DMA_LOOP_COUNT;

    reg = dma_read(chan, XILINX_DMA_REG_DMACR);
    reg &= ~XILINX_DMA_DMACR_RUNSTOP;
    dma_write(chan, XILINX_DMA_REG_DMACR, reg);

    while (timeout--) {
        reg = dma_read(chan, XILINX_DMA_REG_DMASR);
        if (reg & XILINX_DMA_DMASR_HALTED) {
            chan->idle = LW_TRUE;
            return 0;
        }
    }

    printk("[xilinx_dma] stop timeout, cr %x, sr %x\n",
           dma_read(chan, XILINX_DMA_REG_DMACR),
           dma_read(chan, XILINX_DMA_REG_DMASR));
    return -1;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_issue_pending
** 功能描述: 触发待处理传输
** 输　入  : dchan     - DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID xilinx_dma_issue_pending (dma_chan_t *dchan)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);
    INTREG ireg;

    LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);
    if (chan->start_transfer) {
        chan->start_transfer(chan);
    }
    LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_tx_status
** 功能描述: 查询传输状态
** 输　入  : dchan     - DMA 通道指针
**           cookie    - 传输 cookie
**           txstate   - 状态输出指针
** 输　出  : 传输状态
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static dma_status_t xilinx_dma_tx_status (dma_chan_t *dchan, dma_cookie_t cookie, dma_tx_state_t *txstate)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);
    xilinx_dma_tx_descriptor_t *desc;
    dma_status_t ret;
    UINT32 residue = 0;
    INTREG ireg;

    ret = dma_cookie_status(dchan, cookie, txstate);
    if (ret == DMA_COMPLETE || !txstate) {
        return ret;
    }

    LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);

    /*  计算 residue：遍历 active 队列中匹配 cookie 的描述符  */
    PLW_LIST_LINE pline = chan->active_list;
    while (pline) {
        desc = _LIST_ENTRY(pline, xilinx_dma_tx_descriptor_t, async_tx.node);
        if (desc->async_tx.cookie == cookie) {
            /*  累加所有段的长度  */
            PLW_LIST_LINE seg_line = desc->segments;
            while (seg_line) {
                xilinx_axidma_tx_segment_t *seg = _LIST_ENTRY(seg_line, xilinx_axidma_tx_segment_t, node);
                residue += seg->hw.control & chan->xdev->max_buffer_len;
                seg_line = _list_line_get_next(seg_line);
            }
            break;
        }
        pline = _list_line_get_next(pline);
    }

    LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);

    txstate->residue = residue;
    return ret;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_slave_config
** 功能描述: 配置 Slave 通道参数
** 输　入  : dchan     - DMA 通道指针
**           config    - 配置参数指针
** 输　出  : 0 成功；-1 失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static INT xilinx_dma_slave_config (dma_chan_t *dchan, dma_slave_config_t *config)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);

    if (!config) {
        return -1;
    }

    if (config->direction != DMA_MEM_TO_DEV && config->direction != DMA_DEV_TO_MEM) {
        return -1;
    }

    chan->slave_cfg = *config;
    printk("[xilinx_dma] channel '%s' configured\n", dchan->chan_name);
    return 0;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_terminate_all
** 功能描述: 终止所有传输并清空队列
** 输　入  : dchan     - DMA 通道指针
** 输　出  : 0 成功
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static INT xilinx_dma_terminate_all (dma_chan_t *dchan)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);
    UINT32 reg;
    INT err;

    if (!chan->cyclic && chan->stop_transfer) {
        err = chan->stop_transfer(chan);
        if (err) {
            printk("[xilinx_dma] Cannot stop channel %p: %x\n",
                   chan, dma_read(chan, XILINX_DMA_REG_DMASR));
            chan->err = LW_TRUE;
        }
    }

    chan->terminating = LW_TRUE;
    xilinx_dma_free_descriptors(chan);
    chan->idle = LW_TRUE;

    if (chan->cyclic) {
        reg = dma_read(chan, XILINX_DMA_REG_DMACR);
        reg &= ~XILINX_DMA_CR_CYCLIC_BD_EN_MASK;
        dma_write(chan, XILINX_DMA_REG_DMACR, reg);
        chan->cyclic = LW_FALSE;
    }

    return 0;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_prep_slave_sg
** 功能描述: 准备 Slave SG 传输
** 输　入  : dchan     - DMA 通道指针
**           sgl       - SG 条目数组
**           sg_len    - SG 条目数量
**           direction - 传输方向
**           flags     - 标志位
**           context   - 上下文指针
** 输　出  : 描述符指针；LW_NULL 表示失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static dma_async_tx_descriptor_t *xilinx_dma_prep_slave_sg (
    dma_chan_t *dchan, dma_sg_entry_t *sgl, UINT sg_len,
    dma_transfer_direction_t direction, ULONG flags, PVOID context)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);
    xilinx_dma_tx_descriptor_t *desc;
    xilinx_axidma_tx_segment_t *segment, *prev = LW_NULL, *first = LW_NULL;
    UINT32 *app_w = (UINT32 *)context;
    phys_addr_t buf_addr;
    UINT i;

    if (direction != DMA_MEM_TO_DEV && direction != DMA_DEV_TO_MEM) {
        return LW_NULL;
    }

    if (chan->xdev->dma_config->dmatype != XDMA_TYPE_AXIDMA) {
        return LW_NULL;
    }

    if (!sgl || sg_len == 0) {
        return LW_NULL;
    }

    desc = xilinx_dma_alloc_tx_descriptor(chan);
    if (!desc) {
        return LW_NULL;
    }

    for (i = 0; i < sg_len; i++) {
        segment = xilinx_axidma_alloc_tx_segment(chan);
        if (!segment) {
            xilinx_dma_free_tx_descriptor(chan, desc);
            return LW_NULL;
        }

        /*  虚拟地址转物理地址  */
        buf_addr = sgl[i].addr;
        API_VmmVirtualToPhysical((addr_t)buf_addr, &buf_addr);

        segment->hw.buf_addr = (UINT32)(buf_addr & 0xFFFFFFFF);
        if (chan->xdev->ext_addr) {
            segment->hw.buf_addr_msb = (UINT32)((UINT64)buf_addr >> 32);
        }

        segment->hw.control = sgl[i].len & chan->xdev->max_buffer_len;

        if (chan->direction == DMA_MEM_TO_DEV && app_w) {
            lib_memcpy(segment->hw.app, app_w, sizeof(UINT32) * XILINX_DMA_NUM_APP_WORDS);
        }

        /*  记录第一个段，用于设置 SOP 和 desc->phys  */
        if (!first) {
            first = segment;
        }

        /*  将当前段链接到前一个段的 next_desc  */
        if (prev) {
            prev->hw.next_desc = (UINT32)(segment->phys & 0xFFFFFFFF);
            if (chan->xdev->ext_addr) {
                prev->hw.next_desc_msb = (UINT32)((UINT64)segment->phys >> 32);
            }
            API_CacheClear(DATA_CACHE, prev, sizeof(xilinx_axidma_tx_segment_t));
        }

        _List_Line_Add_Tail(&segment->node, &desc->segments);
        prev = segment;
    }

    /*  prev 现在是最后一个段，设置 EOP，next_desc 保持 0  */
    prev->hw.control  |= XILINX_DMA_BD_EOP;
    prev->hw.next_desc     = 0;
    prev->hw.next_desc_msb = 0;
    API_CacheClear(DATA_CACHE, prev, sizeof(xilinx_axidma_tx_segment_t));

    /*  第一个段设置 SOP  */
    first->hw.control |= XILINX_DMA_BD_SOP;
    API_CacheClear(DATA_CACHE, first, sizeof(xilinx_axidma_tx_segment_t));

    desc->phys      = first->phys;
    desc->tail_phys = prev->phys;

    desc->async_tx.chan = dchan;
    desc->async_tx.flags = flags;
    desc->async_tx.tx_submit = xilinx_dma_tx_submit;

#if XILINX_DMA_DEBUG
    {
        xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);
        xilinx_axidma_tx_segment_t *first = _LIST_ENTRY(desc->segments, xilinx_axidma_tx_segment_t, node);
        printk("[xilinx_dma] prep_slave_sg ch%d: sg_len=%d dir=%d\n",
               chan->id, sg_len, direction);
        printk("[xilinx_dma]   first_seg: buf=0x%x len=%d ctrl=0x%x\n",
               first->hw.buf_addr, sgl[0].len, first->hw.control);
    }
#endif

    return &desc->async_tx;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_prep_dma_memcpy
** 功能描述: 准备内存拷贝传输（CDMA）
** 输　入  : dchan     - DMA 通道指针
**           dst       - 目标地址
**           src       - 源地址
**           len       - 传输长度
**           flags     - 标志位
** 输　出  : 描述符指针；LW_NULL 表示失败
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static dma_async_tx_descriptor_t *xilinx_dma_prep_dma_memcpy (
    dma_chan_t *dchan, phys_addr_t dst, phys_addr_t src, size_t len, ULONG flags)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);
    xilinx_dma_tx_descriptor_t *desc;
    xilinx_axidma_tx_segment_t *segment;

    if (!len || len > chan->xdev->max_buffer_len) {
        return LW_NULL;
    }

    desc = xilinx_dma_alloc_tx_descriptor(chan);
    if (!desc) {
        return LW_NULL;
    }

    segment = xilinx_axidma_alloc_tx_segment(chan);
    if (!segment) {
        xilinx_dma_free_tx_descriptor(chan, desc);
        return LW_NULL;
    }

    /*  虚拟地址转物理地址  */
    API_VmmVirtualToPhysical((addr_t)src, &src);


    segment->hw.buf_addr = (UINT32)(src & 0xFFFFFFFF);
    if (chan->xdev->ext_addr) {
        segment->hw.buf_addr_msb = (UINT32)((UINT64)src >> 32);
    }

    segment->hw.control = (len & chan->xdev->max_buffer_len) | XILINX_DMA_BD_SOP | XILINX_DMA_BD_EOP;
    segment->hw.next_desc = 0;
    segment->hw.next_desc_msb = 0;

    API_CacheClear(DATA_CACHE, segment, sizeof(xilinx_axidma_tx_segment_t));

    _List_Line_Add_Tail(&segment->node, &desc->segments);

    desc->phys      = segment->phys;
    desc->tail_phys = segment->phys;
    desc->async_tx.chan = dchan;
    desc->async_tx.flags = flags;
    desc->async_tx.tx_submit = xilinx_dma_tx_submit;
    return &desc->async_tx;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_tx_submit
** 功能描述: 提交传输描述符
** 输　入  : tx        - 传输描述符指针
** 输　出  : cookie 值
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static dma_cookie_t xilinx_dma_tx_submit (dma_async_tx_descriptor_t *tx)
{
    xilinx_dma_tx_descriptor_t *desc = _LIST_ENTRY(tx, xilinx_dma_tx_descriptor_t, async_tx);
    xilinx_dma_chan_t *chan = _LIST_ENTRY(tx->chan, xilinx_dma_chan_t, common);
    dma_cookie_t cookie;
    INTREG ireg;

    LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);
    cookie = dma_cookie_assign(tx);
    _List_Line_Add_Tail(&tx->node, &chan->pending_list);
    chan->desc_pendingcount++;
    chan->terminating = LW_FALSE;
    LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);

    return cookie;
}

/*********************************************************************************************************
  中断处理
*********************************************************************************************************/

/*********************************************************************************************************
** 函数名称: xilinx_dma_complete_descriptor
** 功能描述: 将 active 描述符移到 done 队列
** 输　入  : chan      - 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID xilinx_dma_complete_descriptor (xilinx_dma_chan_t *chan)
{
    xilinx_dma_tx_descriptor_t *desc;

    if (!chan->active_list) {
        return;
    }

    while (chan->active_list) {
        desc = _LIST_ENTRY(chan->active_list, xilinx_dma_tx_descriptor_t, async_tx.node);
        desc->err = chan->err;
        _List_Line_Del(&desc->async_tx.node, &chan->active_list);
        if (!desc->cyclic) {
            dma_cookie_complete(&desc->async_tx);
        }
        _List_Line_Add_Tail(&desc->async_tx.node, &chan->done_list);
    }
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_chan_desc_cleanup
** 功能描述: 清理已完成的描述符并调用回调
** 输　入  : chan      - 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID xilinx_dma_chan_desc_cleanup (xilinx_dma_chan_t *chan)
{
    xilinx_dma_tx_descriptor_t *desc;
    dma_async_tx_callback_result cb;
    PVOID cb_param;
    dmaengine_result_t result;
    INTREG ireg;

    LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);

    while (chan->done_list) {
        desc = _LIST_ENTRY(chan->done_list, xilinx_dma_tx_descriptor_t, async_tx.node);
        _List_Line_Del(&desc->async_tx.node, &chan->done_list);

        cb = desc->async_tx.callback_result;
        cb_param = desc->async_tx.callback_param;

        LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);

        if (cb) {
            result.result  = desc->err ? DMA_ERROR : DMA_COMPLETE;
            result.residue = desc->residue;
            cb(cb_param, &result);
        }

        LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);
        xilinx_dma_free_tx_descriptor(chan, desc);

        if (chan->terminating)
            break;
    }

    LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_do_tasklet
** 功能描述: 中断底半部处理
** 输　入  : arg       - 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static VOID xilinx_dma_do_tasklet (PVOID arg)
{
    xilinx_dma_chan_t *chan = (xilinx_dma_chan_t *)arg;
    INTREG ireg;

    xilinx_dma_chan_desc_cleanup(chan);

    /*  如果有 pending 且通道空闲，启动新传输  */
    LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);
    if (chan->idle && chan->pending_list && chan->start_transfer && !chan->err) {
        chan->start_transfer(chan);
    }
    LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_irq_handler
** 功能描述: DMA 中断处理函数
** 输　入  : data      - 通道指针
**           vector    - 中断向量
** 输　出  : LW_IRQ_HANDLED 或 LW_IRQ_NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static irqreturn_t xilinx_dma_irq_handler (PVOID data, ULONG vector)
{
    xilinx_dma_chan_t *chan = (xilinx_dma_chan_t *)data;
    UINT32 status;

    status = dma_read(chan, XILINX_DMA_REG_DMASR);

#if XILINX_DMA_DEBUG
    printk("[xilinx_dma] IRQ ch%d: DMASR=0x%x\n", chan->id, status);
#endif

    if (!(status & XILINX_DMA_DMAXR_ALL_IRQ_MASK)) {
        return LW_IRQ_NONE;
    }

    /*  清除中断标志  */
    dma_write(chan, XILINX_DMA_REG_DMASR, status & XILINX_DMA_DMAXR_ALL_IRQ_MASK);

    if (status & XILINX_DMA_DMASR_ERR_IRQ) {
        UINT32 errors = status & XILINX_DMA_DMASR_ALL_ERR_MASK;

        dma_write(chan, XILINX_DMA_REG_DMASR, errors & XILINX_DMA_DMASR_ERR_RECOVER_MASK);

        if (errors & ~XILINX_DMA_DMASR_ERR_RECOVER_MASK) {
            printk("[xilinx_dma] Channel %p has errors %x, cdr %x tdr %x\n",
                   chan, errors,
                   dma_read(chan, XILINX_DMA_REG_CURDESC),
                   dma_read(chan, XILINX_DMA_REG_TAILDESC));
            chan->err = LW_TRUE;

            /*  停止传输并标记所有 active 描述符为错误  */
            if (chan->stop_transfer) {
                chan->stop_transfer(chan);
            }
        }
    }

    if (status & XILINX_DMA_DMASR_DLY_CNT_IRQ) {
        /* Device takes too long */
    }

    if (status & XILINX_DMA_DMASR_FRM_CNT_IRQ) {
        INTREG ireg;
        LW_SPIN_LOCK_IRQ(&chan->lock, &ireg);
        xilinx_dma_complete_descriptor(chan);
        chan->idle = LW_TRUE;
        LW_SPIN_UNLOCK_IRQ(&chan->lock, ireg);
    }

    API_InterDeferJobAdd(chan->defer_job, xilinx_dma_do_tasklet, chan);

    return LW_IRQ_HANDLED;
}

/*********************************************************************************************************
** 功能描述: 通道 probe（创建并初始化单个通道）
** 输　入  : xdev      - 设备指针
**           child_idx - 子节点索引
**           chan_id   - 通道 ID
**           irq       - 中断号
**           direction - 传输方向
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/
static INT xilinx_dma_chan_probe (xilinx_dma_device_t *xdev, INT child_idx, INT chan_id,
                                   INT irq, dma_transfer_direction_t direction)
{
    xilinx_dma_chan_t *chan;
    UINT32 ctrl_offset;

    chan = (xilinx_dma_chan_t *)sys_malloc(sizeof(xilinx_dma_chan_t));
    if (!chan) {
        printk("[xilinx_dma] failed to allocate channel\n");
        return -1;
    }
    lib_bzero(chan, sizeof(xilinx_dma_chan_t));

    if (direction == DMA_MEM_TO_DEV) {
        ctrl_offset = (xdev->dma_config->dmatype == XDMA_TYPE_AXIMCDMA) ?
                      XILINX_MCDMA_MM2S_CTRL_OFFSET : XILINX_DMA_MM2S_CTRL_OFFSET;
    } else {
        ctrl_offset = (xdev->dma_config->dmatype == XDMA_TYPE_AXIMCDMA) ?
                      XILINX_MCDMA_S2MM_CTRL_OFFSET : XILINX_DMA_S2MM_CTRL_OFFSET;
    }

    chan->xdev        = xdev;
    chan->id          = chan_id;
    chan->ctrl_offset = ctrl_offset;
    chan->direction   = direction;
    chan->irq         = irq;
    chan->has_sg      = LW_TRUE;
    chan->cyclic      = LW_FALSE;
    chan->err         = LW_FALSE;
    chan->idle        = LW_TRUE;
    chan->terminating = LW_FALSE;

    LW_SPIN_INIT(&chan->lock);
    chan->pending_list   = LW_NULL;
    chan->active_list    = LW_NULL;
    chan->done_list      = LW_NULL;
    chan->free_seg_list  = LW_NULL;

    lib_bzero(&chan->slave_cfg, sizeof(dma_slave_config_t));

    if (xdev->dma_config->dmatype == XDMA_TYPE_AXIDMA) {
        chan->start_transfer = xilinx_dma_start_transfer;
        chan->stop_transfer  = xilinx_dma_stop_transfer;

        /*  复位通道  */
        if (xilinx_dma_reset(chan) != 0) {
            printk("[xilinx_dma] failed to reset channel %d\n", chan_id);
            sys_free(chan);
            return -1;
        }
    } else {
        /*  TODO: 实现 CDMA/VDMA/MCDMA 的 start_transfer 和 stop_transfer  */
        chan->start_transfer = LW_NULL;
        chan->stop_transfer  = LW_NULL;
    }

    chan->defer_job = API_InterDeferGet(0);

    if (irq > 0) {
        bspIntVectorTypeSet(irq, IRQ_TYPE_LEVEL_HIGH);
        API_InterVectorConnect(irq, xilinx_dma_irq_handler, chan, "xilinx-dma");
        API_InterVectorEnable(irq);
    }

    chan->common.device = &xdev->common;
    snprintf(chan->common.chan_name, sizeof(chan->common.chan_name),
             "%s-ch%d", xdev->common.dev_name, chan_id);

    _LIST_LINE_INIT_IN_CODE(chan->common.node);
    _List_Line_Add_Tail(&chan->common.node, &xdev->common.channels);

    xdev->chan[chan_id] = chan;

    printk("[xilinx_dma] channel %d probed (dir=%d, irq=%d)\n", chan_id, direction, irq);
    return 0;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_child_probe
** 功能描述: 子节点 probe（处理 MM2S 或 S2MM 子节点）
** 输　入  : xdev       - 设备指针
**           child_idx  - 子节点索引（0 或 1）
**           params     - 板级参数
**           chan_id    - 当前通道ID（传入传出参数）
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/
static INT xilinx_dma_child_probe (xilinx_dma_device_t *xdev, INT child_idx,
                                    const xilinx_board_params_t *params, INT *chan_id)
{
    dma_transfer_direction_t direction;
    UINT32 nr_channels;
    UINT32 i;

    if (!params->child[child_idx].compatible) {
        return 0;
    }

    if (lib_strstr(params->child[child_idx].compatible, "mm2s")) {
        direction = DMA_MEM_TO_DEV;
    } else if (lib_strstr(params->child[child_idx].compatible, "s2mm")) {
        direction = DMA_DEV_TO_MEM;
    } else {
        direction = DMA_MEM_TO_MEM;
    }

    nr_channels = params->child[child_idx].dma_channels ?
                  params->child[child_idx].dma_channels : 1;

    for (i = 0; i < nr_channels; i++) {
        if (xilinx_dma_chan_probe(xdev, child_idx, *chan_id,
                                   params->child[child_idx].irq, direction) != 0) {
            return -1;
        }
        (*chan_id)++;
    }

    return 0;
}

/*================================================ 两阶段初始化入口 ======================================*/

static BOOL  _G_xilinx_dma_lib_inited = LW_FALSE;

/*********************************************************************************************************
** 函数名称: xilinx_dma_lib_init
** 功能描述: 阶段1 — 驱动库初始化，注册驱动能力
** 输　入  : NONE
** 输　出  : 0 成功
** 说明    : 只执行一次；后续 xilinx_dma_params_register 可调用多次注册不同硬件实例
*********************************************************************************************************/

INT  xilinx_dma_lib_init (VOID)
{
    if (_G_xilinx_dma_lib_inited) {
        return  (0);
    }
    _G_xilinx_dma_lib_inited = LW_TRUE;
    printk("[xilinx_dma] library initialized (axidma/cdma/vdma/mcdma)\n");
    return  (0);
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_params_register
** 功能描述: 阶段2 — 绑定板级硬件实例，分配设备并注册到框架
** 输　入  : params — 板级参数指针（来自 dma.c 的静态定义）
** 输　出  : 0 成功；-1 失败
*********************************************************************************************************/

INT  xilinx_dma_params_register (const xilinx_board_params_t *params)
{
    xilinx_dma_device_t       *xdev;
    const xilinx_dma_config_t *config;
    INT                        i;
    INT                        chan_id = 0;

    if (!_G_xilinx_dma_lib_inited) {
        printk("[xilinx_dma] params_register: library not initialized,"
               " call xilinx_dma_lib_init first\n");
        return  (-1);
    }

    config = xilinx_match_config(params->compatible);
    if (!config) {
        printk("[xilinx_dma] params_register: unsupported compatible: %s\n",
               params->compatible);
        return  (-1);
    }

    xdev = (xilinx_dma_device_t *)sys_malloc(sizeof(xilinx_dma_device_t));
    if (!xdev) {
        printk("[xilinx_dma] params_register: OOM\n");
        return  (-1);
    }
    lib_bzero(xdev, sizeof(xilinx_dma_device_t));

    API_VmmMap((PVOID)params->reg_base, (PVOID)params->reg_base,
                            params->reg_size, LW_VMM_FLAG_DMA);
    xdev->regs = params->reg_base;
    if (!xdev->regs) {
        printk("[xilinx_dma] params_register: failed to map registers\n");
        sys_free(xdev);
        return  (-1);
    }

    xdev->dma_config     = config;
    xdev->ext_addr       = (params->xlnx_addrwidth > 32);
    xdev->max_buffer_len = params->max_buffer_len;

    lib_strlcpy(xdev->common.dev_name, params->dev_name, sizeof(xdev->common.dev_name));
    xdev->common.channels = LW_NULL;
    xdev->common.chancnt  = 0;
    LW_SPIN_INIT(&xdev->common.lock);

    xdev->common.device_alloc_chan_resources = xilinx_dma_alloc_chan_resources;
    xdev->common.device_free_chan_resources  = xilinx_dma_free_chan_resources;
    xdev->common.device_prep_slave_sg        = xilinx_dma_prep_slave_sg;
    xdev->common.device_prep_dma_memcpy      = xilinx_dma_prep_dma_memcpy;
    xdev->common.device_issue_pending        = xilinx_dma_issue_pending;
    xdev->common.device_tx_status            = xilinx_dma_tx_status;
    xdev->common.device_config               = xilinx_dma_slave_config;
    xdev->common.device_terminate_all        = xilinx_dma_terminate_all;

    for (i = 0; i < 2; i++) {
        if (xilinx_dma_child_probe(xdev, i, params, &chan_id) != 0) {
            sys_free(xdev);
            return  (-1);
        }
    }

    xdev->common.chancnt = chan_id;

    printk("[xilinx_dma] params_register OK: type=%d, channels=%d\n",
           config->dmatype, xdev->common.chancnt);

    return  dma_async_device_register(&xdev->common);
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

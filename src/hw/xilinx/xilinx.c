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

/*********************************************************************************************************
  描述符控制字段位定义
*********************************************************************************************************/
#define XILINX_DMA_BD_SOP                   (1 << 27)
#define XILINX_DMA_BD_EOP                   (1 << 26)

/*********************************************************************************************************
  硬件限制
*********************************************************************************************************/
#define XILINX_DMA_MAX_CHANS_PER_DEVICE     2
#define XILINX_CDMA_MAX_CHANS_PER_DEVICE    1
#define XILINX_MCDMA_MAX_CHANS_PER_DEVICE   32

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
    INT                             id;
    dma_transfer_direction_t        direction;
    BOOL                            has_sg;
    BOOL                            cyclic;
    BOOL                            err;
    BOOL                            idle;
    INT                             irq;
    dma_slave_config_t              slave_cfg;
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
  寄存器访问封装
*********************************************************************************************************/
static inline UINT32 dma_read (xilinx_dma_chan_t *chan, UINT32 reg)
{
    return read32((addr_t)chan->xdev->regs + chan->ctrl_offset + reg);
}

static inline VOID dma_write (xilinx_dma_chan_t *chan, UINT32 reg, UINT32 val)
{
    write32(val, (addr_t)chan->xdev->regs + chan->ctrl_offset + reg);
}

/*********************************************************************************************************
  Config 匹配函数
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
  DMAengine ops 实现（阶段1：最小桩函数）
*********************************************************************************************************/
static INT xilinx_dma_alloc_chan_resources (dma_chan_t *dchan)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);

    chan->pending_list = LW_NULL;
    chan->active_list  = LW_NULL;
    chan->done_list    = LW_NULL;
    chan->idle         = LW_TRUE;
    dma_cookie_init(dchan);

    printk("[xilinx_dma] channel '%s' resources allocated\n", dchan->chan_name);
    return 0;
}

static VOID xilinx_dma_free_chan_resources (dma_chan_t *dchan)
{
    printk("[xilinx_dma] channel '%s' resources freed\n", dchan->chan_name);
}

static VOID xilinx_dma_issue_pending (dma_chan_t *dchan)
{
    printk("[xilinx_dma] issue_pending called on '%s'\n", dchan->chan_name);
}

static dma_status_t xilinx_dma_tx_status (dma_chan_t *dchan, dma_cookie_t cookie, dma_tx_state_t *txstate)
{
    return dma_cookie_status(dchan, cookie, txstate);
}

static INT xilinx_dma_slave_config (dma_chan_t *dchan, dma_slave_config_t *config)
{
    xilinx_dma_chan_t *chan = _LIST_ENTRY(dchan, xilinx_dma_chan_t, common);

    if (!config) {
        return -1;
    }

    chan->slave_cfg = *config;
    printk("[xilinx_dma] channel '%s' configured\n", dchan->chan_name);
    return 0;
}

static INT xilinx_dma_terminate_all (dma_chan_t *dchan)
{
    printk("[xilinx_dma] terminate_all called on '%s'\n", dchan->chan_name);
    return 0;
}

/*********************************************************************************************************
** 函数名称: xilinx_dma_chan_probe
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

    LW_SPIN_INIT(&chan->lock);
    chan->pending_list   = LW_NULL;
    chan->active_list    = LW_NULL;
    chan->done_list      = LW_NULL;
    chan->free_seg_list  = LW_NULL;

    lib_bzero(&chan->slave_cfg, sizeof(dma_slave_config_t));

    chan->start_transfer = LW_NULL;
    chan->stop_transfer  = LW_NULL;

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

    xdev->regs = (void *)API_VmmMap((PVOID)params->reg_base, (PVOID)params->reg_base,
                            params->reg_size, LW_VMM_FLAG_DMA);
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

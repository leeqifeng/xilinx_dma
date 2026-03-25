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
** 文   件   名: demoip.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 25 日
**
** 描        述: Demo IP DMA 驱动（纯软件模拟，无真实硬件依赖）
**
**               支持能力：
**                 - DMA_MEMCPY   : device_prep_dma_memcpy（内存到内存）
**                 - DMA_SLAVE SG : device_prep_slave_sg  （外设 ↔ 内存，散布-聚集）
**
**               模拟原理（Slave SG）：
**                 - device_config 保存 dma_slave_config_t 到通道，
**                   其中 src_addr / dst_addr 为用户传入的"设备缓冲区"地址（软件可访问）。
**                 - MEM_TO_DEV : 将各 SG 条目按序 memcpy 到 dev_addr（追加写入，模拟 FIFO）。
**                 - DEV_TO_MEM : 将 dev_addr 数据按序 memcpy 到各 SG 条目。
**                   dev_addr 每次前进 sg[i].len，使设备侧视为连续线性缓冲区。
**
**               驱动注册方式（板级静态参数 probe 模式）：
**                 demoip_driver_init() 以静态 demoip_board_params_t 结构体模仿设备树节点信息，
**                 调用 __demoip_probe() 完成初始化。真实驱动中 probe 由 platform_driver /
**                 of_platform_driver 框架在解析设备树后自动调用，并传入解析后的参数。
**                 静态参数包含：compatible、reg_base/size（寄存器基地址）、irq（中断号）、
**                 nr_channels（通道数）、dev_name（DMA 引擎设备名称）。
**
**               移植说明（替换为真实硬件时）：
**                 1. __demoip_probe            : 从 params 读取 reg_base/irq，
**                                               调用 API_VmmMap 映射寄存器、API_InterVectorConnect 注册中断。
**                 2. device_alloc_chan_resources : 移除信号量/线程，改用已注册的中断。
**                 3. __demoip_execute_xxx        : 替换 memcpy 为硬件 BD/descriptor ring 提交。
**                 4. __demoip_issue_pending       : 替换 post-sem 为写硬件 doorbell / tail pointer。
**                 5. ISR + 底半部完成 dma_cookie_complete + 回调。
**
*********************************************************************************************************/
#define  __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../dmaengine.h"
#include "demoip.h"

/*********************************************************************************************************
  设备标识常量
*********************************************************************************************************/
#define DEMOIP_DEV_NAME             "demoip-dma"
#define DEMOIP_MAX_CHANS            2
/*********************************************************************************************************
  驱动内部常量
*********************************************************************************************************/
#define DEMOIP_WORKER_STACK             (8192)
#define DEMOIP_WORKER_PRIO              (120)
#define DEMOIP_SG_MAX                   (64)                            /*  单次 prep_slave_sg 最大条目数*/

/*********************************************************************************************************
  私有描述符操作类型
*********************************************************************************************************/
typedef enum demoip_op {
    DEMOIP_OP_MEMCPY    = 0,                                            /*  MEM_TO_MEM memcpy           */
    DEMOIP_OP_SLAVE_SG  = 1,                                            /*  Slave SG (外设 ↔ 内存)     */
} demoip_op_t;

/*********************************************************************************************************
  私有描述符
**
**  dma_async_tx_descriptor_t 必须为结构体第一个成员（txd.node 挂入通道链表）。
**  slave SG 额外分配 sgl 拷贝，统一由 __demoip_desc_free 释放。
**
*********************************************************************************************************/
struct demoip_desc {
    dma_async_tx_descriptor_t       txd;                                /*  公共描述符（必须第一）      */
    demoip_op_t                     op;                                 /*  操作类型                    */

    /* MEMCPY params */
    phys_addr_t                     src;
    phys_addr_t                     dst;
    size_t                          len;

    /* SLAVE_SG params */
    dma_sg_entry_t                 *sgl;                                /*  SG 条目拷贝（driver 分配）  */
    UINT                            sg_len;
    dma_transfer_direction_t        direction;
    phys_addr_t                     dev_addr;                           /*  设备缓冲区地址（模拟）      */
};

/*********************************************************************************************************
  私有通道
*********************************************************************************************************/
struct demoip_chan {
    dma_chan_t                      base;                               /*  公共通道（必须第一）        */
    PLW_LIST_LINE                   pending;
    PLW_LIST_LINE                   active;
    spinlock_t                      lock;
    LW_OBJECT_HANDLE                sem;
    LW_OBJECT_HANDLE                thread;
    BOOL                            stop;
    BOOL                            running;
    dma_slave_config_t              slave_cfg;                          /*  最近一次 device_config 配置 */
};

/*********************************************************************************************************
  私有设备
*********************************************************************************************************/
struct demoip_device {
    dma_device_t                    base;
    struct demoip_chan               chans[DEMOIP_MAX_CHANS];
};

static struct demoip_device         _G_demoip_dev;

/*================================================ 内部辅助函数 ==========================================*/

static LW_INLINE struct demoip_desc *__demoip_desc_from_txd (dma_async_tx_descriptor_t *txd)
{
    return  _LIST_ENTRY(txd, struct demoip_desc, txd);
}

static LW_INLINE struct demoip_chan *__demoip_chan_from_base (dma_chan_t *chan)
{
    return  _LIST_ENTRY(chan, struct demoip_chan, base);
}

/*********************************************************************************************************
** 函数名称: __demoip_desc_free
** 功能描述: 统一释放描述符（slave SG 描述符同时释放 sgl 拷贝）
** 输　入  : desc — 待释放的私有描述符
** 输　出  : NONE
*********************************************************************************************************/

static VOID  __demoip_desc_free (struct demoip_desc *desc)
{
    if (!desc) {
        return;
    }
    if (desc->op == DEMOIP_OP_SLAVE_SG && desc->sgl) {
        sys_free(desc->sgl);
        desc->sgl = LW_NULL;
    }
    sys_free(desc);
}

/*================================================ 传输执行函数 ==========================================*/

/*********************************************************************************************************
** 函数名称: __demoip_execute_memcpy
** 功能描述: 执行 MEMCPY 类型描述符（软件 memcpy 模拟 MEM_TO_MEM DMA）
** 输　入  : desc — 待执行的私有描述符
** 输　出  : NONE
** 对应    : 真实硬件中为等待 HW 完成中断后的底半部处理
*********************************************************************************************************/

static VOID  __demoip_execute_memcpy (struct demoip_desc *desc)
{
    lib_memcpy((PVOID)(addr_t)desc->dst,
               (CPVOID)(addr_t)desc->src,
               desc->len);
}

/*********************************************************************************************************
** 函数名称: __demoip_execute_slave_sg
** 功能描述: 执行 SLAVE_SG 类型描述符（软件模拟外设 ↔ 内存散布聚集 DMA）
** 输　入  : desc — 待执行的私有描述符
** 输　出  : NONE
**
**  模拟规则：
**    dev_addr 指向一段连续"设备缓冲区"（由测试代码提供，等同于设备 FIFO/DMA 寄存器区域）。
**    传输时 dev_addr 随每个 SG 条目前进，使设备侧呈现为线性连续缓冲区。
**    MEM_TO_DEV: sg[i].addr → dev_addr+offset（模拟写入外设 FIFO）
**    DEV_TO_MEM: dev_addr+offset → sg[i].addr（模拟从外设 FIFO 读出）
**
** 对应    : 真实网卡/块设备驱动中的 BD ring 描述符链路填充与硬件 scatter-gather 引擎
**
*********************************************************************************************************/

static VOID  __demoip_execute_slave_sg (struct demoip_desc *desc)
{
    phys_addr_t  dev_ptr = desc->dev_addr;
    UINT         i;

    for (i = 0; i < desc->sg_len; i++) {
        phys_addr_t  sg_addr = desc->sgl[i].addr;
        size_t       sg_len  = desc->sgl[i].len;

        if (desc->direction == DMA_MEM_TO_DEV) {
            lib_memcpy((PVOID)(addr_t)dev_ptr,
                       (CPVOID)(addr_t)sg_addr,
                       sg_len);
        } else {                                                        /*  DEV_TO_MEM                  */
            lib_memcpy((PVOID)(addr_t)sg_addr,
                       (CPVOID)(addr_t)dev_ptr,
                       sg_len);
        }
        dev_ptr += sg_len;                                              /*  设备侧线性前进              */
    }
}

/*********************************************************************************************************
** 函数名称: __demoip_execute_desc
** 功能描述: 根据描述符类型分派执行，完成后更新 cookie 并触发回调
** 输　入  : desc — 待执行的私有描述符
** 输　出  : NONE
** 说明    : 在 worker 线程中调用，对应 Linux DMA 驱动底半部的完成处理流程。
*********************************************************************************************************/

static VOID  __demoip_execute_desc (struct demoip_desc *desc)
{
    dmaengine_result_t  result;

    switch (desc->op) {
    case DEMOIP_OP_MEMCPY:
        __demoip_execute_memcpy(desc);
        break;

    case DEMOIP_OP_SLAVE_SG:
        __demoip_execute_slave_sg(desc);
        break;

    default:
        printk("[demoip] unknown op=%d, skipped\n", (INT)desc->op);
        break;
    }

    dma_cookie_complete(&desc->txd);                                    /*  须在回调前完成              */

    if (desc->txd.callback_result) {
        result.result  = DMA_COMPLETE;
        result.residue = 0;
        desc->txd.callback_result(desc->txd.callback_param, &result);
    } else if (desc->txd.callback) {
        desc->txd.callback(desc->txd.callback_param);
    }
}

/*================================================ Worker 线程 ===========================================*/

/*********************************************************************************************************
** 函数名称: __demoip_worker_thread
** 功能描述: 通道 worker 线程，循环从 active 链表取出描述符执行
** 输　入  : arg — struct demoip_chan *
** 输　出  : LW_NULL
** 说明    : 对应 Linux DMA 驱动中的 threaded IRQ / tasklet bottom-half。
**           真实驱动中此线程由 ISR → tasklet_schedule 替代。
*********************************************************************************************************/

static PVOID  __demoip_worker_thread (PVOID arg)
{
    struct demoip_chan   *dchan = (struct demoip_chan *)arg;
    struct demoip_desc  *desc;
    PLW_LIST_LINE        pline;
    INTREG               ireg;

    while (LW_TRUE) {
        API_SemaphoreBPend(dchan->sem, LW_OPTION_WAIT_INFINITE);

        if (dchan->stop) {
            break;
        }

        do {
            LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);
            pline = dchan->active;
            if (pline) {
                _List_Line_Del(pline, &dchan->active);
            }
            LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);

            if (!pline) {
                break;
            }

            desc = _LIST_ENTRY(pline, struct demoip_desc, txd.node);
            __demoip_execute_desc(desc);
            __demoip_desc_free(desc);

        } while (LW_TRUE);
    }

    return  LW_NULL;
}

/*================================================ 描述符提交函数 =========================================*/

/*********************************************************************************************************
** 函数名称: __demoip_tx_submit
** 功能描述: 描述符提交函数，由 dmaengine_submit 回调
** 输　入  : tx — 描述符基类指针
** 输　出  : cookie
** 说明    : 分配 cookie，将描述符加入 pending 链表（未执行，等待 issue_pending 触发）。
**           对应 Linux 驱动中 tx_submit 实现（如 pl330_tx_submit）。
*********************************************************************************************************/

static dma_cookie_t  __demoip_tx_submit (dma_async_tx_descriptor_t *tx)
{
    struct demoip_chan  *dchan = __demoip_chan_from_base(tx->chan);
    dma_cookie_t        cookie;
    INTREG              ireg;

    LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);
    cookie = dma_cookie_assign(tx);
    _List_Line_Add_Tail(&tx->node, &dchan->pending);
    LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);

    return  cookie;
}

/*================================================ device ops 实现 =======================================*/

/*********************************************************************************************************
** 函数名称: __demoip_alloc_chan_resources
** 功能描述: 通道资源分配（对应 Linux device_alloc_chan_resources）
** 输　入  : chan — 目标通道
** 输　出  : 0 成功；-1 失败
** 说明    : 初始化自旋锁、信号量、worker 线程，初始化 cookie 计数。
**           幂等：已初始化时直接返回 0。
*********************************************************************************************************/

static INT  __demoip_alloc_chan_resources (dma_chan_t *chan)
{
    struct demoip_chan   *dchan = __demoip_chan_from_base(chan);
    LW_CLASS_THREADATTR  attr;
    CHAR                 name[32];

    if (dchan->running) {
        return  (0);
    }

    LW_SPIN_INIT(&dchan->lock);
    dchan->pending = LW_NULL;
    dchan->active  = LW_NULL;
    dchan->stop    = LW_FALSE;
    lib_memset(&dchan->slave_cfg, 0, sizeof(dchan->slave_cfg));

    dchan->sem = API_SemaphoreBCreate("demoip_sem",
                                       LW_FALSE,
                                       LW_OPTION_OBJECT_LOCAL,
                                       LW_NULL);
    if (dchan->sem == LW_OBJECT_HANDLE_INVALID) {
        printk("[demoip] failed to create semaphore for '%s'\n", chan->chan_name);
        return  (-1);
    }

    snprintf(name, sizeof(name), "demoip_wk%d",
                 (INT)(dchan - _G_demoip_dev.chans));

    API_ThreadAttrBuild(&attr,
                        DEMOIP_WORKER_STACK,
                        DEMOIP_WORKER_PRIO,
                        LW_OPTION_THREAD_STK_CHK,
                        (PVOID)dchan);

    dchan->thread = API_ThreadCreate(name,
                                      (PTHREAD_START_ROUTINE)__demoip_worker_thread,
                                      &attr,
                                      LW_NULL);
    if (dchan->thread == LW_OBJECT_HANDLE_INVALID) {
        printk("[demoip] failed to create worker thread for '%s'\n", chan->chan_name);
        API_SemaphoreBDelete(&dchan->sem);
        return  (-1);
    }

    dma_cookie_init(chan);
    dchan->running = LW_TRUE;

    printk("[demoip] channel '%s' resources allocated\n", chan->chan_name);
    return  (0);
}

/*********************************************************************************************************
** 函数名称: __demoip_free_chan_resources
** 功能描述: 通道资源释放（对应 Linux device_free_chan_resources）
** 输　入  : chan — 目标通道
** 输　出  : NONE
** 说明    : 停止 worker 线程，清空残余描述符（含 slave SG 的 sgl 拷贝）。
*********************************************************************************************************/

static VOID  __demoip_free_chan_resources (dma_chan_t *chan)
{
    struct demoip_chan  *dchan = __demoip_chan_from_base(chan);
    PLW_LIST_LINE       pline;
    struct demoip_desc *desc;
    INTREG              ireg;

    if (!dchan->running) {
        return;
    }

    dchan->stop = LW_TRUE;
    API_SemaphoreBPost(dchan->sem);
    API_ThreadJoin(dchan->thread, LW_NULL);
    API_SemaphoreBDelete(&dchan->sem);

    LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);

    while ((pline = dchan->pending) != LW_NULL) {
        _List_Line_Del(pline, &dchan->pending);
        desc = _LIST_ENTRY(pline, struct demoip_desc, txd.node);
        LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);
        __demoip_desc_free(desc);
        LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);
    }

    while ((pline = dchan->active) != LW_NULL) {
        _List_Line_Del(pline, &dchan->active);
        desc = _LIST_ENTRY(pline, struct demoip_desc, txd.node);
        LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);
        __demoip_desc_free(desc);
        LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);
    }

    LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);

    dchan->running = LW_FALSE;
    printk("[demoip] channel '%s' resources freed\n", chan->chan_name);
}

/*********************************************************************************************************
** 函数名称: __demoip_config
** 功能描述: Slave 通道参数配置（对应 Linux device_config）
** 输　入  : chan   — 目标通道
**           config — Slave 配置（方向、设备地址、总线宽度、burst 大小）
** 输　出  : 0 成功；-1 参数无效
** 说明    : 保存配置到通道私有数据，在 device_prep_slave_sg 时读取。
**           对应 Linux 驱动中 dmaengine_slave_config() 路径。
**           真实硬件驱动在此处配置控制器寄存器（FIFO 地址、数据宽度、突发长度等）。
*********************************************************************************************************/

static INT  __demoip_config (dma_chan_t *chan, dma_slave_config_t *config)
{
    struct demoip_chan  *dchan = __demoip_chan_from_base(chan);

    if (!config) {
        return  (-1);
    }

    if (config->direction == DMA_MEM_TO_DEV && config->dst_addr == 0) {
        printk("[demoip] config MEM_TO_DEV but dst_addr is 0\n");
        return  (-1);
    }

    if (config->direction == DMA_DEV_TO_MEM && config->src_addr == 0) {
        printk("[demoip] config DEV_TO_MEM but src_addr is 0\n");
        return  (-1);
    }

    dchan->slave_cfg = *config;
    return  (0);
}

/*********************************************************************************************************
** 函数名称: __demoip_prep_dma_memcpy
** 功能描述: 准备 MEM_TO_MEM 传输描述符（对应 Linux device_prep_dma_memcpy）
** 输　入  : chan  — 目标通道
**           dst   — 目标地址（demo 驱动视为虚拟地址）
**           src   — 源地址
**           len   — 传输长度
**           flags — DMA_PREP_xxx 标志
** 输　出  : 描述符指针；LW_NULL 失败
*********************************************************************************************************/

static dma_async_tx_descriptor_t *
__demoip_prep_dma_memcpy (dma_chan_t *chan,
                           phys_addr_t dst, phys_addr_t src,
                           size_t len, ULONG flags)
{
    struct demoip_desc  *desc;

    desc = (struct demoip_desc *)sys_malloc(sizeof(struct demoip_desc));
    if (!desc) {
        printk("[demoip] OOM: descriptor alloc failed\n");
        return  LW_NULL;
    }

    lib_memset(desc, 0, sizeof(struct demoip_desc));

    desc->op        = DEMOIP_OP_MEMCPY;
    desc->src       = src;
    desc->dst       = dst;
    desc->len       = len;

    desc->txd.chan      = chan;
    desc->txd.flags     = flags;
    desc->txd.cookie    = DMA_COOKIE_INVALID;
    desc->txd.tx_submit = __demoip_tx_submit;
    _LIST_LINE_INIT_IN_CODE(desc->txd.node);

    return  &desc->txd;
}

/*********************************************************************************************************
** 函数名称: __demoip_prep_slave_sg
** 功能描述: 准备 Slave SG 传输描述符（对应 Linux device_prep_slave_sg）
** 输　入  : chan      — 目标通道（须已通过 device_config 配置 slave 参数）
**           sgl       — 用户提供的 SG 条目数组（本函数内部拷贝，用户可立即释放）
**           sg_len    — SG 条目数
**           direction — DMA_MEM_TO_DEV 或 DMA_DEV_TO_MEM
**           flags     — DMA_PREP_xxx 标志
**           context   — 扩展上下文（当前未使用）
** 输　出  : 描述符指针；LW_NULL 失败
**
**  设备地址来源：
**    MEM_TO_DEV → dchan->slave_cfg.dst_addr（外设 FIFO/寄存器写地址）
**    DEV_TO_MEM → dchan->slave_cfg.src_addr（外设 FIFO/寄存器读地址）
**
**  真实硬件驱动：
**    在此函数中填充硬件 BD ring entry（src/dst 物理地址、传输长度、控制字段），
**    完成后提交到控制器待发送队列。
**
*********************************************************************************************************/

static dma_async_tx_descriptor_t *
__demoip_prep_slave_sg (dma_chan_t *chan,
                         dma_sg_entry_t *sgl, UINT sg_len,
                         dma_transfer_direction_t direction,
                         ULONG flags, PVOID context)
{
    struct demoip_chan  *dchan = __demoip_chan_from_base(chan);
    struct demoip_desc *desc;
    dma_sg_entry_t     *sgl_copy;
    phys_addr_t         dev_addr;

    (VOID)context;

    if (!sgl || sg_len == 0 || sg_len > DEMOIP_SG_MAX) {
        printk("[demoip] prep_slave_sg: invalid sg_len=%u (max=%d)\n",
               sg_len, DEMOIP_SG_MAX);
        return  LW_NULL;
    }

    if (direction != DMA_MEM_TO_DEV && direction != DMA_DEV_TO_MEM) {
        printk("[demoip] prep_slave_sg: unsupported direction=%d\n", (INT)direction);
        return  LW_NULL;
    }

    dev_addr = (direction == DMA_MEM_TO_DEV)
               ? dchan->slave_cfg.dst_addr
               : dchan->slave_cfg.src_addr;

    if (dev_addr == 0) {
        printk("[demoip] prep_slave_sg: device address not configured"
               " (call dmaengine_slave_config first)\n");
        return  LW_NULL;
    }

    /*
     *  拷贝 SG 列表，驱动持有独立副本，用户 sgl 可在 prep 返回后立即释放
     */
    sgl_copy = (dma_sg_entry_t *)sys_malloc(sg_len * sizeof(dma_sg_entry_t));
    if (!sgl_copy) {
        printk("[demoip] OOM: sg list alloc failed\n");
        return  LW_NULL;
    }
    lib_memcpy(sgl_copy, sgl, sg_len * sizeof(dma_sg_entry_t));

    desc = (struct demoip_desc *)sys_malloc(sizeof(struct demoip_desc));
    if (!desc) {
        sys_free(sgl_copy);
        printk("[demoip] OOM: descriptor alloc failed\n");
        return  LW_NULL;
    }

    lib_memset(desc, 0, sizeof(struct demoip_desc));

    desc->op        = DEMOIP_OP_SLAVE_SG;
    desc->sgl       = sgl_copy;
    desc->sg_len    = sg_len;
    desc->direction = direction;
    desc->dev_addr  = dev_addr;

    desc->txd.chan      = chan;
    desc->txd.flags     = flags;
    desc->txd.cookie    = DMA_COOKIE_INVALID;
    desc->txd.tx_submit = __demoip_tx_submit;
    _LIST_LINE_INIT_IN_CODE(desc->txd.node);

    return  &desc->txd;
}

/*********************************************************************************************************
** 函数名称: __demoip_tx_status
** 功能描述: 查询 cookie 对应传输状态（对应 Linux device_tx_status）
** 输　入  : chan    — 目标通道
**           cookie  — 查询目标
**           txstate — 输出状态（可为 LW_NULL）
** 输　出  : dma_status_t
*********************************************************************************************************/

static dma_status_t  __demoip_tx_status (dma_chan_t     *chan,
                                          dma_cookie_t   cookie,
                                          dma_tx_state_t *txstate)
{
    return  dma_cookie_status(chan, cookie, txstate);
}

/*********************************************************************************************************
** 函数名称: __demoip_issue_pending
** 功能描述: 将 pending→active，唤醒 worker（对应 Linux device_issue_pending）
** 输　入  : chan — 目标通道
** 输　出  : NONE
** 说明    : 真实硬件驱动在此处写 doorbell / tail pointer 寄存器，触发 DMA 硬件开始搬运。
*********************************************************************************************************/

static VOID  __demoip_issue_pending (dma_chan_t *chan)
{
    struct demoip_chan  *dchan = __demoip_chan_from_base(chan);
    BOOL                has_work = LW_FALSE;
    PLW_LIST_LINE       pline;
    INTREG              ireg;

    LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);
    while ((pline = dchan->pending) != LW_NULL) {
        _List_Line_Del(pline, &dchan->pending);
        _List_Line_Add_Tail(pline, &dchan->active);
        has_work = LW_TRUE;
    }
    LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);

    if (has_work) {
        API_SemaphoreBPost(dchan->sem);
    }
}

/*********************************************************************************************************
** 函数名称: __demoip_terminate_all
** 功能描述: 终止所有传输，清空 pending 和 active 链表（对应 Linux device_terminate_all）
** 输　入  : chan — 目标通道
** 输　出  : 0 成功
** 注意    : 不等待 worker 完成当前描述符；若需强制停止，在 free_chan_resources 中处理。
*********************************************************************************************************/

static INT  __demoip_terminate_all (dma_chan_t *chan)
{
    struct demoip_chan  *dchan = __demoip_chan_from_base(chan);
    PLW_LIST_LINE       pline;
    struct demoip_desc *desc;
    INTREG              ireg;

    LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);

    while ((pline = dchan->pending) != LW_NULL) {
        _List_Line_Del(pline, &dchan->pending);
        desc = _LIST_ENTRY(pline, struct demoip_desc, txd.node);
        LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);
        __demoip_desc_free(desc);
        LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);
    }

    while ((pline = dchan->active) != LW_NULL) {
        _List_Line_Del(pline, &dchan->active);
        desc = _LIST_ENTRY(pline, struct demoip_desc, txd.node);
        LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);
        __demoip_desc_free(desc);
        LW_SPIN_LOCK_IRQ(&dchan->lock, &ireg);
    }

    LW_SPIN_UNLOCK_IRQ(&dchan->lock, ireg);
    return  (0);
}

/*================================================ 板级参数与 probe 入口 ==================================*/

/*********************************************************************************************************
**  板级静态参数（模仿设备树节点信息）
**
**  真实驱动中，这些字段由 OF / ACPI 框架解析设备树后填入并传给 probe()：
**    compatible  ← DT: compatible = "demoip,dma-1.00.a"
**    reg_base    ← DT: reg = <0x43C00000 0x10000>（寄存器基地址）
**    reg_size    ← DT: reg 第二个元素（寄存器区域大小）
**    irq         ← DT: interrupts = <0 57 4>（经 GIC 映射后的 SylixOS 中断号）
**    nr_channels ← DT: dma-channels = <2>
**    dev_name    ← 驱动代码内定义，不来自 DT，但在此集中管理便于多实例区分
**
**  软件模拟版本中 reg_base / irq 仅作说明，不做实际映射/注册。
**
*********************************************************************************************************/

typedef struct {
    CPCHAR      compatible;                                             /*  设备兼容字符串              */
    CPCHAR      dev_name;                                               /*  DMA 引擎设备名称            */
    phys_addr_t reg_base;                                               /*  寄存器基地址 (DT: reg[0])   */
    size_t      reg_size;                                               /*  寄存器区域大小 (DT: reg[1]) */
    INT         irq;                                                    /*  中断号 (DT: interrupts)     */
    INT         nr_channels;                                            /*  DMA 通道数 (DT: dma-channels)*/
} demoip_board_params_t;

/*
 *  静态板级参数定义（对应 FMQL SoC 上的 AXI DMA 控制器节点）
 */
static const demoip_board_params_t  _G_demoip_board_params = {
    .compatible  = "demoip,dma-1.00.a",
    .dev_name    = DEMOIP_DEV_NAME,
    .reg_base    = 0x43C00000UL,
    .reg_size    = 0x10000,
    .irq         = 89,
    .nr_channels = DEMOIP_MAX_CHANS,
};

/*********************************************************************************************************
** 函数名称: __demoip_probe
** 功能描述: Demo IP DMA 驱动 probe 函数（使用板级参数完成驱动初始化并注册到框架）
** 输　入  : params — 板级参数指针（来自静态定义或设备树解析结果）
** 输　出  : 0 成功；-1 失败
**
**  probe 执行步骤：
**    1. 校验参数合法性（nr_channels 范围、地址非零）
**    2. 打印板级资源信息（对应真实驱动中的 dev_info 输出）
**    3. 【真实硬件】映射寄存器：API_VmmMap(params->reg_base, params->reg_size)
**    4. 【真实硬件】注册中断：API_InterVectorConnect(params->irq, isr_fn, ...)
**    5. 填充 dma_device_t 并注册到 DMA Engine 框架
**
**  注意：步骤 3/4 在软件模拟版本中跳过，仅打印提示。
**
*********************************************************************************************************/

static INT  __demoip_probe (const demoip_board_params_t *params)
{
    struct demoip_device  *ddev = &_G_demoip_dev;
    dma_device_t          *dev  = &ddev->base;
    INT                    i;

    if (!params || params->nr_channels <= 0 ||
        params->nr_channels > DEMOIP_MAX_CHANS) {
        printk("[demoip] probe: invalid params (nr_channels=%d, max=%d)\n",
               params ? params->nr_channels : -1, DEMOIP_MAX_CHANS);
        return  (-1);
    }

    printk("[demoip] probe: compatible='%s'\n", params->compatible);
    printk("[demoip] probe: reg=0x%08lx/0x%zx  irq=%d  nr_channels=%d\n",
           (unsigned long)params->reg_base, params->reg_size,
           params->irq, params->nr_channels);
    printk("[demoip] probe: (software simulation — reg/irq not mapped)\n");

    /*
     *  【真实硬件替换点 1】映射寄存器基地址
     *  base = API_VmmMap(params->reg_base, params->reg_size,
     *                    LW_VMM_FLAG_RDWR | LW_VMM_FLAG_NO_CACHE);
     *
     *  【真实硬件替换点 2】注册中断向量
     *  API_InterVectorConnect(params->irq, __demoip_isr, ddev, "demoip");
     *  API_InterVectorEnable(params->irq);
     */

    lib_memset(ddev, 0, sizeof(struct demoip_device));

    lib_strlcpy(dev->dev_name, params->dev_name, sizeof(dev->dev_name));
    dev->channels = LW_NULL;
    LW_SPIN_INIT(&dev->lock);

    dev->device_alloc_chan_resources = __demoip_alloc_chan_resources;
    dev->device_free_chan_resources  = __demoip_free_chan_resources;
    dev->device_prep_dma_memcpy      = __demoip_prep_dma_memcpy;
    dev->device_prep_slave_sg        = __demoip_prep_slave_sg;
    dev->device_config               = __demoip_config;
    dev->device_tx_status            = __demoip_tx_status;
    dev->device_issue_pending        = __demoip_issue_pending;
    dev->device_terminate_all        = __demoip_terminate_all;

    for (i = 0; i < params->nr_channels; i++) {
        struct demoip_chan  *dchan = &ddev->chans[i];

        snprintf(dchan->base.chan_name, sizeof(dchan->base.chan_name),
                     "%s-ch%d", params->dev_name, i);
        dchan->base.device = dev;
        _LIST_LINE_INIT_IN_CODE(dchan->base.node);
        _List_Line_Add_Tail(&dchan->base.node, &dev->channels);
    }
    dev->chancnt = params->nr_channels;

    printk("[demoip] probe OK: dev='%s'  %d channels (memcpy + slave-sg)\n",
           params->dev_name, params->nr_channels);
    return  dma_async_device_register(dev);
}

/*********************************************************************************************************
** 函数名称: demoip_driver_init
** 功能描述: Demo IP DMA 驱动初始化入口（模块加载时调用）
** 输　入  : NONE
** 输　出  : 0 成功；-1 失败
** 说明    : 以静态板级参数调用 __demoip_probe()，模仿真实驱动中设备树 probe 的调用路径。
**           真实 SylixOS/Linux 驱动框架会在解析设备树后自动调用 probe()，
**           此处用静态参数显式调用等价于单实例 platform_device 的注册流程。
*********************************************************************************************************/

INT  demoip_driver_init (VOID)
{
    return  __demoip_probe(&_G_demoip_board_params);
}

/*********************************************************************************************************
** 函数名称: demoip_driver_exit
** 功能描述: Demo IP DMA 驱动注销入口
** 输　入  : NONE
** 输　出  : NONE
** 注意    : 调用前须保证所有通道已通过 dma_release_channel 释放。
*********************************************************************************************************/

VOID  demoip_driver_exit (VOID)
{
    dma_async_device_unregister(&_G_demoip_dev.base);
    printk("[demoip] driver exit\n");
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

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
** 文   件   名: dma_core.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA 引擎核心层实现
**               全局设备链表、通道/描述符生命周期管理、pending→active 调度、
**               完成处理（cookie 更新）及错误恢复。
**
**               线程安全模型：
**               - 全局设备链表由 _G_core_lock 保护。
**               - 每个 dma_chan 有独立 lock（IRQ 安全），保护队列和 cookie 字段。
**
** 修改记录:
** 2026.03.24  新增 dma_core_alloc_chan_dir（方向筛选）；
**             dma_core_complete 改用 enum dma_status 并维护 completed_cookie/last_status；
**             callback 签名改为 (void *param)，对齐 Linux dma_async_tx_callback。
**
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include "dma_types.h"
#include "dma_core.h"

/*********************************************************************************************************
  全局变量
*********************************************************************************************************/

static PLW_LIST_LINE  _G_dev_list    = LW_NULL;
static spinlock_t     _G_core_lock;
static BOOL           _G_core_inited = LW_FALSE;

/*********************************************************************************************************
  队列辅助函数（双向链表，基于 LW_LIST_LINE）
*********************************************************************************************************/

static LW_INLINE VOID _q_add_tail (PLW_LIST_LINE *head, PLW_LIST_LINE node)
{
    PLW_LIST_LINE iter;

    node->LINE_plistNext = LW_NULL;
    node->LINE_plistPrev = LW_NULL;

    if (*head == LW_NULL) {
        *head = node;
        return;
    }

    iter = *head;
    while (iter->LINE_plistNext) {
        iter = iter->LINE_plistNext;
    }
    iter->LINE_plistNext = node;
    node->LINE_plistPrev = iter;
}

static LW_INLINE VOID _q_del (PLW_LIST_LINE *head, PLW_LIST_LINE node)
{
    if (node->LINE_plistPrev) {
        node->LINE_plistPrev->LINE_plistNext = node->LINE_plistNext;
    } else {
        *head = node->LINE_plistNext;
    }

    if (node->LINE_plistNext) {
        node->LINE_plistNext->LINE_plistPrev = node->LINE_plistPrev;
    }

    node->LINE_plistNext = LW_NULL;
    node->LINE_plistPrev = LW_NULL;
}

/*********************************************************************************************************
  模块初始化 / 退出
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_core_init
** 功能描述: 初始化 DMA 引擎核心，只调用一次。
** 输　入  : NONE
** 输　出  : 0（始终成功）
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_core_init (void)
{
    if (_G_core_inited) {
        return  (0);
    }

    LW_SPIN_INIT(&_G_core_lock);
    _G_dev_list    = LW_NULL;
    _G_core_inited = LW_TRUE;

    return  (0);
}
/*********************************************************************************************************
** 函数名称: dma_core_exit
** 功能描述: 退出核心层（须先注销所有设备）。
** 输　入  : NONE
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_core_exit (void)
{
    _G_core_inited = LW_FALSE;
}

/*********************************************************************************************************
  设备注册 / 注销 / 查找
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_device_register
** 功能描述: 将控制器设备加入全局链表。
** 输　入  : dev           DMA 设备指针
** 输　出  : 0 成功；-1 参数无效
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_device_register (struct dma_device *dev)
{
    INTREG ireg;

    if (!dev) {
        return  (-1);
    }

    _INIT_LIST_LINE_HEAD(&dev->node);

    LW_SPIN_LOCK_QUICK(&_G_core_lock, &ireg);
    _q_add_tail(&_G_dev_list, &dev->node);
    LW_SPIN_UNLOCK_QUICK(&_G_core_lock, ireg);

    return  (0);
}
/*********************************************************************************************************
** 函数名称: dma_device_unregister
** 功能描述: 将控制器设备从全局链表移除。
** 输　入  : dev           DMA 设备指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_device_unregister (struct dma_device *dev)
{
    INTREG ireg;

    if (!dev) {
        return;
    }

    LW_SPIN_LOCK_QUICK(&_G_core_lock, &ireg);
    _q_del(&_G_dev_list, &dev->node);
    LW_SPIN_UNLOCK_QUICK(&_G_core_lock, ireg);
}
/*********************************************************************************************************
** 函数名称: dma_find_device
** 功能描述: 按名称查找已注册设备。
** 输　入  : name          设备名称字符串
** 输　出  : 找到时返回 dma_device 指针；未找到返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_device *dma_find_device (const char *name)
{
    PLW_LIST_LINE      pnode;
    struct dma_device *dev;
    INTREG             ireg;

    if (!name) {
        return  (LW_NULL);
    }

    LW_SPIN_LOCK_QUICK(&_G_core_lock, &ireg);
    pnode = _G_dev_list;
    while (pnode) {
        dev = _LIST_ENTRY(pnode, struct dma_device, node);
        if (lib_strcmp(dev->name, name) == 0) {
            LW_SPIN_UNLOCK_QUICK(&_G_core_lock, ireg);
            return  (dev);
        }
        pnode = pnode->LINE_plistNext;
    }
    LW_SPIN_UNLOCK_QUICK(&_G_core_lock, ireg);

    return  (LW_NULL);
}

/*********************************************************************************************************
  通道管理
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_core_alloc_chan_dir
** 功能描述: 按方向分配第一个空闲通道。
**           direction == DMA_DIR_ANY 时不过滤方向，分配首个空闲通道。
** 输　入  : dev           目标设备
**           direction     DMA_DIR_* 或 DMA_DIR_ANY
** 输　出  : 成功返回通道指针；无空闲通道返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_chan *dma_core_alloc_chan_dir (struct dma_device *dev, int direction)
{
    struct dma_chan *chan;
    INTREG           ireg;
    int              i;

    if (!dev || !dev->channels) {
        return  (LW_NULL);
    }

    for (i = 0; i < dev->num_channels; i++) {
        chan = &dev->channels[i];

        LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
        if (!chan->in_use &&
            (direction == DMA_DIR_ANY || chan->direction == direction)) {
            chan->in_use          = LW_TRUE;
            chan->pending_q       = LW_NULL;
            chan->active_q        = LW_NULL;
            chan->cookie          = DMA_MIN_COOKIE;
            chan->completed_cookie = 0;
            chan->last_status     = DMA_COMPLETE;
            LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
            return  (chan);
        }
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
    }

    return  (LW_NULL);
}
/*********************************************************************************************************
** 函数名称: dma_core_free_chan
** 功能描述: 排空队列（错误回调），标记通道空闲。
** 输　入  : chan          DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_core_free_chan (struct dma_chan *chan)
{
    if (!chan) {
        return;
    }

    dma_core_handle_error(chan);
    chan->in_use = LW_FALSE;
}

/*********************************************************************************************************
  描述符生命周期
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_desc_alloc
** 功能描述: 分配并初始化空描述符，绑定通道。由 device_prep_* 内部调用。
** 输　入  : chan          DMA 通道指针
** 输　出  : 成功返回描述符指针；失败返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_desc *dma_desc_alloc (struct dma_chan *chan)
{
    struct dma_desc *desc;

    if (!chan) {
        return  (LW_NULL);
    }

    desc = (struct dma_desc *)lib_malloc(sizeof(struct dma_desc));
    if (!desc) {
        return  (LW_NULL);
    }

    lib_memset(desc, 0, sizeof(struct dma_desc));
    desc->chan   = chan;
    desc->status = DMA_IN_PROGRESS;

    return  (desc);
}
/*********************************************************************************************************
** 函数名称: dma_desc_free
** 功能描述: 释放描述符及其 hw_desc。
**           调用 desc->free_hw_desc()（HW 层设置）释放硬件资源，再 free desc 本体。
** 输　入  : desc          DMA 描述符指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_desc_free (struct dma_desc *desc)
{
    if (!desc) {
        return;
    }

    if (desc->free_hw_desc) {
        desc->free_hw_desc(desc);                                       /*  HW 层释放 bd_virt 等资源    */
    }

    lib_free(desc);
}

/*********************************************************************************************************
  调度
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_core_submit
** 功能描述: 将描述符（cookie 已赋值）加入 pending 队列。
** 输　入  : desc          已准备好的 DMA 描述符
** 输　出  : 0 成功；-1 参数无效
** 全局变量:
** 调用模块:
*********************************************************************************************************/
int dma_core_submit (struct dma_desc *desc)
{
    struct dma_chan *chan;
    INTREG           ireg;

    if (!desc || !desc->chan) {
        return  (-1);
    }
    chan = desc->chan;

    LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
    _q_add_tail(&chan->pending_q, &desc->node);
    LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

    return  (0);
}
/*********************************************************************************************************
** 函数名称: dma_core_schedule
** 功能描述: 若 active_q 为空且 pending_q 非空，将首个 pending 描述符移至 active 并触发传输。
**           可在任务上下文和底半部安全调用。
** 输　入  : chan          DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_core_schedule (struct dma_chan *chan)
{
    struct dma_desc *desc;
    INTREG           ireg;

    if (!chan || !chan->dev || !chan->dev->ops) {
        return;
    }

    LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);

    if (chan->active_q != LW_NULL || chan->pending_q == LW_NULL) {
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
        return;
    }

    desc = _LIST_ENTRY(chan->pending_q, struct dma_desc, node);
    _q_del(&chan->pending_q, &desc->node);
    _q_add_tail(&chan->active_q, &desc->node);
    desc->status = DMA_IN_PROGRESS;

    LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

    if (chan->dev->ops->device_issue_pending) {
        chan->dev->ops->device_issue_pending(chan);
    }
}
/*********************************************************************************************************
** 函数名称: dma_core_issue_pending
** 功能描述: 对外调度触发接口，委托给 dma_core_schedule()。
** 输　入  : chan          DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_core_issue_pending (struct dma_chan *chan)
{
    dma_core_schedule(chan);
}

/*********************************************************************************************************
  完成处理
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_core_complete
** 功能描述: 传输完成处理（由硬件驱动底半部调用）。
**           更新 completed_cookie / last_status，移出队列，释放描述符，
**           调度下一个 pending，最后触发完成回调。
** 输　入  : chan          通道指针
**           desc          已完成的描述符
**           status        DMA_COMPLETE 或 DMA_ERROR
** 输　出  : NONE
** 全局变量:
** 调用模块:
** 注意    : 在 defer 线程中执行；回调可使用 API_SemaphoreBPost 等轻量原语。
*********************************************************************************************************/
void dma_core_complete (struct dma_chan *chan, struct dma_desc *desc, enum dma_status status)
{
    dma_async_tx_callback  cb;
    void                  *cb_param;
    INTREG                 ireg;

    if (!chan || !desc) {
        return;
    }

    cb       = desc->callback;
    cb_param = desc->callback_param;

    LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
    _q_del(&chan->active_q, &desc->node);
    desc->status           = status;
    chan->completed_cookie = desc->cookie;                              /*  更新 cookie，供状态查询     */
    chan->last_status      = status;
    LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

    dma_desc_free(desc);                                                /*  释放 hw_desc + desc 本体    */
    dma_core_schedule(chan);                                            /*  调度下一个 pending          */

    if (cb) {
        cb(cb_param);                                                   /*  唤醒等待线程（最后执行）    */
    }
}

/*********************************************************************************************************
  错误恢复
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_core_handle_error
** 功能描述: 停止通道，排空 active_q 和 pending_q，
**           对每个描述符以 DMA_ERROR 触发回调后释放资源。
** 输　入  : chan          DMA 通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_core_handle_error (struct dma_chan *chan)
{
    struct dma_desc  *desc;
    INTREG            ireg;

    if (!chan || !chan->dev) {
        return;
    }

    if (chan->dev->ops && chan->dev->ops->device_terminate_all) {
        chan->dev->ops->device_terminate_all(chan);
    }

    for (;;) {
        LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
        if (chan->active_q == LW_NULL) {
            LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
            break;
        }
        desc = _LIST_ENTRY(chan->active_q, struct dma_desc, node);
        _q_del(&chan->active_q, &desc->node);
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

        if (desc->callback) desc->callback(desc->callback_param);
        dma_desc_free(desc);
    }

    for (;;) {
        LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
        if (chan->pending_q == LW_NULL) {
            LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
            break;
        }
        desc = _LIST_ENTRY(chan->pending_q, struct dma_desc, node);
        _q_del(&chan->pending_q, &desc->node);
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

        if (desc->callback) desc->callback(desc->callback_param);
        dma_desc_free(desc);
    }
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

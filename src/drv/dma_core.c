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
** 创   建   人: AutoGen
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA 引擎核心层
**               实现全局设备链表、通道/描述符生命周期管理、pending→active 调度、
**               完成处理及错误恢复。
**
**               线程安全模型：
**               - 全局设备链表由 _G_core_lock（spinlock）保护。
**               - 每个 dma_chan 有独立的 lock，使用 LW_SPIN_LOCK_QUICK（保存并关中断），
**                 任务上下文和 ISR 上下文均可安全访问队列。
**
** BUG
** 2026.03.23  初始版本。
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include "dma_types.h"
#include "dma_core.h"

/*********************************************************************************************************
  全局变量
*********************************************************************************************************/

static PLW_LIST_LINE  _G_dev_list   = LW_NULL;                         /*  已注册设备链表头            */
static spinlock_t     _G_core_lock;                                     /*  设备链表保护锁              */
static BOOL           _G_core_inited = LW_FALSE;                        /*  核心是否已初始化            */

/*********************************************************************************************************
  队列辅助内联函数（双向链表，基于 LW_LIST_LINE）
  避免依赖 SylixOS 内核私有的 _List_Line_Add/Del 符号。
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: _q_add_tail
** 功能描述: 将节点追加到链表尾部
** 输　入  : head          链表头指针的指针
**           node          待插入节点
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static LW_INLINE VOID _q_add_tail (PLW_LIST_LINE *head, PLW_LIST_LINE node)
{
    PLW_LIST_LINE iter;

    node->LINE_plistNext = LW_NULL;
    node->LINE_plistPrev = LW_NULL;

    if (*head == LW_NULL) {                                             /*  链表为空，直接成为头节点    */
        *head = node;
        return;
    }

    iter = *head;
    while (iter->LINE_plistNext) {                                      /*  找到尾节点                  */
        iter = iter->LINE_plistNext;
    }
    iter->LINE_plistNext = node;
    node->LINE_plistPrev = iter;
}
/*********************************************************************************************************
** 函数名称: _q_del
** 功能描述: 从链表中删除指定节点
** 输　入  : head          链表头指针的指针
**           node          待删除节点
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
static LW_INLINE VOID _q_del (PLW_LIST_LINE *head, PLW_LIST_LINE node)
{
    if (node->LINE_plistPrev) {                                         /*  非头节点                    */
        node->LINE_plistPrev->LINE_plistNext = node->LINE_plistNext;
    } else {
        *head = node->LINE_plistNext;                                   /*  头节点，更新头指针          */
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
** 功能描述: 初始化 DMA 引擎核心：初始化全局 spinlock，重置设备链表。
**           须在 module_init() 中最先调用，且只调用一次。
** 输　入  : NONE
** 输　出  : 0（始终成功）
** 全局变量: _G_core_lock, _G_dev_list, _G_core_inited
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
** 功能描述: 退出 DMA 引擎核心。
**           调用前须确保所有设备已通过 dma_device_unregister() 注销。
** 输　入  : NONE
** 输　出  : NONE
** 全局变量: _G_core_inited
** 调用模块:
*********************************************************************************************************/
void dma_core_exit (void)
{
    _G_core_inited = LW_FALSE;
}

/*********************************************************************************************************
  设备注册 / 注销
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_device_register
** 功能描述: 将控制器设备加入全局设备链表，使其可被 dma_find_device() 找到。
**           通常由控制器驱动的 probe 函数调用。
** 输　入  : dev           dma_device 指针（name、ops 等字段须已填充）
** 输　出  : 0 成功；-1 参数无效
** 全局变量: _G_dev_list, _G_core_lock
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
** 功能描述: 将控制器设备从全局设备链表移除。
**           通常由控制器驱动的 remove 函数调用。
** 输　入  : dev           dma_device 指针
** 输　出  : NONE
** 全局变量: _G_dev_list, _G_core_lock
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
** 功能描述: 按名称查找已注册的 DMA 控制器设备
** 输　入  : name          设备名称字符串
** 输　出  : 找到时返回 dma_device 指针；未找到返回 NULL
** 全局变量: _G_dev_list, _G_core_lock
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
** 函数名称: dma_core_alloc_chan
** 功能描述: 在指定设备上寻找第一个空闲通道并标记为已占用。
** 输　入  : dev           目标设备指针
** 输　出  : 成功返回通道指针；无空闲通道返回 NULL
** 全局变量:
** 调用模块:
*********************************************************************************************************/
struct dma_chan *dma_core_alloc_chan (struct dma_device *dev)
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
        if (!chan->in_use) {
            chan->in_use    = LW_TRUE;
            chan->pending_q = LW_NULL;
            chan->active_q  = LW_NULL;
            LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
            return  (chan);
        }
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
    }

    return  (LW_NULL);
}
/*********************************************************************************************************
** 函数名称: dma_core_free_chan
** 功能描述: 释放通道：先排空队列（发出错误回调），再标记为空闲。
** 输　入  : chan          待释放的通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_core_free_chan (struct dma_chan *chan)
{
    if (!chan) {
        return;
    }

    dma_core_handle_error(chan);                                        /*  排空队列                    */
    chan->in_use = LW_FALSE;
}

/*********************************************************************************************************
  描述符生命周期
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_desc_alloc
** 功能描述: 分配并初始化一个空描述符，绑定到指定通道。
**           hw_desc 由 ops->prep_simple / ops->prep_sg 在后续填充。
** 输　入  : chan          目标通道指针
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
    desc->status = DMA_STATUS_IDLE;

    return  (desc);
}
/*********************************************************************************************************
** 函数名称: dma_desc_free
** 功能描述: 释放描述符及其关联的硬件资源（hw_desc）。
**           通过 ops->desc_free 回调将硬件资源清理委托给控制器驱动。
**           须在完成回调返回后（desc 不在任何队列中）才可调用。
** 输　入  : desc          待释放的描述符指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_desc_free (struct dma_desc *desc)
{
    if (!desc) {
        return;
    }

    if (desc->chan && desc->chan->dev &&
        desc->chan->dev->ops && desc->chan->dev->ops->desc_free) {
        desc->chan->dev->ops->desc_free(desc);                          /*  委托硬件层清理 hw_desc      */
    }

    free(desc);
}

/*********************************************************************************************************
  调度
*********************************************************************************************************/
/*********************************************************************************************************
** 函数名称: dma_core_submit
** 功能描述: 将准备好的描述符加入通道的 pending 队列。
**           调用后须通过 dma_core_issue_pending() 触发调度才会实际启动硬件。
** 输　入  : desc          已由 ops->prep_* 填充的描述符指针
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

    desc->status = DMA_STATUS_PENDING;

    LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
    _q_add_tail(&chan->pending_q, &desc->node);
    LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

    return  (0);
}
/*********************************************************************************************************
** 函数名称: dma_core_schedule
** 功能描述: 内部调度函数。
**           若 active_q 为空且 pending_q 非空，则将 pending_q 首描述符移至 active_q，
**           调用 ops->issue_pending() 启动硬件传输。
**           可在任务上下文和 ISR 上下文中安全调用。
** 输　入  : chan          目标通道指针
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

    if (chan->active_q != LW_NULL || chan->pending_q == LW_NULL) {      /*  active 非空或无 pending     */
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
        return;
    }

    desc = _LIST_ENTRY(chan->pending_q, struct dma_desc, node);         /*  取 pending 队头             */
    _q_del(&chan->pending_q, &desc->node);
    _q_add_tail(&chan->active_q, &desc->node);                          /*  移入 active 队列            */
    desc->status = DMA_STATUS_ACTIVE;

    LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

    if (chan->dev->ops->issue_pending) {
        chan->dev->ops->issue_pending(chan);                             /*  启动硬件（仅寄存器写，安全）*/
    }
}
/*********************************************************************************************************
** 函数名称: dma_core_issue_pending
** 功能描述: 对外的调度触发接口，内部委托给 dma_core_schedule()
** 输　入  : chan          目标通道指针
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_core_issue_pending (struct dma_chan *chan)
{
    dma_core_schedule(chan);
}
/*********************************************************************************************************
** 函数名称: dma_core_complete
** 功能描述: 传输完成处理函数，由硬件 ISR 调用。
**           将描述符从 active_q 移除，更新状态，调用完成回调，
**           释放描述符资源，然后调度下一个 pending 描述符。
**           警告：在中断上下文执行，回调必须轻量级（如 API_SemaphoreBPost），禁止阻塞。
** 输　入  : chan          通道指针
**           desc          已完成的描述符指针
**           status        传输结果 DMA_STATUS_COMPLETE 或 DMA_STATUS_ERROR
** 输　出  : NONE
** 全局变量:
** 调用模块:
*********************************************************************************************************/
void dma_core_complete (struct dma_chan *chan, struct dma_desc *desc, int status)
{
    dma_complete_cb_t  cb;
    void              *cb_arg;
    INTREG             ireg;

    if (!chan || !desc) {
        return;
    }

    cb     = desc->cb;
    cb_arg = desc->cb_arg;

    LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
    _q_del(&chan->active_q, &desc->node);                               /*  移出 active 队列            */
    desc->status = status;
    LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

    dma_desc_free(desc);                                                /*  先释放资源，再唤醒用户线程  */
                                                                        /*  防止用户线程与 desc_free    */
                                                                        /*  并发访问 DMA zone 分配器    */
    dma_core_schedule(chan);                                            /*  调度下一个 pending 描述符   */

    if (cb) {
        cb(cb_arg, status);                                             /*  最后唤醒用户线程（ISR 安全）*/
    }
}
/*********************************************************************************************************
** 函数名称: dma_core_handle_error
** 功能描述: 错误恢复：停止通道，逐一排空 active_q 和 pending_q，
**           对每个描述符调用 DMA_STATUS_ERROR 回调后释放资源。
** 输　入  : chan          目标通道指针
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

    if (chan->dev->ops && chan->dev->ops->chan_stop) {
        chan->dev->ops->chan_stop(chan);                                 /*  停止硬件通道                */
    }

    for (;;) {                                                          /*  排空 active 队列            */
        LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
        if (chan->active_q == LW_NULL) {
            LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
            break;
        }
        desc = _LIST_ENTRY(chan->active_q, struct dma_desc, node);
        _q_del(&chan->active_q, &desc->node);
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

        if (desc->cb) desc->cb(desc->cb_arg, DMA_STATUS_ERROR);
        dma_desc_free(desc);
    }

    for (;;) {                                                          /*  排空 pending 队列           */
        LW_SPIN_LOCK_QUICK(&chan->lock, &ireg);
        if (chan->pending_q == LW_NULL) {
            LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);
            break;
        }
        desc = _LIST_ENTRY(chan->pending_q, struct dma_desc, node);
        _q_del(&chan->pending_q, &desc->node);
        LW_SPIN_UNLOCK_QUICK(&chan->lock, ireg);

        if (desc->cb) desc->cb(desc->cb_arg, DMA_STATUS_ERROR);
        dma_desc_free(desc);
    }
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

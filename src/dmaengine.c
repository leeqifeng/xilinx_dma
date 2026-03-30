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
** 文   件   名: dmaengine.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 25 日
**
** 描        述: SylixOS DMA Engine 框架核心实现
**               维护全局设备链表，提供通道申请/释放、事务提交/触发、状态查询等接口。
**
*********************************************************************************************************/
#define  __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <stdlib.h>
#include <string.h>
#include "dmaengine.h"

/*********************************************************************************************************
  全局状态
*********************************************************************************************************/

static PLW_LIST_LINE    _G_dma_device_list  = LW_NULL;                  /*  已注册的 DMA 控制器链表     */
static spinlock_t       _G_dma_lock;                                    /*  保护 _G_dma_device_list     */
static BOOL             _G_dma_initialized  = LW_FALSE;

/*********************************************************************************************************
** 函数名称: dma_engine_init
** 功能描述: 初始化 DMA Engine 框架（在 module_init 最早期调用）
** 输　入  : NONE
** 输　出  : 0 成功
*********************************************************************************************************/

INT  dma_engine_init (VOID)
{
    if (_G_dma_initialized) {
        return  (0);
    }

    LW_SPIN_INIT(&_G_dma_lock);
    _G_dma_device_list = LW_NULL;
    _G_dma_initialized = LW_TRUE;

    printk("[dmaengine] framework initialized\n");
    return  (0);
}

/*********************************************************************************************************
** 函数名称: dma_engine_exit
** 功能描述: 反初始化 DMA Engine 框架（在 module_exit 最后调用）
** 输　入  : NONE
** 输　出  : NONE
** 注意    : 调用前须保证所有设备已通过 dma_async_device_unregister 注销，所有通道已释放。
*********************************************************************************************************/

VOID  dma_engine_exit (VOID)
{
    _G_dma_initialized = LW_FALSE;
    printk("[dmaengine] framework exited\n");
}

/*********************************************************************************************************
** 函数名称: dma_async_device_register
** 功能描述: 将 DMA 控制器注册到框架（由硬件驱动在初始化时调用）
** 输　入  : device  — 已填充好 ops、dev_name、chancnt、channels 的控制器结构指针
** 输　出  : 0 成功；-1 失败（ops 缺失或未初始化）
** 说明    : 注册只把设备加入全局链表，不分配通道硬件资源（通道资源在 dma_request_channel 时分配）。
*********************************************************************************************************/

INT  dma_async_device_register (dma_device_t *device)
{
    INTREG  ireg;

    if (!device) {
        return  (-1);
    }

    if (!_G_dma_initialized) {
        printk("[dmaengine] ERROR: framework not initialized\n");
        return  (-1);
    }

    /*
     *  校验必须实现的 ops
     */
    if (!device->device_alloc_chan_resources ||
        !device->device_free_chan_resources  ||
        !device->device_issue_pending        ||
        !device->device_tx_status) {
        printk("[dmaengine] ERROR: device '%s' missing required ops\n", device->dev_name);
        return  (-1);
    }

    _LIST_LINE_INIT_IN_CODE(device->node);

    LW_SPIN_LOCK_IRQ(&_G_dma_lock, &ireg);
    _List_Line_Add_Ahead(&device->node, &_G_dma_device_list);
    LW_SPIN_UNLOCK_IRQ(&_G_dma_lock, ireg);

    printk("[dmaengine] registered device '%s' chancnt=%u\n",
           device->dev_name, device->chancnt);
    return  (0);
}

/*********************************************************************************************************
** 函数名称: dma_async_device_unregister
** 功能描述: 从框架注销 DMA 控制器（由硬件驱动在退出时调用）
** 输　入  : device  — 已注册的控制器结构指针
** 输　出  : NONE
** 注意    : 调用前须保证该控制器的所有通道已通过 dma_release_channel 释放。
*********************************************************************************************************/

VOID  dma_async_device_unregister (dma_device_t *device)
{
    INTREG  ireg;

    if (!device) {
        return;
    }

    LW_SPIN_LOCK_IRQ(&_G_dma_lock, &ireg);
    _List_Line_Del(&device->node, &_G_dma_device_list);
    LW_SPIN_UNLOCK_IRQ(&_G_dma_lock, ireg);

    printk("[dmaengine] unregistered device '%s'\n", device->dev_name);
}

/*********************************************************************************************************
** 函数名称: dma_request_channel
** 功能描述: 按事务类型 + 过滤函数申请一个 DMA 通道
** 输　入  : type         — 所需的事务类型（DMA_MEMCPY / DMA_SLAVE / DMA_CYCLIC）
**           filter       — 过滤函数（可为 LW_NULL，表示接受任意通道）
**           filter_param — 传给 filter 的参数
** 输　出  : 成功返回通道指针；失败返回 LW_NULL
** 说明    : 遍历全局设备链表，对每个通道调用 filter；找到第一个匹配的通道后，
**           调用 device_alloc_chan_resources 初始化该通道并返回。
**           本实现不追踪通道占用状态，由驱动的 alloc_chan_resources 保证幂等性。
*********************************************************************************************************/

dma_chan_t  *dma_request_channel (dma_transaction_type_t   type,
                                   dma_filter_fn            filter,
                                   PVOID                    filter_param)
{
    PLW_LIST_LINE   pdev_node;
    INTREG          ireg;

    (VOID)type;                                                         /*  当前版本仅用 filter 筛选    */

    LW_SPIN_LOCK_IRQ(&_G_dma_lock, &ireg);

    for (pdev_node = _G_dma_device_list;
         pdev_node != LW_NULL;
         pdev_node = _list_line_get_next(pdev_node)) {

        dma_device_t  *dev       = _LIST_ENTRY(pdev_node, dma_device_t, node);
        PLW_LIST_LINE  pchan_node;

        for (pchan_node = dev->channels;
             pchan_node != LW_NULL;
             pchan_node = _list_line_get_next(pchan_node)) {

            dma_chan_t  *chan = _LIST_ENTRY(pchan_node, dma_chan_t, node);

            if (filter && !filter(chan, filter_param)) {
                continue;
            }

            LW_SPIN_UNLOCK_IRQ(&_G_dma_lock, ireg);

            if (dev->device_alloc_chan_resources(chan) < 0) {
                printk("[dmaengine] alloc_chan_resources failed on '%s'\n",
                       chan->chan_name);
                return  LW_NULL;
            }
            return  chan;
        }
    }

    LW_SPIN_UNLOCK_IRQ(&_G_dma_lock, ireg);
    printk("[dmaengine] no matching channel found\n");
    return  LW_NULL;
}

/*********************************************************************************************************
** 函数名称: dma_request_chan_by_name
** 功能描述: 按设备名 + 通道索引申请 DMA 通道（嵌入式场景推荐用法）
** 输　入  : dev_name — 控制器名称（与 dma_device_t.dev_name 匹配）
**           idx      — 通道索引（0-based）
** 输　出  : 成功返回通道指针；失败返回 LW_NULL
*********************************************************************************************************/

dma_chan_t  *dma_request_chan_by_name (CPCHAR dev_name, UINT idx)
{
    PLW_LIST_LINE   pdev_node;
    INTREG          ireg;

    LW_SPIN_LOCK_IRQ(&_G_dma_lock, &ireg);

    for (pdev_node = _G_dma_device_list;
         pdev_node != LW_NULL;
         pdev_node = _list_line_get_next(pdev_node)) {

        dma_device_t  *dev = _LIST_ENTRY(pdev_node, dma_device_t, node);

        if (lib_strcmp(dev->dev_name, dev_name) != 0) {
            continue;
        }

        /* 找到设备，按索引定位通道 */
        {
            PLW_LIST_LINE  pchan_node;
            UINT           i = 0;

            for (pchan_node = dev->channels;
                 pchan_node != LW_NULL;
                 pchan_node = _list_line_get_next(pchan_node), i++) {

                if (i == idx) {
                    dma_chan_t  *chan = _LIST_ENTRY(pchan_node, dma_chan_t, node);

                    LW_SPIN_UNLOCK_IRQ(&_G_dma_lock, ireg);

                    if (dev->device_alloc_chan_resources(chan) < 0) {
                        printk("[dmaengine] alloc_chan_resources failed on '%s'\n",
                               chan->chan_name);
                        return  LW_NULL;
                    }
                    return  chan;
                }
            }
        }

        printk("[dmaengine] device '%s' has no channel idx=%u\n", dev_name, idx);
        break;
    }

    LW_SPIN_UNLOCK_IRQ(&_G_dma_lock, ireg);
    printk("[dmaengine] device '%s' not found\n", dev_name);
    return  LW_NULL;
}

/*********************************************************************************************************
** 函数名称: dma_release_channel
** 功能描述: 释放已申请的 DMA 通道
** 输　入  : chan — 通过 dma_request_channel/dma_request_chan_by_name 获取的通道指针
** 输　出  : NONE
** 说明    : 内部调用 device_free_chan_resources，驱动应在此停止 worker 线程并释放资源。
*********************************************************************************************************/

VOID  dma_release_channel (dma_chan_t *chan)
{
    if (!chan || !chan->device) {
        return;
    }
    chan->device->device_free_chan_resources(chan);
}

/*********************************************************************************************************
** 函数名称: dmaengine_submit
** 功能描述: 将描述符加入通道的 pending 队列，返回 cookie
** 输　入  : desc — 由 device_prep_xxx 准备好的描述符（已设置 callback）
** 输　出  : 正数 cookie 表示成功；DMA_COOKIE_INVALID(-1) 表示失败
** 说明    : 本函数仅将描述符入队，不触发传输；须调用 dma_async_issue_pending 触发。
**           内部调用 desc->tx_submit，具体队列操作由驱动实现。
*********************************************************************************************************/

dma_cookie_t  dmaengine_submit (dma_async_tx_descriptor_t *desc)
{
    if (!desc || !desc->tx_submit) {
        return  DMA_COOKIE_INVALID;
    }
    return  desc->tx_submit(desc);
}

/*********************************************************************************************************
** 函数名称: dma_async_issue_pending
** 功能描述: 触发通道开始执行 pending 队列中的传输
** 输　入  : chan — 目标通道
** 输　出  : NONE
** 说明    : 对应 Linux dma_async_issue_pending()，调用驱动 device_issue_pending。
**           通常在 dmaengine_submit 之后立即调用（或批量提交后统一调用一次）。
*********************************************************************************************************/

VOID  dma_async_issue_pending (dma_chan_t *chan)
{
    if (!chan || !chan->device || !chan->device->device_issue_pending) {
        return;
    }
    chan->device->device_issue_pending(chan);
}

/*********************************************************************************************************
** 函数名称: dmaengine_tx_status
** 功能描述: 查询指定 cookie 对应传输的当前状态
** 输　入  : chan   — 目标通道
**           cookie — 由 dmaengine_submit 返回的 cookie
**           state  — 输出状态（可为 LW_NULL）
** 输　出  : dma_status_t
*********************************************************************************************************/

dma_status_t  dmaengine_tx_status (dma_chan_t     *chan,
                                    dma_cookie_t   cookie,
                                    dma_tx_state_t *state)
{
    if (!chan || !chan->device || !chan->device->device_tx_status) {
        return  DMA_ERROR;
    }
    return  chan->device->device_tx_status(chan, cookie, state);
}

/*********************************************************************************************************
** 函数名称: dma_sync_wait
** 功能描述: 同步等待指定 cookie 对应的传输完成（轮询方式，有超时保护）
** 输　入  : chan   — 目标通道
**           cookie — 由 dmaengine_submit 返回的 cookie
** 输　出  : DMA_COMPLETE / DMA_ERROR（超时时返回 DMA_ERROR）
** 注意    : 优先推荐在描述符上设置 callback_result 以异步等待，避免占用调用线程。
**           本函数适用于不便注册回调的简单场景。
*********************************************************************************************************/

dma_status_t  dma_sync_wait (dma_chan_t *chan, dma_cookie_t cookie)
{
    dma_status_t  status;
    INT           retry = 10000;                                        /*  最长约 10 s（1 tick ≈ 1 ms）*/

    while (retry-- > 0) {
        status = dmaengine_tx_status(chan, cookie, LW_NULL);
        if (status != DMA_IN_PROGRESS) {
            return  status;
        }
        API_TimeSleep(1);                                               /*  让出 CPU，使 worker 有机会运行*/
    }

    printk("[dmaengine] WARNING: dma_sync_wait timed out (cookie=%d)\n", cookie);
    return  DMA_ERROR;
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

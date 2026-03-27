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
** 文   件   名: dmaengine.h
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 25 日
**
** 描        述: SylixOS DMA Engine 框架核心头文件
**               参照 Linux DMAengine 子系统设计，提供统一的 DMA 抽象层：
**                 - dma_device_t               : DMA 控制器（由硬件驱动实现并注册）
**                 - dma_chan_t                 : DMA 通道
**                 - dma_async_tx_descriptor_t  : 异步传输描述符
**               使用方需在包含本头文件之前完成 #define __SYLIXOS_KERNEL 和 #include <SylixOS.h>。
**
*********************************************************************************************************/
#ifndef __DMA_ENGINE_H
#define __DMA_ENGINE_H

/*********************************************************************************************************
  DMA Cookie 类型（单调递增的事务 ID，用于追踪传输完成状态）
*********************************************************************************************************/
typedef INT                             dma_cookie_t;

#define DMA_MIN_COOKIE                  1
#define DMA_MAX_COOKIE                  0x7fffffff
#define DMA_COOKIE_INVALID              (-1)

/*********************************************************************************************************
  DMA 传输方向
*********************************************************************************************************/
typedef enum dma_transfer_direction {
    DMA_MEM_TO_MEM  = 0,                                                /*  内存 → 内存 (memcpy)        */
    DMA_MEM_TO_DEV  = 1,                                                /*  内存 → 外设 (TX)            */
    DMA_DEV_TO_MEM  = 2,                                                /*  外设 → 内存 (RX)            */
    DMA_DEV_TO_DEV  = 3,                                                /*  外设 → 外设                 */
} dma_transfer_direction_t;

/*********************************************************************************************************
  DMA 事务类型（描述控制器具备的能力）
*********************************************************************************************************/
typedef enum dma_transaction_type {
    DMA_MEMCPY      = 0,                                                /*  内存拷贝                    */
    DMA_SLAVE       = 1,                                                /*  Slave 模式（外设 DMA）      */
    DMA_CYCLIC      = 2,                                                /*  循环传输                    */
} dma_transaction_type_t;

/*********************************************************************************************************
  DMA 事务状态
*********************************************************************************************************/
typedef enum dma_status {
    DMA_COMPLETE    = 0,                                                /*  传输完成                    */
    DMA_IN_PROGRESS = 1,                                                /*  传输进行中                  */
    DMA_PAUSED      = 2,                                                /*  已暂停                      */
    DMA_ERROR       = 3,                                                /*  传输出错                    */
} dma_status_t;

/*********************************************************************************************************
  DMA Slave 总线位宽
*********************************************************************************************************/
typedef enum dma_slave_buswidth {
    DMA_SLAVE_BUSWIDTH_UNDEFINED = 0,
    DMA_SLAVE_BUSWIDTH_1_BYTE    = 1,
    DMA_SLAVE_BUSWIDTH_2_BYTES   = 2,
    DMA_SLAVE_BUSWIDTH_4_BYTES   = 4,
    DMA_SLAVE_BUSWIDTH_8_BYTES   = 8,
} dma_slave_buswidth_t;

/*********************************************************************************************************
  描述符标志位（传入 device_prep_xxx 的 flags 参数）
*********************************************************************************************************/
#define DMA_PREP_INTERRUPT              (1UL << 0)                      /*  传输完成后触发回调          */
#define DMA_CTRL_ACK                    (1UL << 1)                      /*  驱动负责描述符生命期管理    */
#define DMA_PREP_FENCE                  (1UL << 2)                      /*  本描述符执行前须完成 fence  */

/*********************************************************************************************************
  离散-聚集（Scatter-Gather）条目
*********************************************************************************************************/
typedef struct dma_sg_entry {
    phys_addr_t                         addr;                           /*  物理起始地址                */
    size_t                              len;                            /*  本段长度（字节）            */
} dma_sg_entry_t;

/*********************************************************************************************************
  DMA 事务完成结果（用于 callback_result）
*********************************************************************************************************/
typedef struct dmaengine_result {
    dma_status_t                        result;                         /*  完成状态                    */
    UINT32                              residue;                        /*  未传输字节数（0 表示全部完成）*/
} dmaengine_result_t;

/*********************************************************************************************************
  DMA 事务状态查询结果（用于 dmaengine_tx_status）
*********************************************************************************************************/
typedef struct dma_tx_state {
    dma_cookie_t                        last;                           /*  最后一个完成的 cookie       */
    dma_cookie_t                        used;                           /*  最后一个提交的 cookie       */
    UINT32                              residue;                        /*  未传输字节数                */
} dma_tx_state_t;

/*********************************************************************************************************
  DMA Slave 通道配置（传入 device_config）
*********************************************************************************************************/
typedef struct dma_slave_config {
    dma_transfer_direction_t            direction;
    phys_addr_t                         src_addr;                       /*  外设源寄存器地址            */
    phys_addr_t                         dst_addr;                       /*  外设目标寄存器地址          */
    dma_slave_buswidth_t                src_addr_width;
    dma_slave_buswidth_t                dst_addr_width;
    UINT32                              src_maxburst;                   /*  最大突发读传输次数          */
    UINT32                              dst_maxburst;                   /*  最大突发写传输次数          */
} dma_slave_config_t;

/*********************************************************************************************************
  回调函数类型
*********************************************************************************************************/
typedef VOID (*dma_async_tx_callback)(PVOID param);
typedef VOID (*dma_async_tx_callback_result)(PVOID param,
                                              const dmaengine_result_t *result);

/*********************************************************************************************************
  前向声明
*********************************************************************************************************/
struct dma_chan;
struct dma_device;

/*********************************************************************************************************
  DMA 异步传输描述符
**
**  生命期：
**    1. 驱动通过 device_prep_xxx() 分配并初始化描述符，返回给用户。
**    2. 用户设置 callback/callback_result 后，调用 dmaengine_submit() 提交。
**    3. 调用 dma_async_issue_pending() 触发驱动执行。
**    4. 驱动执行完成后调用回调，并负责释放描述符（设置 DMA_CTRL_ACK 时）。
**
**  驱动程序将此结构嵌入到自己的私有描述符中，通过 _LIST_ENTRY / container_of 取回私有数据。
**
*********************************************************************************************************/
typedef struct dma_async_tx_descriptor {
    dma_cookie_t                        cookie;                         /*  本描述符 cookie（提交后赋值）*/
    ULONG                               flags;                          /*  DMA_PREP_xxx 标志           */
    struct dma_chan                     *chan;                           /*  所属通道                    */
    /*  驱动实现：将描述符加入 pending 队列，返回 cookie
     *  由 dmaengine_submit() 调用
     */
    dma_cookie_t                        (*tx_submit)(struct dma_async_tx_descriptor *tx);
    dma_async_tx_callback               callback;                       /*  简单完成回调（无结果）      */
    PVOID                               callback_param;                 /*  回调参数                    */
    dma_async_tx_callback_result        callback_result;                /*  带结果回调（优先于 callback）*/
    LW_LIST_LINE                        node;                           /*  驱动内部 pending/active 节点*/
} dma_async_tx_descriptor_t;

/*********************************************************************************************************
  DMA 通道
**
**  驱动程序将此结构作为私有通道结构的第一个成员（或用 _LIST_ENTRY 取回私有数据）。
**  框架通过 dma_chan_t * 操作通道；驱动通过 _LIST_ENTRY 拿到私有结构。
**
*********************************************************************************************************/
typedef struct dma_chan {
    struct dma_device                  *device;                         /*  所属 DMA 控制器             */
    dma_cookie_t                        cookie;                         /*  最后提交的 cookie           */
    dma_cookie_t                        completed_cookie;               /*  最后完成的 cookie           */
    CHAR                                chan_name[64];                  /*  通道名称（如 "demoip-dma-ch0"）*/
    LW_LIST_LINE                        node;                           /*  设备通道链表节点            */
    PVOID                               private;                        /*  驱动私有扩展指针            */
} dma_chan_t;

/*********************************************************************************************************
  DMA 设备（控制器）
**
**  硬件驱动负责：
**    1. 填充本结构（ops、dev_name、chancnt、channels 链表）。
**    2. 调用 dma_async_device_register() 注册到框架。
**
**  必须实现的 ops（缺少任意一个，注册将失败）：
**    device_alloc_chan_resources / device_free_chan_resources
**    device_issue_pending / device_tx_status
**
**  可选 ops（根据控制器能力实现）：
**    device_prep_dma_memcpy / device_prep_slave_sg / device_prep_dma_cyclic
**    device_config / device_pause / device_resume / device_terminate_all
**
*********************************************************************************************************/
typedef struct dma_device {
    UINT                                chancnt;                        /*  通道总数                    */
    PLW_LIST_LINE                       channels;                       /*  通道链表头（dma_chan_t.node）*/
    CHAR                                dev_name[32];                   /*  控制器名称                  */
    spinlock_t                          lock;                           /*  保护 channels 链表          */
    LW_LIST_LINE                        node;                           /*  全局设备链表节点            */

    /*
     *  必须实现的 ops
     */
    INT  (*device_alloc_chan_resources)(dma_chan_t *chan);               /*  通道被申请时分配硬件资源    */
    VOID (*device_free_chan_resources) (dma_chan_t *chan);               /*  通道被释放时归还硬件资源    */
    VOID (*device_issue_pending)       (dma_chan_t *chan);               /*  将 pending 队列转入执行队列 */
    dma_status_t (*device_tx_status)   (dma_chan_t *chan,               /*  查询 cookie 对应的传输状态  */
                                         dma_cookie_t cookie,
                                         dma_tx_state_t *txstate);

    /*
     *  可选 ops
     */
    dma_async_tx_descriptor_t *(*device_prep_dma_memcpy)(              /*  准备 MEM_TO_MEM 传输        */
        dma_chan_t *chan,
        phys_addr_t dst, phys_addr_t src,
        size_t len, ULONG flags);

    dma_async_tx_descriptor_t *(*device_prep_slave_sg)(                /*  准备 Slave SG 传输          */
        dma_chan_t *chan,
        dma_sg_entry_t *sgl, UINT sg_len,
        dma_transfer_direction_t direction,
        ULONG flags, PVOID context);

    dma_async_tx_descriptor_t *(*device_prep_dma_cyclic)(              /*  准备循环传输                */
        dma_chan_t *chan,
        phys_addr_t buf_addr, size_t buf_len,
        size_t period_len,
        dma_transfer_direction_t direction,
        ULONG flags);

    INT  (*device_config)        (dma_chan_t *chan,                     /*  配置 Slave 通道参数         */
                                   dma_slave_config_t *config);
    INT  (*device_pause)         (dma_chan_t *chan);                    /*  暂停通道传输                */
    INT  (*device_resume)        (dma_chan_t *chan);                    /*  恢复通道传输                */
    INT  (*device_terminate_all) (dma_chan_t *chan);                    /*  终止所有传输并清队列        */
} dma_device_t;

/*********************************************************************************************************
  通道过滤函数类型（用于 dma_request_channel）
  返回 LW_TRUE 表示该通道符合要求。
*********************************************************************************************************/
typedef BOOL (*dma_filter_fn)(dma_chan_t *chan, PVOID filter_param);

/*================================================ 框架公开 API ==========================================*/

/*
 *  框架初始化 / 反初始化（在 module_init / module_exit 中调用）
 */
INT   dma_engine_init  (VOID);
VOID  dma_engine_exit  (VOID);

/*
 *  DMA 控制器注册 / 注销（由硬件驱动调用）
 */
INT   dma_async_device_register  (dma_device_t *device);
VOID  dma_async_device_unregister(dma_device_t *device);

/*
 *  通道申请（由使用者调用）
 *    dma_request_channel     : 按类型 + 过滤函数申请
 *    dma_request_chan_by_name: 按设备名 + 通道索引申请（推荐）
 */
dma_chan_t *dma_request_channel     (dma_transaction_type_t type,
                                      dma_filter_fn          filter,
                                      PVOID                  filter_param);
dma_chan_t *dma_request_chan_by_name (CPCHAR dev_name, UINT idx);
VOID        dma_release_channel      (dma_chan_t *chan);

/*
 *  事务提交与触发
 */
dma_cookie_t  dmaengine_submit       (dma_async_tx_descriptor_t *desc);
VOID          dma_async_issue_pending(dma_chan_t *chan);

/*
 *  状态查询
 */
dma_status_t  dmaengine_tx_status(dma_chan_t *chan,
                                    dma_cookie_t cookie,
                                    dma_tx_state_t *state);
dma_status_t  dma_sync_wait      (dma_chan_t *chan, dma_cookie_t cookie);

/*================================================ 框架 thin-wrapper inline =============================*/

/*
 *  以下 inline 函数对 device_prep_xxx 的 NULL 安全封装，
 *  对齐 Linux dmaengine_prep_dma_memcpy / dmaengine_prep_slave_sg 命名。
 */
static LW_INLINE dma_async_tx_descriptor_t *
dmaengine_prep_dma_memcpy (dma_chan_t *chan,
                             phys_addr_t dst, phys_addr_t src,
                             size_t len, ULONG flags)
{
    if (!chan || !chan->device || !chan->device->device_prep_dma_memcpy) {
        return  LW_NULL;
    }
    return  chan->device->device_prep_dma_memcpy(chan, dst, src, len, flags);
}

static LW_INLINE dma_async_tx_descriptor_t *
dmaengine_prep_slave_sg (dma_chan_t *chan,
                          dma_sg_entry_t *sgl, UINT sg_len,
                          dma_transfer_direction_t dir, ULONG flags)
{
    if (!chan || !chan->device || !chan->device->device_prep_slave_sg) {
        return  LW_NULL;
    }
    return  chan->device->device_prep_slave_sg(chan, sgl, sg_len, dir, flags, LW_NULL);
}

static LW_INLINE INT
dmaengine_slave_config (dma_chan_t *chan, dma_slave_config_t *config)
{
    if (!chan || !chan->device || !chan->device->device_config) {
        return  -1;
    }
    return  chan->device->device_config(chan, config);
}

static LW_INLINE INT
dmaengine_terminate_all (dma_chan_t *chan)
{
    if (!chan || !chan->device || !chan->device->device_terminate_all) {
        return  -1;
    }
    return  chan->device->device_terminate_all(chan);
}

/*================================================ Cookie 辅助（供驱动使用）==============================*/

/*
 *  初始化通道 cookie 计数
 */
static LW_INLINE VOID  dma_cookie_init (dma_chan_t *chan)
{
    chan->cookie           = DMA_MIN_COOKIE;
    chan->completed_cookie = DMA_MIN_COOKIE - 1;
}

/*
 *  为描述符分配下一个 cookie，并更新通道 cookie 计数
 *  需在驱动 tx_submit 中的自旋锁保护下调用
 */
static LW_INLINE dma_cookie_t  dma_cookie_assign (dma_async_tx_descriptor_t *tx)
{
    dma_chan_t   *chan   = tx->chan;
    dma_cookie_t  cookie = chan->cookie + 1;

    if (cookie < DMA_MIN_COOKIE) {
        cookie = DMA_MIN_COOKIE;
    }
    tx->cookie   = cookie;
    chan->cookie = cookie;
    return  cookie;
}

/*
 *  标记描述符对应的传输已完成
 *  需在传输完成处理中调用（通常在驱动 worker 线程中）
 */
static LW_INLINE VOID  dma_cookie_complete (dma_async_tx_descriptor_t *tx)
{
    if (tx->chan->completed_cookie < tx->cookie) {
        tx->chan->completed_cookie = tx->cookie;
    }
}

/*
 *  根据 cookie 查询传输状态
 *  驱动在 device_tx_status 实现中调用
 */
static LW_INLINE dma_status_t
dma_cookie_status (dma_chan_t *chan, dma_cookie_t cookie, dma_tx_state_t *state)
{
    dma_cookie_t  used     = chan->cookie;
    dma_cookie_t  complete = chan->completed_cookie;

    if (state) {
        state->last    = complete;
        state->used    = used;
        state->residue = 0;
    }

    if (complete >= cookie) {
        return  DMA_COMPLETE;
    }
    if (cookie <= used) {
        return  DMA_IN_PROGRESS;
    }
    return  DMA_ERROR;
}

#endif  /* __DMA_ENGINE_H */
/*********************************************************************************************************
  END
*********************************************************************************************************/

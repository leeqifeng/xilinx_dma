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
** 文   件   名: dma_types.h
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: DMA 引擎框架公共数据类型定义（对标 Linux dmaengine 接口）
**               本文件被所有层（核心层、客户端层、硬件驱动层）共同包含。
**
** 修改记录:
** 2026.03.24  重构：对齐 Linux dmaengine API，增加 ip_type、direction、cookie、
**             dma_vdma_config；dma_ops 重命名为 device_ 前缀风格；
**             删除旧 DMA_STATUS_* 整数常量，改用 enum dma_status。
**
*********************************************************************************************************/

#ifndef DMA_TYPES_H_
#define DMA_TYPES_H_

/*********************************************************************************************************
  DMA 传输方向
*********************************************************************************************************/

#define DMA_DIR_MM2S        0                                           /*  内存→流 (TX，AXI DMA)      */
#define DMA_DIR_S2MM        1                                           /*  流→内存 (RX，AXI DMA)      */
#define DMA_DIR_MEM2MEM     2                                           /*  内存→内存 (CDMA)            */
#define DMA_DIR_ANY         (-1)                                        /*  不指定方向（alloc 时通配）  */

/*********************************************************************************************************
  DMA IP 类型
*********************************************************************************************************/

#define DMA_IP_AXIDMA       0                                           /*  AXI DMA (PG021)             */
#define DMA_IP_CDMA         1                                           /*  AXI CDMA (PG034)            */
#define DMA_IP_VDMA         2                                           /*  AXI VDMA (PG020)            */
#define DMA_IP_MCDMA        3                                           /*  AXI MCDMA (PG288)           */

/*********************************************************************************************************
  传输状态（对标 Linux enum dma_status）
*********************************************************************************************************/

enum dma_status {
    DMA_COMPLETE    = 0,                                                /*  传输成功完成                */
    DMA_IN_PROGRESS = 1,                                                /*  传输进行中                  */
    DMA_PAUSED      = 2,                                                /*  传输已暂停                  */
    DMA_ERROR       = 3,                                                /*  传输发生错误                */
};

/*********************************************************************************************************
  Cookie（对标 Linux dma_cookie_t）
  dmaengine_submit() 为每个提交的 desc 分配唯一递增 cookie，调用方凭 cookie 查询状态。
*********************************************************************************************************/

typedef int dma_cookie_t;
#define DMA_MIN_COOKIE      1                                           /*  有效 cookie 从 1 开始       */

/*********************************************************************************************************
  prep 标志（对标 Linux dma_ctrl_flags）
*********************************************************************************************************/

#define DMA_PREP_INTERRUPT  (1UL << 0)                                  /*  完成时触发回调              */
#define DMA_CTRL_ACK        (1UL << 1)                                  /*  desc 处理完后可被驱动复用   */

/*********************************************************************************************************
  完成回调（对标 Linux dma_async_tx_callback）
  注意：回调在 defer 线程中执行；状态通过 dma_async_is_tx_complete() 查询，不在回调参数中传递。
*********************************************************************************************************/

typedef void (*dma_async_tx_callback)(void *param);

/*********************************************************************************************************
  分散聚集条目（虚拟地址，驱动内部转换物理地址）
*********************************************************************************************************/

struct dma_sg {
    void   *buf;                                                        /*  缓冲区虚拟地址              */
    size_t  len;                                                        /*  字节长度                    */
};

/*********************************************************************************************************
  VDMA 2D 传输配置（对标 Linux struct xilinx_vdma_config）
  用于 dmaengine_prep_interleaved_dma()
*********************************************************************************************************/

struct dma_vdma_config {
    UINT32  vsize;                                                      /*  垂直行数                    */
    UINT32  hsize;                                                      /*  每行字节数                  */
    UINT32  stride;                                                     /*  行间距（字节，含 padding）  */
    int     frm_cnt;                                                    /*  帧缓冲区数量（1~32）        */
    void   *frm_addrs[32];                                              /*  各帧缓冲区虚拟地址          */
    int     park_frm;                                                   /*  ≥0 park 到指定帧；-1=循环  */
};

/*********************************************************************************************************
  前向声明
*********************************************************************************************************/

struct dma_device;
struct dma_chan;
struct dma_desc;

/*********************************************************************************************************
  硬件操作表（对标 Linux struct dma_device 中的 device_* 函数指针）
  由控制器驱动实现，由核心层通过 chan->dev->ops 调用。
  device_prep_* 函数负责分配并填充 dma_desc，返回 NULL 表示失败。
  device_terminate_all 停止通道并排空所有队列。
  device_tx_status 可选；若为 NULL，客户端层通过 cookie 比较返回粗粒度状态。
*********************************************************************************************************/

struct dma_ops {
    /*  通道资源管理  */
    int  (*device_alloc_chan_resources)(struct dma_chan *chan);          /*  通道初始化（对应 chan_init） */
    void (*device_free_chan_resources )(struct dma_chan *chan);          /*  通道清理（停止+释放）       */

    /*  传输准备（分配并填充 dma_desc，设置 desc->free_hw_desc）  */
    struct dma_desc *(*device_prep_dma_memcpy)(                         /*  CDMA：M2M 连续拷贝          */
                        struct dma_chan *chan,
                        void *dst, const void *src, size_t len,
                        unsigned long flags);

    struct dma_desc *(*device_prep_slave_sg)(                           /*  AXI DMA / MCDMA：流式 SG    */
                        struct dma_chan *chan,                           /*  nents=1 覆盖原 Simple 模式  */
                        struct dma_sg *sgl, int nents,
                        unsigned long flags);

    struct dma_desc *(*device_prep_interleaved_dma)(                    /*  VDMA：2D 帧传输             */
                        struct dma_chan *chan,
                        const struct dma_vdma_config *cfg,
                        unsigned long flags);

    /*  触发 / 查询 / 终止  */
    int              (*device_issue_pending )(struct dma_chan *chan);
    enum dma_status  (*device_tx_status     )(struct dma_chan *chan, dma_cookie_t cookie);
    int              (*device_terminate_all )(struct dma_chan *chan);
};

/*********************************************************************************************************
  DMA 通道（对标 Linux struct dma_chan，由核心层管理）
*********************************************************************************************************/

struct dma_chan {
    struct dma_device   *dev;                                           /*  所属控制器                  */
    int                  id;                                            /*  通道编号                    */
    int                  direction;                                     /*  DMA_DIR_*（probe 时赋值）   */
    PLW_LIST_LINE        pending_q;                                     /*  待调度队列头                */
    PLW_LIST_LINE        active_q;                                      /*  传输中队列头                */
    spinlock_t           lock;                                          /*  队列 + cookie 保护锁        */
    BOOL                 in_use;                                        /*  是否已分配                  */
    dma_cookie_t         cookie;                                        /*  下一个可分配 cookie         */
    dma_cookie_t         completed_cookie;                              /*  最近已完成传输的 cookie     */
    enum dma_status      last_status;                                   /*  最近完成传输的状态          */
    void                *priv;                                          /*  硬件私有数据（AXI_DMA_CHAN）*/
};

/*********************************************************************************************************
  DMA 描述符（对标 Linux struct dma_async_tx_descriptor，一次传输请求）
  desc 由 device_prep_* 分配；dmaengine_submit() 赋 cookie；完成后由核心层自动释放。
  调用方在 dmaengine_submit() 返回后不得继续持有指针。
*********************************************************************************************************/

struct dma_desc {
    struct dma_chan          *chan;                                      /*  所属通道                    */
    void                     *hw_desc;                                  /*  硬件私有描述符（由 HW 层管理）*/
    void                    (*free_hw_desc)(struct dma_desc *desc);     /*  hw_desc 释放函数（HW 层设置）*/
    dma_async_tx_callback     callback;                                 /*  完成回调（调用方赋值）      */
    void                     *callback_param;                           /*  回调参数（调用方赋值）      */
    unsigned long             flags;                                    /*  DMA_PREP_* 标志             */
    dma_cookie_t              cookie;                                   /*  dmaengine_submit() 赋值     */
    enum dma_status           status;                                   /*  当前状态                    */
    LW_LIST_LINE              node;                                     /*  链入 pending_q / active_q   */
};

/*********************************************************************************************************
  DMA 控制器设备（一个 IP 实例，可含多个通道）
*********************************************************************************************************/

struct dma_device {
    const char          *name;                                          /*  设备名，供 dma_request_chan */
    int                  ip_type;                                       /*  DMA_IP_*                    */
    struct dma_ops      *ops;                                           /*  硬件操作表                  */
    struct dma_chan     *channels;                                      /*  通道数组                    */
    int                  num_channels;                                  /*  通道数量                    */
    LW_LIST_LINE         node;                                          /*  全局设备链表节点            */
    void                *priv;                                          /*  硬件私有设备数据            */
};

#endif                                                                  /*  DMA_TYPES_H_                */
/*********************************************************************************************************
  END
*********************************************************************************************************/


## AXI DMA RTOS 框架设计（分层 + 接口清单）

### 应用层 (Application)

直接使用 DMA 完成数据搬运，不关心底层硬件细节

#### 职责

- 发起 DMA 传输请求
- 提供回调函数处理完成事件
- 管理 buffer 生命周期

#### 接口定义

```c
typedef void (*dma_complete_cb_t)(void*arg, int status);

int app_dma_simple_transfer_async(void *dst, void*src, size_t len,
                         dma_complete_cb_t cb, void *arg);

int app_dma_sg_transfer_async(struct dma_sg *sg_list, int sg_len,
                              dma_complete_cb_t cb, void*arg);
```

### DMA Client API 层（面向设备驱动）

提供给各类驱动（如网卡、音频、存储）使用的统一 DMA API

#### 职责

- 封装 DMA 请求（channel / descriptor）
- 提供统一接口屏蔽 Simple/SG 差异
- 负责 buffer 映射（cache / 地址转换）

#### 核心数据结构

```c
struct dma_client;
struct dma_chan;
struct dma_desc;

struct dma_sg {
    void *buf;
    size_t len;
};
```

#### 接口清单

```c
/*通道管理*/
struct dma_chan *dma_request_chan(const char*name);
void dma_release_chan(struct dma_chan *chan);

/*异步传输*/
struct dma_desc *dma_prep_simple(struct dma_chan*chan,
                                 void *dst, void*src, size_t len,
                                 dma_complete_cb_t cb, void *arg);

struct dma_desc *dma_prep_sg(struct dma_chan*chan,
                            struct dma_sg *sgl, int sg_len,
                            dma_complete_cb_t cb, void*arg);

/*提交与启动*/
int dma_submit(struct dma_desc *desc);
int dma_issue_pending(struct dma_chan*chan);

/*状态控制*/
int dma_terminate(struct dma_chan *chan);
int dma_tx_status(struct dma_chan*chan, struct dma_desc *desc);
```

### DMA Engine / Core 层（调度 + 抽象）

框架核心，类似 Linux DMA Engine

#### 职责

- 统一调度不同 DMA 控制器
- 管理 channel / 队列 / descriptor 生命周期
- 异步执行 + 中断回调分发
- 支持 Simple / SG 模式统一抽象

#### 核心数据结构

```c
struct dma_device;
struct dma_chan {
    struct dma_device *dev;
    int id;
    queue_t pending_q;
    queue_t active_q;
};

struct dma_desc {
    struct dma_chan *chan;
    void*hw_desc;
    dma_complete_cb_t cb;
    void *cb_arg;
    int status;
};
```

#### 接口清单

```c
/*设备注册*/
int dma_device_register(struct dma_device *dev);
void dma_device_unregister(struct dma_device*dev);

/*通道分配*/
struct dma_chan *dma_core_alloc_chan(struct dma_device*dev);
void dma_core_free_chan(struct dma_chan *chan);

/*描述符管理*/
struct dma_desc *dma_desc_alloc(struct dma_chan*chan);
void dma_desc_free(struct dma_desc *desc);

/*调度*/
int dma_core_submit(struct dma_desc *desc);
void dma_core_issue_pending(struct dma_chan*chan);

/*完成处理（中断回调）*/
void dma_core_complete(struct dma_chan *chan, struct dma_desc*desc, int status);

/*调度器*/
void dma_core_schedule(struct dma_chan *chan);

/*错误处理*/
void dma_core_handle_error(struct dma_chan *chan);
```

### DMA 平台抽象层（Platform / Adapter）

屏蔽不同硬件（AXI DMA / 自定义 DMA）的差异

#### 职责

- 提供统一硬件操作接口（ops）
- 适配 Simple / SG 模式能力差异
- 抽象寄存器操作

#### 核心结构

```c
struct dma_ops {
    int (*chan_init)(struct dma_chan*chan);
    int (*chan_start)(struct dma_chan*chan);
    int (*chan_stop)(struct dma_chan*chan);

    int (*prep_simple)(struct dma_desc *desc,
                       void *dst, void *src, size_t len);

    int (*prep_sg)(struct dma_desc *desc,
                   struct dma_sg *sgl, int sg_len);

    int (*issue_pending)(struct dma_chan *chan);

    int (*tx_status)(struct dma_chan *chan, struct dma_desc *desc);

    void (*irq_handler)(struct dma_chan *chan);
};
```

#### 接口清单

```c
int dma_adapter_register(struct dma_device *dev, struct dma_ops*ops);

int dma_adapter_prep_simple(struct dma_desc *desc,
                           void*dst, void *src, size_t len);

int dma_adapter_prep_sg(struct dma_desc *desc,
                       struct dma_sg*sgl, int sg_len);

int dma_adapter_issue_pending(struct dma_chan *chan);

void dma_adapter_irq_handler(struct dma_chan *chan);
```

### DMA 控制器驱动层（AXI DMA / 硬件）

直接操作 AXI DMA 控制器（如 Xilinx AXI DMA）

#### 职责

- 配置寄存器（MM2S / S2MM）
- 构建 BD（Buffer Descriptor）链（SG 模式）
- 处理中断
- 管理 DMA 状态机

#### 核心结构

```c
struct axi_dma_chan {
    void *regs;
    int irq;
    int direction; // MM2S / S2MM
};

struct axi_dma_desc {
    void *bd_list;
    int bd_num;
};
```

#### 接口清单（实现 dma_ops）

```c
/*初始化*/
int axi_dma_chan_init(struct dma_chan *chan);

/*Simple 模式*/
int axi_dma_prep_simple(struct dma_desc *desc,
                        void*dst, void *src, size_t len);

/*SG 模式*/
int axi_dma_prep_sg(struct dma_desc *desc,
                    struct dma_sg*sgl, int sg_len);

/*启动*/
int axi_dma_start(struct dma_chan *chan);

/*停止*/
int axi_dma_stop(struct dma_chan *chan);

/*提交*/
int axi_dma_issue_pending(struct dma_chan *chan);

/*状态查询*/
int axi_dma_tx_status(struct dma_chan *chan, struct dma_desc*desc);

/*中断处理*/
void axi_dma_irq_handler(struct dma_chan *chan);

/*BD 管理*/
void axi_dma_bd_init(struct axi_dma_desc *adesc);
void axi_dma_bd_build_chain(struct axi_dma_desc*adesc,
                            struct dma_sg *sgl, int sg_len);
```

```bash  典型调用流程（异步）
Application
   ↓
dma_prep_xxx()
   ↓
dma_submit()
   ↓
dma_issue_pending()
   ↓
DMA Engine 调度
   ↓
Platform ops -> AXI DMA 启动
   ↓
硬件传输
   ↓
中断
   ↓
axi_dma_irq_handler()
   ↓
dma_core_complete()
   ↓
callback()
```

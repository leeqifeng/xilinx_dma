# Xilinx DMA Linux驱动分析文档

## 1. 驱动概述

### 1.1 支持的IP类型

Xilinx DMA驱动支持4种DMA IP核:

| IP类型 | 枚举值 | 功能描述 | 最大通道数 |
|--------|--------|----------|-----------|
| AXI DMA | XDMA_TYPE_AXIDMA | 一维DMA,支持MM2S/S2MM | 2 |
| AXI CDMA | XDMA_TYPE_CDMA | 内存到内存DMA | 1 |
| AXI VDMA | XDMA_TYPE_VDMA | 视频DMA,支持二维传输 | 2 |
| AXI MCDMA | XDMA_TYPE_AXIMCDMA | 多通道DMA,支持SG | 32 |

### 1.2 传输方向

- **MM2S** (Memory to Stream): 内存→外设
- **S2MM** (Stream to Memory): 外设→内存
- **MEM_TO_MEM**: 内存到内存(CDMA专用)

---

## 2. 核心数据结构

### 2.1 硬件描述符 (Hardware Descriptor)

#### 2.1.1 VDMA描述符 (xilinx_vdma_desc_hw)

```c
struct xilinx_vdma_desc_hw {
    u32 next_desc;        // 下一个描述符地址 @0x00
    u32 pad1;             // 保留 @0x04
    u32 buf_addr;         // 缓冲区地址 @0x08
    u32 buf_addr_msb;     // 缓冲区地址高32位 @0x0C
    u32 vsize;            // 垂直尺寸(行数) @0x10
    u32 hsize;            // 水平尺寸(每行字节数) @0x14
    u32 stride;           // 行间距(字节) @0x18
} __aligned(64);
```

**特点**: 支持二维视频帧传输,包含stride用于处理非连续内存布局。

#### 2.1.2 AXI DMA描述符 (xilinx_axidma_desc_hw)

```c
struct xilinx_axidma_desc_hw {
    u32 next_desc;        // 下一个描述符地址 @0x00
    u32 next_desc_msb;    // 下一个描述符地址高32位 @0x04
    u32 buf_addr;         // 缓冲区地址 @0x08
    u32 buf_addr_msb;     // 缓冲区地址高32位 @0x0C
    u32 reserved1;        // 保留 @0x10
    u32 reserved2;        // 保留 @0x14
    u32 control;          // 控制字段(传输长度+标志) @0x18
    u32 status;           // 状态字段(已传输长度) @0x1C
    u32 app[5];           // 应用字段 @0x20-0x30
} __aligned(64);
```

**关键字段**:
- `control`: 包含传输长度、SOP/EOP标志
- `status`: 硬件更新,表示已完成的字节数
- `app[5]`: 用户自定义数据,可传递给应用层

#### 2.1.3 MCDMA描述符 (xilinx_aximcdma_desc_hw)

```c
struct xilinx_aximcdma_desc_hw {
    u32 next_desc;
    u32 next_desc_msb;
    u32 buf_addr;
    u32 buf_addr_msb;
    u32 rsvd;
    u32 control;
    u32 status;
    u32 sideband_status;  // 边带信号状态
    u32 app[5];
} __aligned(64);
```

#### 2.1.4 CDMA描述符 (xilinx_cdma_desc_hw)

```c
struct xilinx_cdma_desc_hw {
    u32 next_desc;
    u32 next_desc_msb;
    u32 src_addr;         // 源地址 @0x08
    u32 src_addr_msb;
    u32 dest_addr;        // 目标地址 @0x10
    u32 dest_addr_msb;
    u32 control;          // 传输长度
    u32 status;
} __aligned(64);
```

**特点**: 同时包含源地址和目标地址,用于内存到内存拷贝。

---

### 2.2 软件描述符结构

#### 2.2.1 传输段 (TX Segment)

每种IP类型都有对应的段结构,包含硬件描述符、链表节点和物理地址:

```c
struct xilinx_axidma_tx_segment {
    struct xilinx_axidma_desc_hw hw;  // 硬件描述符
    struct list_head node;             // 链表节点
    dma_addr_t phys;                   // 物理地址
} __aligned(64);
```

#### 2.2.2 传输描述符 (xilinx_dma_tx_descriptor)

```c
struct xilinx_dma_tx_descriptor {
    struct dma_async_tx_descriptor async_tx;  // Linux DMAengine标准描述符
    struct list_head segments;                 // 段链表(支持SG)
    struct list_head node;                     // 在通道队列中的节点
    bool cyclic;                               // 是否循环传输
    bool err;                                  // 是否有错误
    u32 residue;                               // 剩余未传输字节数
};
```

**关键点**:
- `segments`: 支持scatter-gather,一个描述符可包含多个段
- `async_tx`: 嵌入Linux标准描述符,用于回调和依赖管理

---

### 2.3 通道结构 (xilinx_dma_chan)

```c
struct xilinx_dma_chan {
    struct xilinx_dma_device *xdev;    // 所属设备
    u32 ctrl_offset;                    // 控制寄存器偏移
    u32 desc_offset;                    // 描述符寄存器偏移
    spinlock_t lock;                    // 自旋锁

    // 描述符队列
    struct list_head pending_list;      // 待提交队列
    struct list_head active_list;       // 活动队列
    struct list_head done_list;         // 完成队列
    struct list_head free_seg_list;     // 空闲段池

    struct dma_chan common;             // Linux标准通道
    struct dma_pool *desc_pool;         // 描述符内存池
    int irq;                            // 中断号
    int id;                             // 通道ID
    enum dma_transfer_direction direction;  // 传输方向

    bool has_sg;                        // 是否支持SG
    bool cyclic;                        // 是否循环模式
    bool err;                           // 错误标志
    bool idle;                          // 空闲标志
    bool terminating;                   // 终止标志

    struct tasklet_struct tasklet;      // 底半部处理

    // 预分配的描述符
    struct xilinx_axidma_tx_segment *seg_v;      // 虚拟地址
    dma_addr_t seg_p;                             // 物理地址
    struct xilinx_axidma_tx_segment *cyclic_seg_v;  // 循环模式专用

    // 函数指针(不同IP类型实现不同)
    void (*start_transfer)(struct xilinx_dma_chan *chan);
    int (*stop_transfer)(struct xilinx_dma_chan *chan);
};
```

**队列管理**:

1. **pending_list**: 用户提交但未启动的描述符
2. **active_list**: 硬件正在处理的描述符
3. **done_list**: 已完成等待回调的描述符
4. **free_seg_list**: 预分配的空闲段池(AXIDMA/MCDMA使用)

### 2.4 设备结构 (xilinx_dma_device)

```c
struct xilinx_dma_device {
    void __iomem *regs;                 // 寄存器映射基地址
    struct device *dev;
    struct dma_device common;           // Linux标准DMA设备
    struct xilinx_dma_chan *chan[32];   // 通道数组(最多32个)

    bool ext_addr;                      // 是否支持64位地址
    const struct xilinx_dma_config *dma_config;  // IP配置

    // 时钟
    struct clk *axi_clk;                // AXI总线时钟
    struct clk *tx_clk;                 // MM2S时钟
    struct clk *rx_clk;                 // S2MM时钟

    u32 max_buffer_len;                 // 最大传输长度
};

```

## 3. 寄存器定义

### 3.1 控制寄存器 (DMACR - 0x00)

| 位域 | 名称 | 说明 |
|------|------|------|
| [0] | RUNSTOP | 运行/停止控制 |
| [1] | CIRC_EN | 循环模式使能 |
| [2] | RESET | 软复位 |
| [3] | GENLOCK_EN | 帧锁定使能 |
| [4] | FRAMECNT_EN | 帧计数使能 |
| [12] | FRM_CNT_IRQ | 帧计数中断使能 |
| [13] | DLY_CNT_IRQ | 延迟计数中断使能 |
| [14] | ERR_IRQ | 错误中断使能 |
| [23:16] | FRAME_COUNT | 帧计数值 |
| [31:24] | DELAY | 延迟计数值 |

### 3.2 状态寄存器 (DMASR - 0x04)

| 位域 | 名称 | 说明 |
|------|------|------|
| [0] | HALTED | 通道已停止 |
| [1] | IDLE | 通道空闲 |
| [3] | SG_MASK | SG模式指示 |
| [4] | DMA_INT_ERR | DMA内部错误 |
| [5] | DMA_SLAVE_ERR | Slave错误 |
| [6] | DMA_DEC_ERR | 解码错误 |
| [12] | FRM_CNT_IRQ | 帧计数中断 |
| [13] | DLY_CNT_IRQ | 延迟计数中断 |
| [14] | ERR_IRQ | 错误中断 |

### 3.3 描述符寄存器

| 寄存器 | 偏移 | 说明 |
|--------|------|------|
| CURDESC | 0x08 | 当前描述符地址 |
| TAILDESC | 0x10 | 尾描述符地址(启动传输) |

**关键**: 写TAILDESC寄存器触发DMA引擎开始处理描述符链。

---

## 4. 关键操作流程

### 4.1 通道资源分配 (alloc_chan_resources)

```
xilinx_dma_alloc_chan_resources()
├─ 检查desc_pool是否已分配
├─ 根据IP类型分配描述符内存:
│  ├─ AXIDMA: dma_alloc_coherent分配255个段 + 1个cyclic段
│  ├─ MCDMA: dma_alloc_coherent分配255个段
│  ├─ CDMA/VDMA: dma_pool_create创建内存池
│  └─ 构建循环链表(next_desc指向下一个)
├─ 初始化free_seg_list
├─ dma_cookie_init初始化cookie
└─ 使能中断(AXIDMA)
```

**内存分配策略**:

- **AXIDMA/MCDMA**: 预分配固定数量(255)的连续描述符,使用free_seg_list管理
- **CDMA/VDMA**: 使用dma_pool动态分配

### 4.2 准备传输 (prep函数)

以`device_prep_slave_sg`为例:

```
xilinx_dma_prep_slave_sg()
├─ xilinx_dma_alloc_tx_descriptor() 分配软件描述符
├─ 遍历scatterlist:
│  ├─ xilinx_axidma_alloc_tx_segment() 从free_seg_list获取段
│  ├─ 填充hw描述符:
│  │  ├─ buf_addr = sg_dma_address(sg)
│  │  ├─ control = sg_dma_len(sg) | SOP | EOP
│  │  └─ next_desc = 下一个段的物理地址
│  └─ list_add_tail(&segment->node, &desc->segments)
├─ 设置第一个段SOP,最后一个段EOP
└─ 返回&desc->async_tx
```

**关键点**:

- 硬件描述符形成物理链表(next_desc)
- 软件描述符通过segments链表管理所有段
- SOP/EOP标志标记包边界

### 4.3 提交传输 (tx_submit)

```c
dmaengine_submit()
├─ desc->tx_submit()
   ├─ dma_cookie_assign(desc) 分配cookie
   ├─ list_add_tail(&desc->node, &chan->pending_list)
   └─ 返回cookie
```

### 4.4 触发传输 (issue_pending)

```c
xilinx_dma_issue_pending()
├─ spin_lock_irqsave(&chan->lock)
├─ 将pending_list移动到active_list
├─ 调用chan->start_transfer()
│  └─ xilinx_dma_start_transfer() / xilinx_vdma_start_transfer()
│     ├─ 从active_list取第一个描述符
│     ├─ 写CURDESC寄存器(第一个段物理地址)
│     ├─ 设置DMACR寄存器(RUNSTOP=1)
│     └─ 写TAILDESC寄存器(最后一个段物理地址) → 启动DMA
└─ spin_unlock_irqrestore(&chan->lock)
```

**关键**: 写TAILDESC触发硬件开始从CURDESC遍历描述符链。

### 4.5 中断处理流程

```c
xilinx_dma_irq_handler() [硬中断]
├─ 读DMASR寄存器
├─ 检查中断类型:
│  ├─ ERR_IRQ: 设置chan->err标志
│  ├─ FRM_CNT_IRQ / DLY_CNT_IRQ: 正常完成
│  └─ 写DMASR清除中断标志
├─ 将active_list中完成的描述符移到done_list
└─ tasklet_schedule(&chan->tasklet) 调度底半部

xilinx_dma_do_tasklet() [底半部]
└─ xilinx_dma_chan_desc_cleanup()
   ├─ 遍历done_list
   ├─ 对每个描述符:
   │  ├─ 计算residue(未传输字节数)
   │  ├─ 调用dmaengine_desc_get_callback_invoke() 执行回调
   │  ├─ dma_run_dependencies() 处理依赖
   │  └─ xilinx_dma_free_tx_descriptor() 释放描述符
   └─ 将段归还到free_seg_list
```

**中断处理特点**:

- 硬中断只做最少工作(读状态、移队列、调度tasklet)
- 回调在tasklet中执行,避免阻塞硬中断
- 使用done_list作为硬中断和tasklet的缓冲

---

## 5. 关键技术点

### 5.1 Scatter-Gather支持

**硬件链表**:

```
段1.hw.next_desc → 段2物理地址
段2.hw.next_desc → 段3物理地址
段3.hw.next_desc → NULL
```

硬件自动遍历链表,无需软件干预。

**软件管理**:

```
desc->segments: [段1] → [段2] → [段3]
```

用于residue计算和错误处理。

### 5.2 循环模式 (Cyclic DMA)

用于音频/视频流:

```
段1 → 段2 → 段3 → 段1 (循环)
     ↑______________|
```

- 最后一个段的next_desc指向第一个段
- 每完成一个period触发一次中断
- 描述符不释放,持续循环

### 5.3 64位地址支持

```c
if (chan->ext_addr) {
    hw->buf_addr = lower_32_bits(addr);
    hw->buf_addr_msb = upper_32_bits(addr);
} else {
    hw->buf_addr = addr;  // 32位模式
}
```

通过设备树属性`xlnx,addrwidth`配置。

### 5.4 Residue计算

```c
residue = (hw->control - hw->status) & max_buffer_len
```

- `control`: 配置的传输长度
- `status`: 硬件更新的已传输长度
- 用于`dma_tx_status()`查询进度

---

## 6. Linux与SylixOS API映射

### 6.1 内存管理

| Linux API | SylixOS API | 说明 |
|-----------|-------------|------|
| `dma_alloc_coherent()` | `API_VmmDmaAlloc()` | 分配DMA一致性内存 |
| `dma_free_coherent()` | `API_VmmDmaFree()` | 释放DMA内存 |
| `dma_pool_create()` | 自行实现或使用`sys_malloc` | 内存池 |
| `dma_pool_alloc()` | `sys_malloc` | 从池分配 |
| `dma_pool_free()` | `sys_free` | 释放到池 |
| `kzalloc()` | `sys_malloc()` + `lib_bzero()` | 分配并清零 |
| `kfree()` | `sys_free()` | 释放内存 |

### 6.2 同步原语

| Linux API | SylixOS API | 说明 |
|-----------|-------------|------|
| `spin_lock_irqsave()` | `LW_SPIN_LOCK_IRQ()` | 自旋锁+关中断 |
| `spin_unlock_irqrestore()` | `LW_SPIN_UNLOCK_IRQ()` | 解锁+恢复中断 |
| `spin_lock_init()` | `LW_SPIN_INIT()` | 初始化自旋锁 |

### 6.3 链表操作

**SylixOS支持双向链表(LW_LIST_LINE)**:

```c
typedef struct __list_line {
    struct __list_line *LINE_plistNext;  // 前向指针
    struct __list_line *LINE_plistPrev;  // 后向指针
} LW_LIST_LINE;
```

| Linux API | SylixOS API | 说明 |
|-----------|-------------|------|
| `INIT_LIST_HEAD()` | `_LIST_LINE_INIT_IN_CODE()` | 初始化链表头 |
| `list_add_tail()` | `_List_Line_Add_Tail()` | 尾部添加 |
| `list_del()` | `_List_Line_Del()` | 删除节点 |
| `list_empty()` | `_LIST_LINE_IS_EMPTY()` | 判空 |
| `list_first_entry()` | `_LIST_ENTRY(head, type, member)` | 获取首元素 |
| `list_for_each_entry_safe()` | 遍历`_list_line_get_next()`+`_list_line_get_prev()` | 安全遍历 |
| `container_of()` | `_LIST_ENTRY()` | 从成员获取结构体 |

### 6.4 中断处理

| Linux API | SylixOS API | 说明 |
|-----------|-------------|------|
| `request_irq()` | `API_InterVectorConnect()` | 注册中断 |
| `free_irq()` | `API_InterVectorDisconnect()` | 注销中断 |
| `tasklet_init()` | `API_InterDeferJobAdd()` | 底半部(延迟作业) |
| `tasklet_schedule()` | `API_InterDeferJobAdd()` | 调度底半部 |

### 6.5 寄存器访问

**重要**: PL侧寄存器必须使用`API_VmmMap`平板映射,不能用DMA内存分配函数。

| Linux API | SylixOS API | 说明 |
|-----------|-------------|------|
| `devm_ioremap()` | `API_VmmMap()` | 映射PL寄存器空间(平板映射) |
| `ioread32()` | `read32()` | 读32位寄存器(含内存屏障) |
| `iowrite32()` | `write32()` | 写32位寄存器(含内存屏障) |
| `readl_poll_timeout()` | 手动循环+`API_TimeGet64()` | 轮询超时 |

**寄存器映射示例**:
```c
// Linux
xdev->regs = devm_ioremap_resource(&pdev->dev, res);

// SylixOS (PL寄存器必须用平板映射)
xdev->regs = API_VmmMap((PVOID)params->reg_base,
                        (PVOID)params->reg_base,
                        params->reg_size,
                        LW_VMM_FLAG_DMA);
```

---

## 7. 移植到SylixOS方案

### 7.1 整体架构对比

**Linux驱动架构**:

```
of_device_id匹配 → platform_driver.probe
    ↓
xilinx_dma_probe (主probe)
    ↓
xilinx_dma_child_probe (子节点probe)
    ↓
xilinx_dma_chan_probe (通道probe,循环创建)
    ↓
注册到DMAengine Framework
```

**SylixOS目标架构**:

```
静态xilinx_dma_config数组 → xilinx_dma_probe
    ↓
xilinx_dma_child_probe (子节点初始化)
    ↓
xilinx_dma_chan_probe (通道初始化,循环创建)
    ↓
注册到DMAengine Framework
```

### 7.2 Linux设备匹配机制

#### 7.2.1 of_device_id表

Linux通过设备树compatible字符串匹配驱动:

```c
static const struct of_device_id xilinx_dma_of_ids[] = {
    { .compatible = "xlnx,axi-dma-1.00.a",   .data = &axidma_config },
    { .compatible = "xlnx,axi-cdma-1.00.a",  .data = &axicdma_config },
    { .compatible = "xlnx,axi-vdma-1.00.a",  .data = &axivdma_config },
    { .compatible = "xlnx,axi-mcdma-1.00.a", .data = &aximcdma_config },
    {}
};
```

#### 7.2.2 xilinx_dma_config结构

每种IP类型有独立配置:

```c
struct xilinx_dma_config {
    enum xdma_ip_type dmatype;              // IP类型
    int (*clk_init)(...);                   // 时钟初始化
    irqreturn_t (*irq_handler)(int, void*); // 中断处理函数
    const int max_channels;                 // 最大通道数
};

static const struct xilinx_dma_config axidma_config = {
    .dmatype = XDMA_TYPE_AXIDMA,
    .clk_init = axidma_clk_init,
    .irq_handler = xilinx_dma_irq_handler,
    .max_channels = 2,
};
```

### 7.3 SylixOS移植方案

#### 7.3.1 板级参数结构

```c
typedef struct {
    CPCHAR compatible;
    phys_addr_t reg_base;
    size_t reg_size;
    struct {
        CPCHAR compatible;
        INT irq;
        UINT32 dma_channels;
    } child[2];
    UINT32 xlnx_datawidth;
    UINT32 xlnx_addrwidth;
} xilinx_board_params_t;
```

#### 7.3.2 Config匹配

```c
const xilinx_dma_config_t* xilinx_match_config(CPCHAR compatible)
{
    if (lib_strcmp(compatible, "xlnx,axi-dma-1.00.a") == 0)
        return &axidma_config;
    // 其他类型...
    return NULL;
}
```

#### 7.3.3 主Probe函数

```c
INT xilinx_dma_probe(const xilinx_board_params_t *params)
{
    xilinx_dma_device_t *xdev;
    const xilinx_dma_config_t *config;

    config = xilinx_match_config(params->compatible);
    xdev->dma_config = config;

    // 映射寄存器(PL必须用平板映射)
    xdev->regs = API_VmmMap((PVOID)params->reg_base,
                            (PVOID)params->reg_base,
                            params->reg_size,
                            LW_VMM_FLAG_DMA);

    // 遍历子节点
    for (i = 0; i < 2; i++) {
        xilinx_dma_child_probe(xdev, &params->child[i]);
    }

    // 注册到框架
    dma_async_device_register(&xdev->common);
    return 0;
}
```

#### 7.3.4 子节点Probe

```c
INT xilinx_dma_child_probe(xilinx_dma_device_t *xdev,
                           const xilinx_child_params_t *child)
{
    UINT32 nr_channels = child->dma_channels ? child->dma_channels : 1;

    for (i = 0; i < nr_channels; i++) {
        xilinx_dma_chan_probe(xdev, child);
    }
    return 0;
}
```

#### 7.3.5 通道Probe

```c
INT xilinx_dma_chan_probe(xilinx_dma_device_t *xdev,
                          const xilinx_child_params_t *child)
{
    xilinx_dma_chan_t *chan;

    chan = sys_malloc(sizeof(*chan));
    LW_SPIN_INIT(&chan->lock);

    // 根据compatible确定方向
    if (lib_strstr(child->compatible, "mm2s")) {
        chan->direction = DMA_MEM_TO_DEV;
        chan->id = xdev->mm2s_chan_id++;
        chan->ctrl_offset = XILINX_DMA_MM2S_CTRL_OFFSET;
    } else if (lib_strstr(child->compatible, "s2mm")) {
        chan->direction = DMA_DEV_TO_MEM;
        chan->id = xdev->s2mm_chan_id++;
        chan->ctrl_offset = XILINX_DMA_S2MM_CTRL_OFFSET;
    }

    // 注册中断
    API_InterVectorConnect(child->irq, xdev->dma_config->irq_handler,
                          chan, "xilinx-dma");

    // 根据IP类型设置函数指针
    if (xdev->dma_config->dmatype == XDMA_TYPE_AXIDMA) {
        chan->start_transfer = xilinx_dma_start_transfer;
        chan->stop_transfer = xilinx_dma_stop_transfer;
    } else if (xdev->dma_config->dmatype == XDMA_TYPE_MCDMA) {
        chan->start_transfer = xilinx_mcdma_start_transfer;
    }
    // 其他类型...

    // 创建延迟作业
    chan->defer_job = API_InterDeferGet(0);

    // 加入通道链表
    _List_Line_Add_Tail(&chan->common.node, &xdev->common.channels);
    xdev->chan[chan->id] = chan;

    return 0;
}
```

### 7.4 关键移植注意事项

#### 7.4.0 移植要求

代码移植到sylixos要求如下:

- 严格遵守**sylixos**注释风格，函数注释，代码内注释
- 头文件仅放置对外暴露的必要接口，数据结构，声明等内容放到要用的源文件中
- 待定

#### 7.4.1 寄存器映射

**错误做法**:
```c
xdev->regs = API_VmmDmaAlloc(size);  // 错误!DMA分配只能用于数据缓冲区
```

**正确做法**:
```c
xdev->regs = API_VmmMap((PVOID)reg_base, (PVOID)reg_base,
                        reg_size, LW_VMM_FLAG_DMA);  // 平板映射
```

#### 7.4.2 双向链表支持

SylixOS的LW_LIST_LINE是双向链表,与Linux list_head兼容:

```c
// Linux
list_for_each_entry_safe(desc, next, &chan->done_list, node) {
    list_del(&desc->node);
}

// SylixOS
PLW_LIST_LINE pline, pnext;
for (pline = chan->done_list; pline; pline = pnext) {
    pnext = _list_line_get_next(pline);
    desc = _LIST_ENTRY(pline, xilinx_dma_tx_descriptor_t, node);
    _List_Line_Del(&desc->node, &chan->done_list);
}
```

#### 7.4.3 中断底半部

```c
// 初始化时获取延迟队列
chan->defer_jobq = API_InterDeferGet(0);

// 中断处理
static irqreturn_t xilinx_dma_irq_handler(PVOID data, ULONG vector)
{
    xilinx_dma_chan_t *chan = data;
    // 读状态、清中断...
    API_InterDeferJobAdd(chan->defer_jobq, xilinx_dma_do_tasklet, chan);
    return LW_IRQ_HANDLED;
}
```

#### 7.4.4 完整IP类型支持

必须支持所有4种IP:

- **AXI DMA**: 标准DMA,2通道(MM2S+S2MM)
- **AXI CDMA**: 内存拷贝,1通道
- **AXI VDMA**: 视频DMA,2通道,支持stride
- **AXI MCDMA**: 多通道DMA,最多32通道

每种IP有不同的:
- 寄存器布局
- 描述符格式
- 中断处理函数
- start/stop函数

### 7.5 移植工作量评估(完整驱动)

| 模块 | 工作量 | 难度 | 说明 |
|------|--------|------|------|
| 数据结构适配 | 3天 | 低 | 所有IP类型的结构体 |
| 寄存器操作 | 2天 | 低 | 封装read/write,平板映射 |
| 内存管理 | 3天 | 中 | DMA内存+描述符池 |
| Probe机制 | 4天 | 中 | 主probe+child_probe+chan_probe |
| 中断处理 | 3天 | 中 | 4种IP的中断处理 |
| AXI DMA | 4天 | 中 | SG传输+Simple模式 |
| AXI CDMA | 2天 | 低 | 内存拷贝 |
| AXI VDMA | 4天 | 高 | 二维传输+stride |
| AXI MCDMA | 5天 | 高 | 多通道管理 |
| 循环模式 | 2天 | 中 | Cyclic DMA |
| 测试验证 | 8天 | 高 | 4种IP功能+压力测试 |
| **总计** | **40天** | - | 约8周(完整驱动) |

---

## 8. 移植开发建议

### 8.1 分阶段实施

**阶段1**: 基础框架(8天)

- 数据结构定义(所有IP类型)
- 寄存器访问封装
- Probe机制(主probe+child+chan)
- Config匹配机制
- 注册到DMAengine框架

**阶段2**: AXI DMA实现(10天)

- Simple模式传输
- SG模式传输
- 中断处理
- 回调机制
- Memcpy测试

**阶段3**: 其他IP类型(14天)

- CDMA实现(2天)
- VDMA实现(5天)
- MCDMA实现(5天)
- 循环模式(2天)

**阶段4**: 测试验证(8天)

- 单元测试
- 压力测试
- 4种IP交叉测试
- 性能测试

### 8.2 调试建议

1. **分IP类型调试**: 先完成AXI DMA,再扩展其他类型
2. **寄存器dump**: 每次传输前后dump关键寄存器
3. **描述符验证**: 检查next_desc链完整性
4. **中断统计**: 记录每种中断触发次数
5. **使用逻辑分析仪**: 观察AXI总线波形

### 8.3 参考资料

- Xilinx PG021: AXI DMA v7.1 Product Guide
- Xilinx PG034: AXI CDMA v4.1 Product Guide
- Xilinx PG020: AXI VDMA v6.3 Product Guide
- Xilinx PG288: AXI MCDMA v1.1 Product Guide
- Linux源码: `drivers/dma/xilinx/xilinx_dma.c`
- SylixOS框架: `src/dmaengine.h`

---

## 9. 总结

Xilinx DMA完整驱动移植到SylixOS的核心工作:

1. **Probe机制**: 设备树→静态板级参数,保持主probe+child_probe+chan_probe三层结构
2. **寄存器映射**: PL寄存器必须用`API_VmmMap`平板映射,不能用DMA分配
3. **双向链表**: SylixOS的LW_LIST_LINE是双向链表,与Linux完全兼容,支持O(1)删除
4. **完整IP支持**: 必须支持AXI DMA/CDMA/VDMA/MCDMA全部4种IP类型
5. **Config机制**: 通过compatible字符串匹配对应的xilinx_dma_config

关键差异已在现有DMAengine框架中抽象(cookie、队列),驱动专注硬件操作。参考Linux驱动的三层probe结构,可系统化完成移植。

#### Linux通道结构

```c
struct xilinx_dma_chan {
    struct list_head pending_list;
    struct list_head active_list;
    struct list_head done_list;
    struct tasklet_struct tasklet;
};
```

#### SylixOS通道结构

```c
typedef struct xilinx_dma_chan {
    PLW_LIST_LINE pending_list;
    PLW_LIST_LINE active_list;
    PLW_LIST_LINE done_list;
    LW_OBJECT_HANDLE defer_job;  // 替代tasklet
    dma_chan_t common;            // 嵌入框架通道
} xilinx_dma_chan_t;
```

### 7.3 关键函数移植

#### 7.3.1 Probe函数

**Linux**:
```c
static int xilinx_dma_probe(struct platform_device *pdev)
{
    struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    xdev->regs = devm_ioremap_resource(&pdev->dev, res);
    irq = platform_get_irq(pdev, 0);
    // ...
}
```

**SylixOS**:
```c
INT xilinx_dma_probe(const xilinx_board_params_t *params)
{
    xdev->regs = API_VmmIoRemap(params->reg_base, params->reg_size);
    API_InterVectorConnect(params->irq, xilinx_dma_irq_handler,
                          xdev, "xilinx-dma");
    // ...
}
```

#### 7.3.2 中断处理

**Linux**:
```c
static irqreturn_t xilinx_dma_irq_handler(int irq, void *data)
{
    // 读状态、清中断
    tasklet_schedule(&chan->tasklet);
    return IRQ_HANDLED;
}
```

**SylixOS**:
```c
static irqreturn_t xilinx_dma_irq_handler(PVOID data, ULONG vector)
{
    // 读状态、清中断
    API_InterDeferJobAdd(chan->defer_job);
    return LW_IRQ_HANDLED;
}
```

#### 7.3.3 内存分配

**Linux**:
```c
chan->seg_v = dma_alloc_coherent(chan->dev,
                                  sizeof(*chan->seg_v) * 255,
                                  &chan->seg_p, GFP_KERNEL);
```

**SylixOS**:
```c
chan->seg_v = API_VmmDmaAlloc(sizeof(*chan->seg_v) * 255);
chan->seg_p = API_VmmGetPhysicalAddr(chan->seg_v);
```

### 7.4 移植步骤

#### 步骤1: 创建板级参数结构

```c
typedef struct {
    CPCHAR      compatible;
    CPCHAR      dev_name;
    phys_addr_t reg_base;
    size_t      reg_size;
    INT         irq;
    INT         nr_channels;
    BOOL        has_sg;
    UINT32      max_buffer_len;
} xilinx_board_params_t;

static const xilinx_board_params_t _G_xilinx_params = {
    .compatible     = "xlnx,axi-dma-1.00.a",
    .dev_name       = "xilinx-dma",
    .reg_base       = 0x40400000,
    .reg_size       = 0x10000,
    .irq            = 61,
    .nr_channels    = 2,
    .has_sg         = TRUE,
    .max_buffer_len = (1 << 23) - 1,
};
```

#### 步骤2: 实现Probe函数

```c
INT xilinx_dma_probe(const xilinx_board_params_t *params)
{
    xilinx_dma_device_t *xdev;

    // 1. 分配设备结构
    xdev = sys_malloc(sizeof(*xdev));
    lib_bzero(xdev, sizeof(*xdev));

    // 2. 映射寄存器
    xdev->regs = API_VmmIoRemap(params->reg_base, params->reg_size);

    // 3. 初始化通道
    for (i = 0; i < params->nr_channels; i++) {
        chan = sys_malloc(sizeof(*chan));
        chan->xdev = xdev;
        chan->id = i;
        LW_SPIN_INIT(&chan->lock);
        // 初始化队列...
    }

    // 4. 注册中断
    API_InterVectorConnect(params->irq, xilinx_dma_irq_handler,
                          xdev, "xilinx-dma");

    // 5. 填充dma_device_t
    xdev->common.device_alloc_chan_resources = xilinx_alloc_chan_resources;
    xdev->common.device_free_chan_resources  = xilinx_free_chan_resources;
    xdev->common.device_prep_slave_sg        = xilinx_prep_slave_sg;
    xdev->common.device_config               = xilinx_slave_config;
    xdev->common.device_issue_pending        = xilinx_issue_pending;
    xdev->common.device_tx_status            = xilinx_tx_status;
    xdev->common.device_terminate_all        = xilinx_terminate_all;

    // 6. 注册到框架
    dma_async_device_register(&xdev->common);

    return 0;
}
```

#### 步骤3: 实现寄存器访问封装

```c
static inline UINT32 dma_read(xilinx_dma_chan_t *chan, UINT32 reg)
{
    return read32((addr_t)chan->xdev->regs + reg);
}

static inline VOID dma_write(xilinx_dma_chan_t *chan, UINT32 reg, UINT32 val)
{
    write32(val, (addr_t)chan->xdev->regs + reg);
}
```

#### 步骤4: 实现中断底半部

```c
static VOID xilinx_dma_defer_job(PVOID arg)
{
    xilinx_dma_chan_t *chan = (xilinx_dma_chan_t *)arg;
    xilinx_dma_chan_desc_cleanup(chan);
}

// 在通道初始化时创建
chan->defer_job = API_InterDeferJobCreate(xilinx_dma_defer_job, chan);
```

### 7.5 关键移植注意事项

#### 7.5.1 DMA内存一致性

Linux的`dma_alloc_coherent`保证CPU和DMA视图一致。SylixOS需要:

```c
// 分配
ptr = API_VmmDmaAlloc(size);
phys = API_VmmGetPhysicalAddr(ptr);

// 写入后刷新缓存
API_CacheClear(DATA_CACHE, ptr, size);

// 读取前使无效
API_CacheInvalidate(DATA_CACHE, ptr, size);
```

#### 7.5.2 中断处理差异

**Linux**: 硬中断 + tasklet(软中断上下文)

**SylixOS**: 硬中断 + 延迟作业(独立线程上下文)

**注意**: SylixOS延迟作业可以睡眠,但需注意优先级配置。

#### 7.5.3 设备树解析

Linux从设备树自动解析参数,SylixOS需手动定义:

```c
// Linux: 自动从DT解析
of_property_read_u32(node, "xlnx,num-fstores", &num_frms);

// SylixOS: 静态配置
static const xilinx_board_params_t params = {
    .num_frms = 16,  // 手动配置
};
```


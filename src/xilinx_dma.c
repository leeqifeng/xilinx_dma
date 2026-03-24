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
** 文   件   名: xilinx_dma.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 24 日
**
** 描        述: Xilinx DMA SylixOS 内核模块入口
**               支持 AXI DMA、CDMA、VDMA、MCDMA 四种 IP 核。
**
**               板级参数（基地址、中断向量）由下方宏控制，
**               须与 Vivado 工程 Address Editor 及 BSP 中断分配表保持一致。
**
**               当前默认配置：仅启用 AXI DMA（与原始版本行为一致）。
**               若要启用其他 IP，取消相应宏注释并填写正确的板级参数。
**
** 修改记录:
** 2026.03.24  更新为统一 probe 接口：改用 struct xilinx_dma_probe_config + ip_type 字段
**             统一调用 xilinx_dma_probe() / xilinx_dma_remove()，对齐 Linux
**             platform_driver.probe 单入口设计；删除各 IP 独立 probe 调用。
**
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <module.h>
#include "drv/dma_core.h"
#include "drv/hw/xilinx/xilinx_dma.h"

/*********************************************************************************************************
  外部声明
*********************************************************************************************************/

extern void dma_test_register_cmds(void);

/*********************************************************************************************************
  AXI DMA 板级配置宏
  AXI_DMA_BASE     — AXI DMA IP 核物理基地址（Vivado Address Editor）
  AXI_DMA_IRQ_MM2S — MM2S 通道 SylixOS 中断向量号
  AXI_DMA_IRQ_S2MM — S2MM 通道 SylixOS 中断向量号
  AXI_DMA_MAX_SG   — SG 模式每描述符最大 BD 数
*********************************************************************************************************/

#define AXI_DMA_BASE        0x40400000UL
#define AXI_DMA_IRQ_MM2S    57
#define AXI_DMA_IRQ_S2MM    58
#define AXI_DMA_MAX_SG      32

/*********************************************************************************************************
  CDMA 板级配置宏（注释掉表示不启用 CDMA）
  CDMA_BASE        — CDMA IP 核物理基地址
  CDMA_IRQ         — 中断向量号
  CDMA_HAS_SG      — LW_TRUE 使用 SG 模式，LW_FALSE 使用 Simple 模式
*********************************************************************************************************/

/* #define ENABLE_CDMA */
#define CDMA_BASE           0x7E200000UL
#define CDMA_IRQ            59
#define CDMA_HAS_SG         LW_FALSE

/*********************************************************************************************************
  VDMA 板级配置宏（注释掉表示不启用 VDMA）
  VDMA_BASE        — VDMA IP 核物理基地址
  VDMA_IRQ_MM2S    — MM2S 通道中断向量号
  VDMA_IRQ_S2MM    — S2MM 通道中断向量号
  VDMA_MAX_FRAMES  — 最大帧缓冲数量（1~32）
*********************************************************************************************************/

/* #define ENABLE_VDMA */
#define VDMA_BASE           0x43000000UL
#define VDMA_IRQ_MM2S       60
#define VDMA_IRQ_S2MM       61
#define VDMA_MAX_FRAMES     3

/*********************************************************************************************************
  MCDMA 板级配置宏（注释掉表示不启用 MCDMA）
  MCDMA_BASE       — MCDMA IP 核物理基地址
  MCDMA_N_MM2S     — MM2S 通道数
  MCDMA_N_S2MM     — S2MM 通道数
  MCDMA_IRQ_*      — 各通道中断向量号（须与 IP 参数匹配）
*********************************************************************************************************/

/* #define ENABLE_MCDMA */
#define MCDMA_BASE          0x40000000UL
#define MCDMA_N_MM2S        2
#define MCDMA_N_S2MM        2
#define MCDMA_IRQ_MM2S_0    62
#define MCDMA_IRQ_MM2S_1    63
#define MCDMA_IRQ_S2MM_0    64
#define MCDMA_IRQ_S2MM_1    65

/*********************************************************************************************************
  模块全局变量
*********************************************************************************************************/

static struct dma_device  _G_axidma_dev;

#ifdef ENABLE_CDMA
static struct dma_device  _G_cdma_dev;
#endif

#ifdef ENABLE_VDMA
static struct dma_device  _G_vdma_dev;
#endif

#ifdef ENABLE_MCDMA
static struct dma_device  _G_mcdma_dev;
#endif

/*********************************************************************************************************
** 函数名称: module_init
** 功能描述: 内核模块加载入口（insmod 时由 SylixOS 自动调用）。
**           依次完成：DMA 核心层初始化、各 IP 控制器探测、测试命令注册。
** 输　入  : NONE
** 输　出  : 0 成功；非 0 失败
*********************************************************************************************************/

int module_init (void)
{
    struct xilinx_dma_probe_config  cfg;
    int                             ret;

    printk("xilinx: DMA driver loading...\n");

    ret = dma_core_init();
    if (ret) {
        printk("xilinx: dma_core_init failed (%d)\n", ret);
        return  ret;
    }

    /*  ---- AXI DMA ----  */
    lib_memset(&cfg, 0, sizeof(cfg));
    cfg.ip_type    = DMA_IP_AXIDMA;
    cfg.base_addr  = (addr_t)AXI_DMA_BASE;
    cfg.irq_mm2s   = (ULONG)AXI_DMA_IRQ_MM2S;
    cfg.irq_s2mm   = (ULONG)AXI_DMA_IRQ_S2MM;
    cfg.has_mm2s   = LW_TRUE;
    cfg.has_s2mm   = LW_TRUE;
    cfg.max_sg_len = AXI_DMA_MAX_SG;

    lib_memset(&_G_axidma_dev, 0, sizeof(_G_axidma_dev));
    _G_axidma_dev.name = "axi_dma0";

    ret = xilinx_dma_probe(&_G_axidma_dev, &cfg);
    if (ret) {
        printk("xilinx: axidma_probe failed (%d)\n", ret);
        dma_core_exit();
        return  ret;
    }

#ifdef ENABLE_CDMA
    {
        lib_memset(&cfg, 0, sizeof(cfg));
        cfg.ip_type   = DMA_IP_CDMA;
        cfg.base_addr = (addr_t)CDMA_BASE;
        cfg.irq       = (ULONG)CDMA_IRQ;
        cfg.has_sg    = CDMA_HAS_SG;

        lib_memset(&_G_cdma_dev, 0, sizeof(_G_cdma_dev));
        _G_cdma_dev.name = "cdma0";

        ret = xilinx_dma_probe(&_G_cdma_dev, &cfg);
        if (ret) {
            printk("xilinx: cdma_probe failed (%d)\n", ret);
        }
    }
#endif

#ifdef ENABLE_VDMA
    {
        lib_memset(&cfg, 0, sizeof(cfg));
        cfg.ip_type      = DMA_IP_VDMA;
        cfg.base_addr    = (addr_t)VDMA_BASE;
        cfg.irq_mm2s     = (ULONG)VDMA_IRQ_MM2S;
        cfg.irq_s2mm     = (ULONG)VDMA_IRQ_S2MM;
        cfg.has_mm2s     = LW_TRUE;
        cfg.has_s2mm     = LW_TRUE;
        cfg.max_frm_cnt  = VDMA_MAX_FRAMES;

        lib_memset(&_G_vdma_dev, 0, sizeof(_G_vdma_dev));
        _G_vdma_dev.name = "vdma0";

        ret = xilinx_dma_probe(&_G_vdma_dev, &cfg);
        if (ret) {
            printk("xilinx: vdma_probe failed (%d)\n", ret);
        }
    }
#endif

#ifdef ENABLE_MCDMA
    {
        lib_memset(&cfg, 0, sizeof(cfg));
        cfg.ip_type       = DMA_IP_MCDMA;
        cfg.base_addr     = (addr_t)MCDMA_BASE;
        cfg.n_mm2s        = MCDMA_N_MM2S;
        cfg.n_s2mm        = MCDMA_N_S2MM;
        cfg.irqs_mm2s[0]  = (ULONG)MCDMA_IRQ_MM2S_0;
        cfg.irqs_mm2s[1]  = (ULONG)MCDMA_IRQ_MM2S_1;
        cfg.irqs_s2mm[0]  = (ULONG)MCDMA_IRQ_S2MM_0;
        cfg.irqs_s2mm[1]  = (ULONG)MCDMA_IRQ_S2MM_1;

        lib_memset(&_G_mcdma_dev, 0, sizeof(_G_mcdma_dev));
        _G_mcdma_dev.name = "mcdma0";

        ret = xilinx_dma_probe(&_G_mcdma_dev, &cfg);
        if (ret) {
            printk("xilinx: mcdma_probe failed (%d)\n", ret);
        }
    }
#endif

    dma_test_register_cmds();

    printk("xilinx: DMA driver loaded (axi_dma0 base=0x%08lX)\n",
           (ULONG)AXI_DMA_BASE);
    return  0;
}

/*********************************************************************************************************
** 函数名称: module_exit
** 功能描述: 内核模块卸载出口（rmmod 时由 SylixOS 自动调用）。
** 输　入  : NONE
** 输　出  : NONE
*********************************************************************************************************/

void module_exit (void)
{
    printk("xilinx: DMA driver unloading...\n");

#ifdef ENABLE_MCDMA
    xilinx_dma_remove(&_G_mcdma_dev);
#endif

#ifdef ENABLE_VDMA
    xilinx_dma_remove(&_G_vdma_dev);
#endif

#ifdef ENABLE_CDMA
    xilinx_dma_remove(&_G_cdma_dev);
#endif

    xilinx_dma_remove(&_G_axidma_dev);
    dma_core_exit();

    printk("xilinx: DMA driver unloaded\n");
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

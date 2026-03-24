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
** 创   建   人: AutoGen
**
** 文件创建日期: 2026 年 03 月 23 日
**
** 描        述: Xilinx AXI DMA SylixOS 内核模块入口
**               module_init：初始化 DMA 引擎核心层、探测 AXI DMA 控制器并注册测试命令。
**               module_exit：卸载控制器并反初始化核心层。
**
**               板级参数（基地址、中断向量、通道使能、工作模式）由下方宏控制，
**               须与 Vivado 工程 Address Editor 及 BSP 中断分配表保持一致。
**
** BUG
** 2026.03.23  初始版本。
*********************************************************************************************************/
#define __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <module.h>
#include "drv/dma_core.h"
#include "drv/dma_hw.h"

/*********************************************************************************************************
  外部声明
*********************************************************************************************************/

extern void dma_test_register_cmds(void);

/*********************************************************************************************************
  板级 / IP 配置宏
  须根据 Vivado 工程修改以下参数：
    AXI_DMA_BASE     — AXI DMA IP 核物理基地址
                       在 Vivado Address Editor 中查看（Zynq-7000 典型值 0x40400000）
    AXI_DMA_IRQ_MM2S — MM2S 通道 SylixOS 中断向量号
    AXI_DMA_IRQ_S2MM — S2MM 通道 SylixOS 中断向量号
                       Zynq-7000 PL 中断从向量 61（SPI 29）起，参考 BSP 中断分配表
    AXI_DMA_HAS_SG   — IP 核是否开启 Scatter-Gather（LW_TRUE / LW_FALSE）
    AXI_DMA_MODE     — 工作模式（DMA_MODE_SIMPLE 或 DMA_MODE_SG），须与 has_sg 一致
    AXI_DMA_MAX_SG   — SG 模式下每个描述符最大 BD 数量
*********************************************************************************************************/

#define AXI_DMA_BASE        0x40400000UL
#define AXI_DMA_IRQ_MM2S    57
#define AXI_DMA_IRQ_S2MM    58
#define AXI_DMA_HAS_SG      LW_TRUE
#define AXI_DMA_MODE        DMA_MODE_SG
#define AXI_DMA_MAX_SG      32

/*********************************************************************************************************
  模块全局变量
*********************************************************************************************************/

static struct dma_device _G_axi_dma_dev;                               /*  单实例 AXI DMA 设备         */

/*********************************************************************************************************
** 函数名称: module_init
** 功能描述: 内核模块加载入口（insmod 时由 SylixOS 自动调用）。
**           依次完成：DMA 核心层初始化、控制器探测、测试命令注册。
** 输　入  : NONE
** 输　出  : 0 成功；非 0 失败
** 全局变量: _G_axi_dma_dev
** 调用模块:
                                           API 函数
*********************************************************************************************************/
int module_init (void)
{
    struct axi_dma_config  cfg;
    int                    ret;

    printk("xilinx: AXI DMA driver loading...\n");

    ret = dma_core_init();                                              /*  初始化核心层（设备链表+锁） */
    if (ret) {
        printk("xilinx: dma_core_init failed (%d)\n", ret);
        return  ret;
    }

    lib_memset(&cfg, 0, sizeof(cfg));                                   /*  填写板级硬件配置            */
    cfg.base_addr   = (addr_t)AXI_DMA_BASE;
    cfg.irq_mm2s    = (ULONG)AXI_DMA_IRQ_MM2S;
    cfg.irq_s2mm    = (ULONG)AXI_DMA_IRQ_S2MM;
    cfg.has_sg      = AXI_DMA_HAS_SG;
    cfg.has_mm2s    = LW_TRUE;
    cfg.has_s2mm    = LW_TRUE;
    cfg.mode        = AXI_DMA_MODE;
    cfg.max_sg_len  = AXI_DMA_MAX_SG;

    lib_memset(&_G_axi_dma_dev, 0, sizeof(_G_axi_dma_dev));
    _G_axi_dma_dev.name = "axi_dma0";

    ret = axi_dma_probe(&_G_axi_dma_dev, &cfg);                        /*  映射寄存器、挂中断、注册    */
    if (ret) {
        printk("xilinx: axi_dma_probe failed (%d)\n", ret);
        dma_core_exit();
        return  ret;
    }

    dma_test_register_cmds();                                           /*  注册 tshell 测试命令        */

    printk("xilinx: AXI DMA driver loaded (base=0x%08lX MM2S irq=%lu S2MM irq=%lu)\n",
           (ULONG)AXI_DMA_BASE,
           (ULONG)AXI_DMA_IRQ_MM2S,
           (ULONG)AXI_DMA_IRQ_S2MM);

    return  0;
}
/*********************************************************************************************************
** 函数名称: module_exit
** 功能描述: 内核模块卸载出口（rmmod 时由 SylixOS 自动调用）。
**           依次完成：控制器卸载、DMA 核心层反初始化。
** 输　入  : NONE
** 输　出  : NONE
** 全局变量: _G_axi_dma_dev
** 调用模块:
                                           API 函数
*********************************************************************************************************/
void module_exit (void)
{
    printk("xilinx: AXI DMA driver unloading...\n");

    axi_dma_remove(&_G_axi_dma_dev);
    dma_core_exit();

    printk("xilinx: AXI DMA driver unloaded\n");
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

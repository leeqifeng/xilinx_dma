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
** 文   件   名: dma.c
**
** 创   建   人: Li.Qifeng
**
** 文件创建日期: 2026 年 03 月 24 日
**
** 描        述: SylixOS DMA Engine 内核模块入口
**
**               本文件职责：
**                 1. 集中管理所有 DMA 控制器的板级配置参数（寄存器地址、中断号等）
**                 2. 初始化 DMA Engine 框架
**                 3. 调用各驱动库的 probe 接口完成设备注册
**
**               模块加载顺序：
**                 1. dma_engine_init()           — 初始化框架全局状态
**                 2. demoip_probe(&params)       — 注册 Demo IP DMA 控制器
**                 3. xilinx_dma_probe(&params)   — 注册 Xilinx DMA 控制器
**                 4. dma_test_register_cmds()    — 向 tshell 注册测试命令
**
**               模块卸载顺序（逆序）：
**                 1. 各驱动库自动注销（通过 dma_async_device_unregister）
**                 2. dma_engine_exit()           — 清理框架全局状态
**
*********************************************************************************************************/
#define  __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <module.h>
#include "dmaengine.h"
#include "hw/demoip/demoip.h"
#include "hw/xilinx/xilinx.h"

/*
 *  来自 test/dma_test.c 的命令注册函数
 */
extern VOID  dma_test_register_cmds(VOID);

/*********************************************************************************************************
  板级配置参数（集中管理）
*********************************************************************************************************/

/*
 *  Demo IP 板级参数（软件模拟 DMA，用于框架测试）
 */

static const demoip_board_params_t _G_demoip_params = {
    .compatible  = "demoip,dma-1.00.a",
    .dev_name    = "demoip-dma",
    .reg_base    = 0x43C00000UL,
    .reg_size    = 0x10000,
    .irq         = 89,
    .nr_channels = 2,
};

/*
 *  Xilinx DMA 板级参数（真实硬件）
 */

static const xilinx_board_params_t _G_xilinx_params = {
    .compatible      = "xlnx,axi-dma-1.00.a",
    .reg_base        = 0x40400000,
    .reg_size        = 0x10000,
    .child = {
        {
            .compatible   = "xlnx,axi-dma-mm2s-channel",
            .irq          = 57,
            .dma_channels = 1,
        },
        {
            .compatible   = "xlnx,axi-dma-s2mm-channel",
            .irq          = 58,
            .dma_channels = 1,
        }
    },
    .xlnx_datawidth  = 32,
    .xlnx_addrwidth  = 32,
};

/*********************************************************************************************************
** 函数名称: module_init
** 功能描述: 内核模块加载入口（insmod 时由 SylixOS 自动调用）
** 输　入  : NONE
** 输　出  : 0 成功；非 0 失败
*********************************************************************************************************/

int  module_init (void)
{
    INT  ret;

    /*
     *  Step 1: 初始化 DMA Engine 框架
     */
    ret = dma_engine_init();
    if (ret != 0) {
        printk("[dma] ERROR: dma_engine_init failed (%d)\n", ret);
        return  (ret);
    }

    /*
     *  Step 2: 阶段1 - 驱动库初始化（注册驱动能力）
     */
    ret = demoip_lib_init();
    if (ret != 0) {
        printk("[dma] ERROR: demoip_lib_init failed (%d)\n", ret);
        dma_engine_exit();
        return  (ret);
    }

    ret = xilinx_dma_lib_init();
    if (ret != 0) {
        printk("[dma] ERROR: xilinx_dma_lib_init failed (%d)\n", ret);
        dma_engine_exit();
        return  (ret);
    }

    /*
     *  Step 3: 阶段2 - 硬件实例注册（绑定板级参数）
     */
    ret = demoip_params_register(&_G_demoip_params);
    if (ret != 0) {
        printk("[dma] ERROR: demoip_params_register failed (%d)\n", ret);
        dma_engine_exit();
        return  (ret);
    }

    ret = xilinx_dma_params_register(&_G_xilinx_params);
    if (ret != 0) {
        printk("[dma] WARNING: xilinx_dma_params_register failed (%d),"
               " continue with demoip only\n", ret);
    }

    /*
     *  Step 4: 注册 tshell 测试命令
     */
    dma_test_register_cmds();

    printk("[dma] module loaded, use 'dma_test_memcpy' to run tests\n");
    return  (0);
}

/*********************************************************************************************************
** 函数名称: module_exit
** 功能描述: 内核模块卸载出口（rmmod 时由 SylixOS 自动调用）
** 输　入  : NONE
** 输　出  : NONE
** 注意    : 各驱动库会在 dma_engine_exit 前自动调用 dma_async_device_unregister 注销
*********************************************************************************************************/

void  module_exit (void)
{
    dma_engine_exit();
    printk("[dma] module unloaded\n");
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

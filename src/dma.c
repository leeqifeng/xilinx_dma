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
**               模块加载顺序：
**                 1. dma_engine_init()       — 初始化框架全局状态
**                 2. demoip_driver_init()    — 注册 Demo IP DMA 控制器
**                 3. dma_test_register_cmds() — 向 tshell 注册测试命令
**
**               模块卸载顺序（逆序）：
**                 1. demoip_driver_exit()    — 注销 Demo IP DMA 控制器
**                 2. dma_engine_exit()       — 清理框架全局状态
**
*********************************************************************************************************/
#define  __SYLIXOS_KERNEL
#include <SylixOS.h>
#include <module.h>
#include "dmaengine.h"
#include "hw/fream/demoip.h"

/*
 *  来自 test/dma_test.c 的命令注册函数
 */
extern VOID  dma_test_register_cmds(VOID);

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
     *  Step 2: 注册 Demo IP DMA 控制器
     */
    ret = demoip_driver_init();
    if (ret != 0) {
        printk("[dma] ERROR: demoip_driver_init failed (%d)\n", ret);
        dma_engine_exit();
        return  (ret);
    }

    /*
     *  Step 3: 注册 tshell 测试命令
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
** 注意    : 若有测试命令正在执行（通道未释放），rmmod 前请确保命令已返回。
*********************************************************************************************************/

void  module_exit (void)
{
    demoip_driver_exit();
    dma_engine_exit();
    printk("[dma] module unloaded\n");
}
/*********************************************************************************************************
  END
*********************************************************************************************************/

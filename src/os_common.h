#ifndef A274D2E0_32E0_4EF5_8D41_E7AAAD376DE1
#define A274D2E0_32E0_4EF5_8D41_E7AAAD376DE1
#ifndef SRC_OS_COMMON_H_
#define SRC_OS_COMMON_H_

/*********************************************************************************************************
  INTERRUPT
*********************************************************************************************************/
#define LW_IRQ_NONE                     (0)
#define LW_IRQ_HANDLED                  (1)

#define IRQ_TYPE_LEVEL_HIGH             (0x00000004)
extern VOID  bspIntVectorTypeSet(ULONG  ulVector, INT  iType);

typedef INT         irqreturn_t;
typedef irqreturn_t (*PINT_SVR_ROUTINE)(PVOID pvArg, ULONG ulVector);   /*  系统中断服务函数            */

LW_API ULONG            API_InterVectorConnect(ULONG                 ulVector,
                                               PINT_SVR_ROUTINE      pfuncIsr,
                                               PVOID                 pvArg,
                                               CPCHAR                pcName);
                                                                        /*  设置指定向量的服务程序      */
LW_API ULONG            API_InterVectorEnable(ULONG  ulVector);         /*  使能指定向量中断            */

LW_API ULONG            API_InterVectorDisable(ULONG  ulVector);        /*  禁能指定向量中断            */

/*********************************************************************************************************
  INTERRUPT 底半部
  示例：
  PLW_JOB_QUEUE   pJobqueue = NULL;

  pJobqueue = API_InterDeferGet(0); //底半部默认0核

  static irqreturn_t  __Isr (__CHANNEL  pChannel, ULONG  ulVector)
{
    API_InterDeferJobAdd(pJobqueue, (VOIDFUNCPTR)__do_Isr, (PVOID)pChannel);

    return  (LW_IRQ_HANDLED);
}

*********************************************************************************************************/
typedef VOID        (*VOIDFUNCPTR)(...);                                /*  function returning void     */

LW_API PLW_JOB_QUEUE    API_InterDeferGet(ULONG  ulCPUId);              /*  获得对应 CPU 的中断延迟队列 */

LW_API INT              API_InterDeferContext(VOID);                    /*  是否在中断或 defer 上下文   */

LW_API ULONG            API_InterDeferJobAdd(PLW_JOB_QUEUE  pjobq, VOIDFUNCPTR  pfunc, PVOID  pvArg);
                                                                        /*  向中断延迟处理队列加入任务  */
LW_API ULONG            API_InterDeferJobAddEx(PLW_JOB_QUEUE  pjobq,
                                               VOIDFUNCPTR    pfunc,
                                               PVOID          pvArg0,
                                               PVOID          pvArg1,
                                               PVOID          pvArg2,
                                               PVOID          pvArg3,
                                               PVOID          pvArg4,
                                               PVOID          pvArg5);  /*  向中断延迟处理队列加入任务  */

LW_API ULONG            API_InterDeferJobDelete(PLW_JOB_QUEUE  pjobq, BOOL  bMatchArg, 
                                                VOIDFUNCPTR  pfunc, PVOID  pvArg);
                                                                        /*  从中断延迟处理队列删除任务  */

/*********************************************************************************************************
  VMM API (以下分配函数可以分配出确定的, 可供直接访问的内存空间)
*********************************************************************************************************/
#define LW_VMM_FLAG_DMA                 (LW_VMM_FLAG_VALID |        \
                                         LW_VMM_FLAG_ACCESS |       \
                                         LW_VMM_FLAG_WRITABLE |     \
                                         LW_VMM_FLAG_GUARDED)           /*  物理硬件映射 (CACHE 一致的) */
LW_API ULONG        API_VmmMap(PVOID  pvVirtualAddr,
                               PVOID  pvPhysicalAddr,
                               size_t stSize,
                               ULONG  ulFlag);                          /*  直接映射,映射后地址可直接访问 */

                   
LW_API PVOID        API_VmmMalloc(size_t stSize);                       /*  分配逻辑连续内存, 虚拟地址  */
LW_API PVOID        API_VmmMallocEx(size_t stSize, ULONG ulFlag);       /*  分配逻辑连续内存, 虚拟地址  */
LW_API PVOID        API_VmmMallocAlign(size_t stSize, 
                                       size_t stAlign, 
                                       ULONG  ulFlag);                  /*  分配逻辑连续内存, 虚拟地址  */
LW_API VOID         API_VmmFree(PVOID  pvVirtualMem);                   /*  回收虚拟连续内存            */

LW_API ULONG        API_VmmMap(PVOID  pvVirtualAddr,
                               PVOID  pvPhysicalAddr,
                               size_t stSize,
                               ULONG  ulFlag);                          /*  直接映射 (谨慎使用!)        */

LW_API ULONG        API_VmmVirtualToPhysical(addr_t  ulVirtualAddr,     /*  通过虚拟地址获取物理地址    */
                                             phys_addr_t  *ppaPhysicalAddr);
/*********************************************************************************************************
  以下 API 只负责分配物理内存, 并没有产生映射关系. 不能直接使用, 必须通过虚拟内存映射才能使用.
*********************************************************************************************************/

LW_API PVOID        API_VmmPhyAlloc(size_t stSize);                     /*  分配物理内存                */
LW_API PVOID        API_VmmPhyAllocEx(size_t  stSize, UINT  uiAttr);    /*  与上相同, 但可以指定内存属性*/
LW_API PVOID        API_VmmPhyAllocAlign(size_t stSize, 
                                         size_t stAlign,
                                         UINT   uiAttr);                /*  分配物理内存, 指定对齐关系  */
LW_API VOID         API_VmmPhyFree(PVOID  pvPhyMem);                    /*  释放物理内存                */

/*********************************************************************************************************
  以下 API 只允许驱动程序使用
  
  no cache 区域操作 (dma 操作返回的是直接的物理地址, 即平板映射, 并且在 LW_ZONE_ATTR_DMA 域中)
*********************************************************************************************************/

LW_API PVOID        API_VmmDmaAlloc(size_t  stSize);                    /*  分配物理连续内存, 物理地址  */
LW_API PVOID        API_VmmDmaAllocAlign(size_t stSize, size_t stAlign);/*  与上相同, 但可以指定对齐关系*/
LW_API PVOID        API_VmmDmaAllocAlignWithFlags(size_t  stSize, size_t  stAlign, ULONG  ulFlags);
                                                                        /*  与上相同, 但可以指定内存类型*/
LW_API VOID         API_VmmDmaFree(PVOID  pvDmaMem);                    /*  回收 DMA 内存缓冲区         */

/*********************************************************************************************************
  ARM 处理器 I/O 屏障 (Non-Cache 区域 SylixOS 未使用低效率的强排序方式, 所以这里需要加入内存屏障)
*********************************************************************************************************/

#define KN_IO_MB()      armDsbOp()
#define KN_IO_RMB()     armDsbOp()
#define KN_IO_WMB()     armDsbOp(st)

/*********************************************************************************************************
  ARM 处理器 I/O 内存读
*********************************************************************************************************/
static LW_INLINE UINT32  read32_raw (addr_t  ulAddr)
{
    UINT32  uiVal = *(volatile UINT32 *)ulAddr;
    KN_IO_RMB();
    return  (uiVal);
}

/*********************************************************************************************************
  ARM 处理器 I/O 内存写
*********************************************************************************************************/
static LW_INLINE VOID  write32_raw (UINT32  uiData, addr_t  ulAddr)
{
    KN_IO_WMB();
    *(volatile UINT32 *)ulAddr = uiData;
}
/*********************************************************************************************************
  read IOMEM
*********************************************************************************************************/
#define read32(a)           read32_raw(a)
/*********************************************************************************************************
  write IOMEM
*********************************************************************************************************/
#define write32(d, a)       write32_raw(d, a)
/*********************************************************************************************************
  THREAD
*********************************************************************************************************/
LW_API  
LW_CLASS_THREADATTR     API_ThreadAttrGetDefault(VOID);                 /*  获得默认线程属性块          */

LW_API  
LW_CLASS_THREADATTR     API_ThreadAttrGet(LW_OBJECT_HANDLE  ulId);      /*  获得指定线程当前属性块      */

LW_API ULONG            API_ThreadAttrBuild(PLW_CLASS_THREADATTR    pthreadattr,
                                            size_t                  stStackByteSize, 
                                            UINT8                   ucPriority, 
                                            ULONG                   ulOption, 
                                            PVOID                   pvArg);
                                                                        /*  生成一个线程属性块          */
                                            
LW_API ULONG            API_ThreadAttrBuildEx(PLW_CLASS_THREADATTR    pthreadattr,
                                              PLW_STACK               pstkStackTop, 
                                              size_t                  stStackByteSize, 
                                              UINT8                   ucPriority, 
                                              ULONG                   ulOption, 
                                              PVOID                   pvArg,
                                              PVOID                   pvExt);
                                                                        /*  生成一个高级线程属性块      */

                                                                        
LW_API LW_OBJECT_HANDLE API_ThreadInit(CPCHAR                   pcName,
                                       PTHREAD_START_ROUTINE    pfuncThread,
                                       PLW_CLASS_THREADATTR     pthreadattr,
                                       LW_OBJECT_ID            *pulId); /*  线程初始化                  */
                                       
LW_API LW_OBJECT_HANDLE API_ThreadCreate(CPCHAR                   pcName,
                                         PTHREAD_START_ROUTINE    pfuncThread,
                                         PLW_CLASS_THREADATTR     pthreadattr,
                                         LW_OBJECT_ID            *pulId);
                                                                        /*  建立一个线程                */

LW_API ULONG            API_ThreadDelete(LW_OBJECT_HANDLE  *pulId, 
                                         PVOID  pvRetVal);              /*  删除一个线程                */
                                         
LW_API ULONG            API_ThreadStart(LW_OBJECT_HANDLE    ulId);      /*  启动一个已经初始化的线程    */

LW_API ULONG            API_ThreadStartEx(LW_OBJECT_HANDLE  ulId, 
                                          BOOL              bJoin, 
                                          PVOID            *ppvRetValAddr);

LW_API ULONG            API_ThreadJoin(LW_OBJECT_HANDLE  ulId, 
                                       PVOID  *ppvRetValAddr);          /*  线程合并                    */                                         
/*********************************************************************************************************
  BINARY SEMAPHORE
*********************************************************************************************************/
typedef ULONG       LW_OBJECT_ID;                                       /*  系统对象 ID 号              */
typedef ULONG       LW_OBJECT_HANDLE;                                   /*  这里的对象句柄和 ID 号等同  */

typedef ULONG       LW_HANDLE;                                          /*  这里的对象句柄和 ID 号等同  */
                                                                        /*  LW_HANDLE 多为应用程序使用  */
LW_API LW_OBJECT_HANDLE API_SemaphoreBCreate(CPCHAR             pcName,
                                             BOOL               bInitValue,
                                             ULONG              ulOption,
                                             LW_OBJECT_ID      *pulId); /*  建立二进制信号量            */
                                             
LW_API ULONG            API_SemaphoreBDelete(LW_OBJECT_HANDLE  *pulId); /*  删除二进制信号量            */

LW_API ULONG            API_SemaphoreBPend(LW_OBJECT_HANDLE  ulId, 
                                           ULONG             ulTimeout);/*  等待二进制信号量            */

LW_API ULONG            API_SemaphoreBPendEx(LW_OBJECT_HANDLE  ulId, 
                                             ULONG             ulTimeout,
                                             PVOID            *ppvMsgPtr);
                                                                        /*  等待二进制信号量消息        */

LW_API ULONG            API_SemaphoreBTryPend(LW_OBJECT_HANDLE  ulId);  /*  无阻塞等待二进制信号量      */

LW_API ULONG            API_SemaphoreBRelease(LW_OBJECT_HANDLE  ulId, 
                                              ULONG             ulReleaseCounter, 
                                              BOOL             *pbPreviousValue);
                                                                        /*  WIN32 释放信号量            */

LW_API ULONG            API_SemaphoreBPost(LW_OBJECT_HANDLE  ulId);     /*  RT 释放信号量               */

LW_API ULONG            API_SemaphoreBPost2(LW_OBJECT_HANDLE  ulId, LW_OBJECT_HANDLE  *pulId);

LW_API ULONG            API_SemaphoreBPostEx(LW_OBJECT_HANDLE  ulId, 
                                             PVOID      pvMsgPtr);      /*  RT 释放信号量消息           */
                                             
LW_API ULONG            API_SemaphoreBPostEx2(LW_OBJECT_HANDLE  ulId, 
                                              PVOID             pvMsgPtr, 
                                              LW_OBJECT_HANDLE *pulId);

LW_API ULONG            API_SemaphoreBClear(LW_OBJECT_HANDLE  ulId);    /*  清除信号量信号              */

LW_API ULONG            API_SemaphoreBFlush(LW_OBJECT_HANDLE  ulId, 
                                            ULONG            *pulThreadUnblockNum);
                                                                        /*  解锁所有等待线程            */

LW_API ULONG            API_SemaphoreBStatus(LW_OBJECT_HANDLE   ulId,
                                             BOOL              *pbValue,
                                             ULONG             *pulOption,
                                             ULONG             *pulThreadBlockNum);
                                                                        /*  检查二进制信号量状态        */

/*********************************************************************************************************
  ShellKeyword
*********************************************************************************************************/
typedef INT               (*PCOMMAND_START_ROUTINE)(INT  iArgC, PCHAR  ppcArgV[]);

W_API ULONG                API_TShellKeywordAdd(CPCHAR  pcKeyword, 
                                                 PCOMMAND_START_ROUTINE  pfuncCommand);  
                                                                        /*  向 tshell 系统中添加关键字  */
LW_API ULONG                API_TShellKeywordAddEx(CPCHAR  pcKeyword, 
                                                   PCOMMAND_START_ROUTINE  pfuncCommand, 
                                                   ULONG  ulOption);    /*  向 tshell 系统中添加关键字  */
LW_API ULONG                API_TShellFormatAdd(CPCHAR  pcKeyword, CPCHAR  pcFormat);
                                                                        /*  向某一个关键字添加格式帮助  */
LW_API ULONG                API_TShellHelpAdd(CPCHAR  pcKeyword, CPCHAR  pcHelp);
                                                                        /*  向某一个关键字添加帮助      */
/*********************************************************************************************************
  symbol api
  delete 函数谨慎使用, 例如遍历结束后, 用户不能再使用回调函数中的 PLW_SYMBOL 结构.
  用法例：API_SymbolAdd("__aeabi_read_tp", (caddr_t)__aeabi_read_tp, LW_SYMBOL_FLAG_XEN)
*********************************************************************************************************/
LW_API VOID         API_SymbolInit(VOID);
LW_API INT          API_SymbolAddStatic(PLW_SYMBOL  psymbol, INT  iNum);/*  静态符号表安装              */
LW_API INT          API_SymbolAdd(CPCHAR  pcName, caddr_t  pcAddr, INT  iFlag);
LW_API INT          API_SymbolDelete(CPCHAR  pcName, INT  iFlag);
LW_API PVOID        API_SymbolFind(CPCHAR  pcName, INT  iFlag);
LW_API VOID         API_SymbolTraverse(BOOL (*pfuncCb)(PVOID, PLW_SYMBOL), PVOID  pvArg);


/*********************************************************************************************************
** 函数名称: _List_Ring_Add_Ahead
** 功能描述: 向 RING 头中插入一个节点 (Header 将指向这个节点, 等待表的操作)
** 输　入  : pringNew      新的节点
**           ppringHeader  链表头
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Ring_Add_Ahead (PLW_LIST_RING  pringNew, LW_LIST_RING_HEADER  *ppringHeader)
{
    REGISTER LW_LIST_RING_HEADER    pringHeader;
    
    pringHeader = *ppringHeader;
    
    if (pringHeader) {                                                  /*  链表内还有节点              */
        pringNew->RING_plistNext = pringHeader;
        pringNew->RING_plistPrev = pringHeader->RING_plistPrev;
        pringHeader->RING_plistPrev->RING_plistNext = pringNew;
        pringHeader->RING_plistPrev = pringNew;
        
    } else {                                                            /*  链表内没有节点              */
        pringNew->RING_plistPrev = pringNew;                            /*  只有新节点                  */
        pringNew->RING_plistNext = pringNew;                            /*  自我抱环                    */
    }
    
    *ppringHeader = pringNew;                                           /*  将表头指向新节点            */
}
/*********************************************************************************************************
** 函数名称: _List_Ring_Add_Front
** 功能描述: 向 RING 头中插入一个节点 (Header 有节点时，不变化，就绪表的操作)
** 输　入  : pringNew      新的节点
**           ppringHeader  链表头
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Ring_Add_Front (PLW_LIST_RING  pringNew, LW_LIST_RING_HEADER  *ppringHeader)
{
    REGISTER LW_LIST_RING_HEADER    pringHeader;
    
    pringHeader = *ppringHeader;
    
    if (pringHeader) {                                                  /*  链表内还有节点              */
        pringNew->RING_plistPrev = pringHeader;
        pringNew->RING_plistNext = pringHeader->RING_plistNext;
        pringHeader->RING_plistNext->RING_plistPrev = pringNew;
        pringHeader->RING_plistNext = pringNew;
    
    } else {                                                            /*  链表内没有节点              */
        pringNew->RING_plistPrev = pringNew;                            /*  只有新节点                  */
        pringNew->RING_plistNext = pringNew;                            /*  自我抱环                    */
        *ppringHeader = pringNew;                                       /*  更新链表头                  */
    }
}
/*********************************************************************************************************
** 函数名称: _List_Ring_Add_Last
** 功能描述: 从后面向 RING 尾中插入一个节点
** 输　入  : pringNew      新的节点
**           ppringHeader  链表头
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Ring_Add_Last (PLW_LIST_RING  pringNew, LW_LIST_RING_HEADER  *ppringHeader)
{
    REGISTER LW_LIST_RING_HEADER    pringHeader;
    
    pringHeader = *ppringHeader;
    
    if (pringHeader) {                                                  /*  没有更新链表                */
        pringHeader->RING_plistPrev->RING_plistNext = pringNew;
        pringNew->RING_plistPrev = pringHeader->RING_plistPrev;
        pringNew->RING_plistNext = pringHeader;
        pringHeader->RING_plistPrev = pringNew;
        
    } else {
        pringNew->RING_plistPrev = pringNew;
        pringNew->RING_plistNext = pringNew;
        *ppringHeader = pringNew;                                       /*  更新链表头                  */
    }
}
/*********************************************************************************************************
** 函数名称: _List_Ring_Del
** 功能描述: 删除一个节点
** 输　入  : pringDel      需要删除的节点
**           ppringHeader  链表头
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Ring_Del (PLW_LIST_RING  pringDel, LW_LIST_RING_HEADER  *ppringHeader)
{
    REGISTER LW_LIST_RING_HEADER    pringHeader;
    
    pringHeader = *ppringHeader;
    
    if (pringDel->RING_plistNext == pringDel) {                         /*  链表中只有一个节点          */
        *ppringHeader = LW_NULL;
        _INIT_LIST_RING_HEAD(pringDel);
        return;
    
    } else if (pringDel == pringHeader) {
        _list_ring_next(ppringHeader);
    }
    
    pringHeader = pringDel->RING_plistPrev;                             /*  pringHeader 属于临时变量    */
    pringHeader->RING_plistNext = pringDel->RING_plistNext;
    pringDel->RING_plistNext->RING_plistPrev = pringHeader;
    
    _INIT_LIST_RING_HEAD(pringDel);                                     /*  prev = next = NULL          */
}
/*********************************************************************************************************
** 函数名称: _List_Line_Add_Ahead
** 功能描述: 从前面向 Line 头中插入一个节点 (Header 将指向这个节点)
** 输　入  : plingNew      新的节点
**           pplingHeader  链表头
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Line_Add_Ahead (PLW_LIST_LINE  plineNew, LW_LIST_LINE_HEADER  *pplineHeader)
{
    REGISTER LW_LIST_LINE_HEADER    plineHeader;
    
    plineHeader = *pplineHeader;
    
    plineNew->LINE_plistNext = plineHeader;
    plineNew->LINE_plistPrev = LW_NULL;
    
    if (plineHeader) {    
        plineHeader->LINE_plistPrev = plineNew;
    }
    
    *pplineHeader = plineNew;                                           /*  指向最新变量                */
}
/*********************************************************************************************************
** 函数名称: _List_Line_Add_Tail
** 功能描述: 从前面向 Line 头中插入一个节点 (Header 不变)
** 输　入  : plingNew      新的节点
**           pplingHeader  链表头
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Line_Add_Tail (PLW_LIST_LINE  plineNew, LW_LIST_LINE_HEADER  *pplineHeader)
{
    REGISTER LW_LIST_LINE_HEADER    plineHeader;
    
    plineHeader = *pplineHeader;
    
    if (plineHeader) {
        if (plineHeader->LINE_plistNext) {
            plineNew->LINE_plistNext = plineHeader->LINE_plistNext;
            plineNew->LINE_plistPrev = plineHeader;
            plineHeader->LINE_plistNext->LINE_plistPrev = plineNew;
            plineHeader->LINE_plistNext = plineNew;
        
        } else {
            plineHeader->LINE_plistNext = plineNew;
            plineNew->LINE_plistPrev = plineHeader;
            plineNew->LINE_plistNext = LW_NULL;
        }
    
    } else {
        plineNew->LINE_plistPrev = LW_NULL;
        plineNew->LINE_plistNext = LW_NULL;
        
        *pplineHeader = plineNew;
    }
}
/*********************************************************************************************************
** 函数名称: _List_Line_Add_Left
** 功能描述: 将新的节点插入指定节点的左侧.
** 输　入  : plineNew      新的节点
**           plineRight    右侧节点
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Line_Add_Left (PLW_LIST_LINE  plineNew, PLW_LIST_LINE  plineRight)
{
    REGISTER PLW_LIST_LINE      plineLeft = plineRight->LINE_plistPrev;
    
    plineNew->LINE_plistNext = plineRight;
    plineNew->LINE_plistPrev = plineLeft;
    
    if (plineLeft) {
        plineLeft->LINE_plistNext = plineNew;
    }
    
    plineRight->LINE_plistPrev = plineNew;
}
/*********************************************************************************************************
** 函数名称: _List_Line_Add_Right
** 功能描述: 将新的节点插入指定节点的右侧.
** 输　入  : plineNew      新的节点
**           plineLeft     左侧节点
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Line_Add_Right (PLW_LIST_LINE  plineNew, PLW_LIST_LINE  plineLeft)
{
    REGISTER PLW_LIST_LINE      plineRight = plineLeft->LINE_plistNext;
    
    plineNew->LINE_plistNext = plineRight;
    plineNew->LINE_plistPrev = plineLeft;
    
    if (plineRight) {
        plineRight->LINE_plistPrev = plineNew;
    }
    
    plineLeft->LINE_plistNext = plineNew;
}
/*********************************************************************************************************
** 函数名称: _List_Line_Del
** 功能描述: 删除一个节点
** 输　入  : plingDel      需要删除的节点
**           pplingHeader  链表头
** 输　出  : NONE
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
VOID  _List_Line_Del (PLW_LIST_LINE  plineDel, LW_LIST_LINE_HEADER  *pplineHeader)
{
    if (plineDel->LINE_plistPrev == LW_NULL) {                          /*  表头                        */
        *pplineHeader = plineDel->LINE_plistNext;
    } else {
        plineDel->LINE_plistPrev->LINE_plistNext = plineDel->LINE_plistNext;
    }
    
    if (plineDel->LINE_plistNext) {
        plineDel->LINE_plistNext->LINE_plistPrev = plineDel->LINE_plistPrev;
    }
}
/*********************************************************************************************************
  链表结构初始化
*********************************************************************************************************/

#define	_LIST_RING_INIT(name)	 { LW_NULL, LW_NULL }
#define _LIST_LINE_INIT(name)    { LW_NULL, LW_NULL }
#define _LIST_MONO_INIT(name)    { LW_NULL }

/*********************************************************************************************************
  链表结构初始化
*********************************************************************************************************/

#define	_LIST_RING_INIT_IN_CODE(name) do {                    \
            (name).RING_plistNext = LW_NULL;                  \
            (name).RING_plistPrev = LW_NULL;                  \
        } while (0)
#define _LIST_LINE_INIT_IN_CODE(name) do {                    \
            (name).LINE_plistNext = LW_NULL;                  \
            (name).LINE_plistPrev = LW_NULL;                  \
        } while (0)
#define _LIST_MONO_INIT_IN_CODE(name) do {                    \
            (name).MONO_plistNext = LW_NULL;                  \
        } while (0)
        
/*********************************************************************************************************
  含有变量定义的链表结构初始化
*********************************************************************************************************/

#define _LIST_RING_HEAD(name)                                 \
        LW_LIST_RING name = _LIST_RING_INIT(name)
#define _LIST_LINE_HEAD(name)                                 \
        LW_LIST_LINE name = _LIST_LINE_INIT(name)
#define _LIST_MONO_HEAD(name)                                 \
        LW_LIST_MONO name = _LIST_MONO_INIT(name)
        
/*********************************************************************************************************
  链表指针初始化
*********************************************************************************************************/

#define _INIT_LIST_RING_HEAD(ptr) do {                        \
            (ptr)->RING_plistNext = LW_NULL;                  \
            (ptr)->RING_plistPrev = LW_NULL;                  \
        } while (0)
#define _INIT_LIST_LINE_HEAD(ptr) do {                        \
            (ptr)->LINE_plistNext = LW_NULL;                  \
            (ptr)->LINE_plistPrev = LW_NULL;                  \
        } while (0)
#define _INIT_LIST_MONO_HEAD(ptr) do {                        \
            (ptr)->MONO_plistNext = LW_NULL;                  \
        } while (0)
        
/*********************************************************************************************************
  偏移量计算
*********************************************************************************************************/

#define _LIST_OFFSETOF(type, member)                          \
        ((size_t)&((type *)0)->member)
        
/*********************************************************************************************************
  得到ptr的容器结构
*********************************************************************************************************/

#define _LIST_CONTAINER_OF(ptr, type, member)                 \
        ((type *)((size_t)ptr - _LIST_OFFSETOF(type, member)))
        
/*********************************************************************************************************
  获得链表母体指针结构
*********************************************************************************************************/

#define _LIST_ENTRY(ptr, type, member)                        \
        _LIST_CONTAINER_OF(ptr, type, member)
        
/*********************************************************************************************************
  判断链表是否为空
*********************************************************************************************************/

#define _LIST_RING_IS_EMPTY(ptr)                              \
        ((ptr) == LW_NULL)
#define _LIST_LINE_IS_EMPTY(ptr)                              \
        ((ptr) == LW_NULL)
#define _LIST_MONO_IS_EMPTY(ptr)                              \
        ((ptr) == LW_NULL)
        
/*********************************************************************************************************
  判断节点是否不在链表中
*********************************************************************************************************/

#define _LIST_RING_IS_NOTLNK(ptr)                             \
        (((ptr)->RING_plistNext == LW_NULL) || ((ptr)->RING_plistPrev == LW_NULL))
#define _LIST_LINE_IS_NOTLNK(ptr)                             \
        (((ptr)->LINE_plistNext == LW_NULL) && ((ptr)->LINE_plistPrev == LW_NULL))

/*********************************************************************************************************
  资源表初始化连接
*********************************************************************************************************/

#define _LIST_MONO_LINK(ptr, ptrnext)                         \
        ((ptr)->MONO_plistNext = (ptrnext))

/*********************************************************************************************************
  下一个
*********************************************************************************************************/

static LW_INLINE VOID _list_ring_next (PLW_LIST_RING  *phead)
{
    *phead = (*phead)->RING_plistNext;
}
static LW_INLINE VOID _list_line_next (PLW_LIST_LINE  *phead)
{
    *phead = (*phead)->LINE_plistNext;
}
static LW_INLINE VOID _list_mono_next (PLW_LIST_MONO  *phead)
{
    *phead = (*phead)->MONO_plistNext;
}

/*********************************************************************************************************
  获取下一个
*********************************************************************************************************/

static LW_INLINE PLW_LIST_RING    _list_ring_get_next (PLW_LIST_RING  pring)
{
    return  (pring->RING_plistNext);
}
static LW_INLINE PLW_LIST_LINE    _list_line_get_next (PLW_LIST_LINE  pline)
{
    return  (pline->LINE_plistNext);
}
static LW_INLINE PLW_LIST_MONO    _list_mono_get_next (PLW_LIST_MONO  pmono)
{
    return  (pmono->MONO_plistNext);
}

/*********************************************************************************************************
  获取上一个
*********************************************************************************************************/

static LW_INLINE PLW_LIST_RING    _list_ring_get_prev (PLW_LIST_RING  pring)
{
    return  (pring->RING_plistPrev);
}
static LW_INLINE PLW_LIST_LINE    _list_line_get_prev (PLW_LIST_LINE  pline)
{
    return  (pline->LINE_plistPrev);
}

/*********************************************************************************************************
  资源缓冲池分配与回收操作
*********************************************************************************************************/

static LW_INLINE PLW_LIST_MONO _list_mono_allocate (PLW_LIST_MONO  *phead)
{
    REGISTER  PLW_LIST_MONO  pallo;
    
    pallo  = *phead;
    *phead = (*phead)->MONO_plistNext;
    
    return  (pallo);
}
static LW_INLINE VOID _list_mono_free (PLW_LIST_MONO  *phead, PLW_LIST_MONO  pfree)
{
    pfree->MONO_plistNext = *phead;
    *phead = pfree;
}

/*********************************************************************************************************
  资源缓冲池分配与回收操作 (顺序资源链, 预防句柄卷绕)
*********************************************************************************************************/

static LW_INLINE PLW_LIST_MONO _list_mono_allocate_seq (PLW_LIST_MONO  *phead, PLW_LIST_MONO  *ptail)
{
    REGISTER  PLW_LIST_MONO  pallo;
    
    pallo = *phead;
    if (*ptail == *phead) {
        *ptail = (*phead)->MONO_plistNext;
    }
    *phead = (*phead)->MONO_plistNext;
    
    return  (pallo);
}
static LW_INLINE VOID _list_mono_free_seq (PLW_LIST_MONO  *phead, PLW_LIST_MONO  *ptail, PLW_LIST_MONO  pfree)
{
    pfree->MONO_plistNext = LW_NULL;
    if (*ptail) {                                                       /*  存在有空闲节点时            */
        (*ptail)->MONO_plistNext = pfree;
    } else {
        *phead = pfree;                                                 /*  没有空闲节点                */
    }
    *ptail = pfree;
}
/*********************************************************************************************************
  双向环形队列表
*********************************************************************************************************/

typedef struct __list_ring {
    struct __list_ring      *RING_plistNext;                            /*  环形表前向指针              */
    struct __list_ring      *RING_plistPrev;                            /*  环形表后向指针              */
} LW_LIST_RING;
typedef LW_LIST_RING        *PLW_LIST_RING;
typedef PLW_LIST_RING        LW_LIST_RING_HEADER;                       /*  环表表头                    */

/*********************************************************************************************************
  双向线形管理表
*********************************************************************************************************/

typedef struct __list_line {
    struct __list_line      *LINE_plistNext;                            /*  线形表前向指针              */
    struct __list_line      *LINE_plistPrev;                            /*  线形表后向指针              */
} LW_LIST_LINE;
typedef LW_LIST_LINE        *PLW_LIST_LINE;
typedef PLW_LIST_LINE        LW_LIST_LINE_HEADER;                       /*  线形表表头                  */

/*********************************************************************************************************
  单向资源分配表
*********************************************************************************************************/

typedef struct __list_mono {
    struct __list_mono      *MONO_plistNext;                            /*  资源表前向指针              */
} LW_LIST_MONO;
typedef LW_LIST_MONO        *PLW_LIST_MONO;
typedef PLW_LIST_MONO        LW_LIST_MONO_HEADER;                       /*  资源表表头                  */

/*********************************************************************************************************
  哈希链表
*********************************************************************************************************/

typedef struct __hlist_node {
    struct __hlist_node     *HNDE_phndeNext;                            /*  前向指针                    */
    struct __hlist_node    **HNDE_pphndePrev;                           /*  后向双指针                  */
} LW_HLIST_NODE;
typedef LW_HLIST_NODE       *PLW_HLIST_NODE;                            /*  节点指针类型                */

typedef struct __hlist_head {
    struct __hlist_node     *HLST_phndeFirst;                           /*  第一个节点                  */
} LW_HLIST_HEAD;
typedef LW_HLIST_HEAD       *PLW_HLIST_HEAD;                            /*  头指针类型                  */

/*********************************************************************************************************
  SMP spinlock 初始化与状态判断.
*********************************************************************************************************/

VOID  _SmpSpinInit(spinlock_t *psl);
BOOL  _SmpSpinIsLocked(spinlock_t *psl);
VOID  _SmpSpinUnlockWait(spinlock_t *psl);

#define LW_SPIN_INIT(psl)         _SmpSpinInit(psl)
#define LW_SPIN_IS_LOCKED(psl)    _SmpSpinIsLocked(psl)
#define LW_SPIN_UNLOCK_WAIT(psl)  _SmpSpinUnlockWait(psl)

/*********************************************************************************************************
  SMP 内核级 spinlock 操作, 可以在多任务与中断服务上下文中使用
*********************************************************************************************************/

VOID  _SmpSpinLock(spinlock_t *psl);
BOOL  _SmpSpinTryLock(spinlock_t *psl);
INT   _SmpSpinUnlock(spinlock_t *psl);

VOID  _SmpSpinLockIgnIrq(spinlock_t *psl);
BOOL  _SmpSpinTryLockIgnIrq(spinlock_t *psl);
VOID  _SmpSpinUnlockIgnIrq(spinlock_t *psl);

VOID  _SmpSpinLockIrq(spinlock_t *psl, INTREG  *piregInterLevel);
BOOL  _SmpSpinTryLockIrq(spinlock_t *psl, INTREG  *piregInterLevel);
INT   _SmpSpinUnlockIrq(spinlock_t *psl, INTREG  iregInterLevel);

VOID  _SmpSpinLockIrqQuick(spinlock_t *psl, INTREG  *piregInterLevel);
VOID  _SmpSpinUnlockIrqQuick(spinlock_t *psl, INTREG  iregInterLevel);

#define LW_SPIN_LOCK(psl)               _SmpSpinLock(psl)
#define LW_SPIN_TRYLOCK(psl)            _SmpSpinTryLock(psl)
#define LW_SPIN_UNLOCK(psl)             _SmpSpinUnlock(psl)

#define LW_SPIN_LOCK_IGNIRQ(psl)        _SmpSpinLockIgnIrq(psl)
#define LW_SPIN_TRYLOCK_IGNIRQ(psl)     _SmpSpinTryLockIgnIrq(psl)
#define LW_SPIN_UNLOCK_IGNIRQ(psl)      _SmpSpinUnlockIgnIrq(psl)

#define LW_SPIN_LOCK_IRQ(psl, pireg)    _SmpSpinLockIrq(psl, pireg)
#define LW_SPIN_TRYLOCK_IRQ(psl, pireg) _SmpSpinTryLockIrq(psl, pireg)
#define LW_SPIN_UNLOCK_IRQ(psl, ireg)   _SmpSpinUnlockIrq(psl, ireg)

#define LW_SPIN_LOCK_QUICK(psl, pireg)  _SmpSpinLockIrqQuick(psl, pireg)
#define LW_SPIN_UNLOCK_QUICK(psl, ireg) _SmpSpinUnlockIrqQuick(psl, ireg)

/*********************************************************************************************************
  SMP 原始 spinlock 操作
*********************************************************************************************************/

VOID  _SmpSpinLockRaw(spinlock_t *psl, INTREG  *piregInterLevel);
BOOL  _SmpSpinTryLockRaw(spinlock_t *psl, INTREG  *piregInterLevel);
VOID  _SmpSpinUnlockRaw(spinlock_t *psl, INTREG  iregInterLevel);

#define LW_SPIN_LOCK_RAW(psl, pireg)     _SmpSpinLockRaw(psl, pireg)
#define LW_SPIN_TRYLOCK_RAW(psl, pireg)  _SmpSpinTryLockRaw(psl, pireg)
#define LW_SPIN_UNLOCK_RAW(psl, ireg)    _SmpSpinUnlockRaw(psl, ireg)

/*********************************************************************************************************
  SMP 前向兼容符号 (已废弃)
*********************************************************************************************************/

VOID  _SmpSpinLockTask(spinlock_t *psl);
BOOL  _SmpSpinTryLockTask(spinlock_t *psl);
INT   _SmpSpinUnlockTask(spinlock_t *psl);

#define LW_SPIN_LOCK_TASK(psl)     _SmpSpinLockTask(psl)
#define LW_SPIN_TRYLOCK_TASK(psl)  _SmpSpinTryLockTask(psl)
#define LW_SPIN_UNLOCK_TASK(psl)   _SmpSpinUnlockTask(psl)

/*********************************************************************************************************
  SMP 内核锁操作.
*********************************************************************************************************/

VOID  _SmpKernelLockIgnIrq(VOID);
VOID  _SmpKernelUnlockIgnIrq(VOID);

VOID  _SmpKernelLockQuick(INTREG  *piregInterLevel);
VOID  _SmpKernelUnlockQuick(INTREG  iregInterLevel);
VOID  _SmpKernelUnlockSched(struct __lw_tcb *ptcbOwner);

#define LW_SPIN_KERN_LOCK_IGNIRQ()       _SmpKernelLockIgnIrq()
#define LW_SPIN_KERN_UNLOCK_IGNIRQ()     _SmpKernelUnlockIgnIrq()

#define LW_SPIN_KERN_LOCK_QUICK(pireg)   _SmpKernelLockQuick(pireg)
#define LW_SPIN_KERN_UNLOCK_QUICK(ireg)  _SmpKernelUnlockQuick(ireg)
#define LW_SPIN_KERN_UNLOCK_SCHED(ptcb)  _SmpKernelUnlockSched(ptcb)

/*********************************************************************************************************
  SMP 内核时间锁操作.
*********************************************************************************************************/

VOID  _SmpKernTimeLockIgnIrq(VOID);
VOID  _SmpKernTimeUnlockIgnIrq(VOID);

VOID  _SmpKernTimeLockQuick(INTREG  *piregInterLevel);
VOID  _SmpKernTimeUnlockQuick(INTREG  iregInterLevel);

#define LW_SPIN_KERN_TIME_LOCK_IGNIRQ()       _SmpKernTimeLockIgnIrq()
#define LW_SPIN_KERN_TIME_UNLOCK_IGNIRQ()     _SmpKernTimeUnlockIgnIrq()

#define LW_SPIN_KERN_TIME_LOCK_QUICK(pireg)   _SmpKernTimeLockQuick(pireg)
#define LW_SPIN_KERN_TIME_UNLOCK_QUICK(ireg)  _SmpKernTimeUnlockQuick(ireg)


#endif /* A274D2E0_32E0_4EF5_8D41_E7AAAD376DE1 */

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "iwdg.h"   /* HAL_IWDG_Refresh / hiwdg */
#include "m1_net.h"   /* M1：以太网应用任务入口 */
#include "m2_master.h" /* M2：Modbus RTU 主站轮询任务入口 */
#include "m3_can.h"   /* M3：CAN 驱动与双 CAN 网关任务入口 */
#include "m4_log.h"   /* M4：日志（环形缓冲 printf，修 BUG-039） */
#include "m4_pool.h"  /* M4：统一数据池 */
#include "m4_task.h"  /* M4：监控任务 + 存活事件组 */
#include "m5_link.h"  /* M5：链路冗余（主备切换） */
#include "m5_wdg.h"   /* M5：看门狗裁决 */
#include "m6_store.h" /* M6：断网补传缓存（W25Q128） */
#include <stdio.h>  /* 栈溢出 hook 里要用 printf */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* ⭐⭐ 固件版本号 —— 【每次做 OTA 验证都要改这里】
   ⚠️ 为什么必须改：OTA 下发一份和当前一模一样的固件时，
      切换成不成功从串口完全看不出来（输出一模一样），
      你就无法证明"Bootloader 真的完成了 B→A 拷贝"。
      改版本号后重启看 `[sys] fw=` 这一行，是 OTA 闭环的唯一铁证。
   当前：v1 = 基线（已备份 ota_backup/app_good_20261022.*）
        v2 = 第一次 OTA 验证用 */
#define FW_VER_STR   "v3-len-fix"

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  /* M1 调整 128*4(512B) → 256*4(1024B)：
     MX_LWIP_Init() 就在这个任务里调用，里面还有 PHY 地址扫描的 printf，
     ARMCC 的 printf 内部要吃掉 200~400 字节栈，512B 会溢出。 */
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

extern void MX_LWIP_Init(void);
void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   /* 栈溢出时停在这里，串口打出任务名 —— 静默跑飞最难查 */
   printf("!! STACK OVERFLOW: %s\r\n", (char *)pcTaskName);
   taskDISABLE_INTERRUPTS();
   for (;;);
}
/* USER CODE END 4 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  /* 这条打印发生在调度器启动之前（还在 main 的上下文里）。
     它的唯一作用：当后面串口一片安静时，能分清是"系统压根没跑起来"
     还是"跑起来了但卡在某个地方"。 */
  /* ⚠️ 这条打印发生在调度器启动之前，会走 m4_log 的"直接阻塞发送"兜底路径
     （那时还没有任务概念，不能用环形缓冲）。这是故意的：
     当后面串口一片安静时，能分清是"系统压根没跑起来"还是"跑起来了但卡住了"。 */
  /* ⭐ OTA 验收用：把固件版本号打在启动第一行。
     ⚠️ 为什么必须要有这个：OTA 如果下发的是【和当前一模一样】的固件，
        切换成不成功从日志上完全看不出来 —— 串口输出一模一样，
        你无法区分"Bootloader 真的把 Bank B 拷到 Bank A 了"
        还是"根本没切换、只是又跑了一遍老固件"。
        所以每次做 OTA 验证都要先改这个版本号（v1 → v2 → v3 …），
        重启后看这一行变没变，就是 OTA 链路是否真的闭环的铁证。 */
  printf("[sys] fw=%s  build=%s %s\r\n", FW_VER_STR, __DATE__, __TIME__);
  printf("[sys] boot ok, FreeRTOS starting...\r\n");
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */

  /* ---- M4-⓪ 日志：必须最先启动 ----
     之后所有任务的 printf 都会走环形缓冲（不再多任务重入丢行，见 BUG-039）。
     在它之前 printf 仍然可用，只是走阻塞发送兜底路径。 */
  M4_LogStart();

  /* ---- M4-① 数据池互斥量（幂等，多个模块都会调）---- */
  M4_PoolInit();

  /* ---- M5：链路冗余抽象层（必须在 m1_net_start 之前）----
     它内部会初始化 ESP8266（USART3 DMA + IDLE），并准备好"主/备/降级"三态。
     ⚠️ 顺序：m1_net_start() 里会把以太网的发送回调注册进 m5_link，
        所以 m5_link 必须先初始化好。 */
  M5_LinkInit();

  /* ---- M6：断网补传缓存（W25Q128 / SPI1 + PB14 片选）----
     ⚠️ 顺序：必须在 m1_net_start() 之前，这样上报任务第一帧发出去时
        缓存区已经可用（否则 DEGRADED 第一次触发时会漏掉缓存）。
     ⚠️ 为什么可以放在调度器启动【之前】：
        M6_StoreInit() 只做【读】（读 JEDEC ID + 扫描已有槽位），
        不擦不写，所以内部不会走到 w25q_wait_busy()（那里才用 osDelay）。
        256 个槽全扫一遍约 8~20 ms，这点启动延迟可以接受。
     ⚠️ 失败不会卡死：找不到 W25Q128 就打印一条并禁用缓存，
        系统退化成 M5 的"只计数"行为 —— 降级可用，不硬失败。 */
  M6_StoreInit();

  /* M1/M4：命令服务（TCP Server 5000）+ 周期上报（TCP Client 5001）。
     两个任务内部都会先等 netif 就绪，所以放在这里创建是安全的。 */
  m1_net_start();

  /* M2：Modbus RTU 主站轮询（RS485 / USART6 / 9600 8N1）。
     内部会 MB_Init() → RS485_Init()（开 DMA 循环接收 + IDLE 中断），
     所以必须放在调度器启动之后 —— 这里正好满足。 */
  /* TODO(M3-a)：M3 回环/对连测完必须取消下面的注释，把 M2 恢复回来 */
  // M2_MasterStart();

  /* M3：CAN 驱动 + 双 CAN 网关（CAN1 PA11/PA12，CAN2 PB12/PB13，500 kbps）。
     内部会重新 HAL_CAN_Init（改 AutoRetransmission / 回环模式）→ ConfigFilter
     → HAL_CAN_Start → ActivateNotification，必须晚于 MX_CAN1_Init / MX_CAN2_Init。
     ⚠️ 调 M3-a 回环自测前，建议先把上面 M2_MasterStart() 注释掉：
        M2 没接从机会一直刷 OFFLINE 日志，干扰观察。 */
  /* ⭐ M5-a 验收【已完成】：曾临时注释掉以制造 "missing 0x04 CAN"，
     实测链：10 s 宽限 → REFUSE to feed ×3 → IWDG 复位 → cause=IWDG
             → 黑匣子 miss=0x04 CAN、uptime=11s、reset_cnt 正确累加。
     ⚠️ 已恢复，不要再注释（除非重做该验收）。 */
  M3_CanStart();

  /* M4：系统监控任务（heap/栈余量/存活事件组/数据池摘要）。
     ⚠️ 放最后：它会立即开始读其他任务的栈水线，晚一点创建能避开
        "任务还没建好就被检查"的首轮误报。 */
  M4_TaskStart();
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* init code for LWIP */
  MX_LWIP_Init();
  /* USER CODE BEGIN StartDefaultTask */
  /* 能打出来说明 HAL_ETH_Init / netif_add / tcpip_thread 全部走通了。
     如果串口只有 "[sys] boot ok" 而没有这一行，说明卡在 MX_LWIP_Init() 内部
     （最常见是 HAL_ETH_Init 里 DMA 软复位超时 → Error_Handler）。 */
  printf("[sys] LWIP init done\r\n");

  /* Infinite loop */
  for(;;)
  {
    /* ⭐ M5：这里的无脑喂狗【已删除】。
       它在 M1~M4 是临时修法（BUG-034），盲区很大：
       只要本任务在转就喂，其他任务全死光也发现不了 —— 形同虚设。
       现在喂狗由 m4_mon 每秒裁决（m5_wdg.c 的 M5_WdgKick），
       ⚠️ 全工程只有那一处 HAL_IWDG_Refresh()，多一处整套裁决就失效。

       本任务改做【链路守护】：每 200 ms 驱动一次 ESP8266 状态机 + 链路切换裁决。
       为什么不新建任务：FreeRTOS heap 只剩 ~8 KB，M5 不起新任务（见 M5 文档硬约束①）。 */
    M5_LinkTick(200u);

    osDelay(200);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */


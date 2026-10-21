/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "cmsis_os.h"
#include "can.h"
#include "dma.h"
#include "i2c.h"
#include "iwdg.h"
#include "lwip.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "wwdg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

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

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */
void MX_GPIO_PostInit(void);   /* 定义在 Core/Src/gpio.c 的 USER CODE 2 */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#include <stdio.h>
#include <string.h>
#include "m4_log.h"
#include "stm32f4xx_hal_wwdg.h"   /* HAL_WWDG_EnableIT()（main.h 没有间接带进来） */
#include "m5_wdg.h"
#include "m5_blackbox.h"
#include "m5_ota.h"
#include "m5_fan.h"
#include "shared/flash_layout.h"

/* printf 重定向到 USART1(PA9/PA10, 115200)。
   Keil 下请在 Options → Target 勾选 "Use MicroLIB"，否则链接时会报
   __stdout / _sys_exit 之类的半主机(semihosting)符号未定义错误。

   ⚠️ M4 改动（修 BUG-039）：
     原来这里是直接 HAL_UART_Transmit 轮询发送，没有任何保护 ——
     两个任务同时 printf 会互相插队，表现为丢行、错序（M1 实测时
     "[echo] listening on port 5000" 整行丢失就是这个原因），
     而且一行要阻塞 3~4ms，会把高优先级任务拖住。

     现在改走 m4_log 的环形缓冲：fputc 只做微秒级的入队，
     真正的串口发送交给一个低优先级任务（m4_log）慢慢发。 */
int fputc(int ch, FILE *f)
{
  M4_LogPutc((char)ch);
  return ch;
}

/*==============================================================================
 * M5 · 复位诊断：上电第一件事就是"交代上次是怎么死的"
 *
 * ⚠️ 为什么必须在所有外设初始化之前：
 *    RCC_CSR 的复位标志不会自动清，一旦 IWDG 启动并发生复位，
 *    你就再也读不到"第一次"的原因了。所以【上电立刻读 + 立刻清】。
 *
 * ⚠️ 用 HAL 的宏名，不要手写 bit 号 —— 不同系列 bit 位置不一样，
 *    抄错就是静默错误（读到 0 就显示"上电复位"，根本看不出异常）。
 *============================================================================*/
/* ⚠️ M5 修正（BUG-059）：原来"读标志 + 打印"写在一个函数里，而它在
 *    MX_USART1_UART_Init() 【之前】被调用 —— 那时 huart1.gState 还是
 *    HAL_UART_STATE_RESET，HAL_UART_Transmit() 会直接返回 HAL_BUSY，
 *    一个字节都发不出去，于是【上电第一行的 [rst] cause=... 永远看不到】。
 *
 *    所以拆成两步：
 *      ① snapshot_reset_cause() —— 上电立刻执行：读 RCC_CSR 并清标志（必须在
 *         IWDG 启动前，否则第一次死因会被后续复位覆盖）；结果只存进静态变量。
 *      ② print_reset_cause()    —— 在 UART 初始化完成后再打印（USER CODE BEGIN 2）。
 */
static const char *s_rst_cause = "UNKNOWN";
static int         s_rst_is_wdg;

static void snapshot_reset_cause(void)
{
  if      (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) { s_rst_cause = "IWDG";  s_rst_is_wdg = 1; }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) { s_rst_cause = "WWDG";  s_rst_is_wdg = 1; }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))  { s_rst_cause = "POR(power-on)"; }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))  { s_rst_cause = "PIN(reset button)"; }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))  { s_rst_cause = "SOFT(software/OTA)"; }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))  { s_rst_cause = "BOR(brown-out)"; }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST)) { s_rst_cause = "LPWR"; }
  else                                           { s_rst_cause = "UNKNOWN"; s_rst_is_wdg = 0; }

  /* ⭐ 读完必须清，否则下次上电读到的是上上次的原因 */
  __HAL_RCC_CLEAR_RESET_FLAGS();
}

static void print_reset_cause(void)
{
  printf("[rst] cause=%s\r\n", s_rst_cause);

  /* 看门狗复位 → 把黑匣子打出来，这是排障的钥匙 */
  if (s_rst_is_wdg)
  {
    M5_BlackboxPrint();
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* ⭐ M5：上电第一件事 —— 先把上次怎么死的【快照】下来（必须在 IWDG 启动前读标志）。
     这里只存不打印：UART 还没初始化，现在 printf 会一个字节都发不出去（BUG-059）。 */
  snapshot_reset_cause();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  MX_USART6_UART_Init();
  MX_CAN1_Init();
  MX_CAN2_Init();
  MX_IWDG_Init();
  MX_TIM3_Init();
  /* ⭐ M5：WWDG 放开。
     喂狗点在 WWDG 的 EWI 中断回调里（m5_wdg.c 的 HAL_WWDG_EarlyWakeupCallback），
     由硬件节拍驱动，天然踩在窗口正中间，不存在"来不及喂"的问题。

     ⚠️ EWIMode=ENABLE 只是配了寄存器选项，【中断开关要单独开】：
        少了 HAL_WWDG_EnableIT() 这一行，WWDG 会永远在 ~50 ms 时复位。 */
#if M5_WWDG_ENABLE
  /* ⚠️ WWDG 默认关闭（M5_WWDG_ENABLE=0）—— 它与 OTA 擦 Flash 天然冲突：
     擦除期间 CPU 停摆 > 50 ms，EWI 进不来必然复位。详见 m5_wdg.h 的说明。
     做 WWDG 专项验证（M5-b）时把它置 1，且【不要同时做 OTA】。 */
  MX_WWDG_Init();
  /* ⚠️ 这个 HAL 版本【没有】HAL_WWDG_EnableIT() 函数（只有宏），
     实测链接会报 L6218E: Undefined symbol HAL_WWDG_EnableIT。
     EWI 中断位由 MX_WWDG_Init() 内部的
         MODIFY_REG(CFR, ..., Window | Prescaler | EWIMode)
     按 hwwdg.Init.EWIMode = WWDG_EWI_ENABLE 直接置好了，
     这里再用宏显式置一次（幂等，换 HAL 版本也不会漏）。 */
  __HAL_WWDG_ENABLE_IT(&hwwdg, WWDG_IT_EWI);
#endif /* M5_WWDG_ENABLE */
  /* ⚠️ NVIC 也要单独开：HAL_WWDG_EnableIT() 只置了 WWDG 的 CFR 寄存器位，
     不会帮你开 NVIC 通道。缺这两行 → EWI 中断根本进不到 CPU。
     优先级给 0（最高）：喂狗中断被别的中断堵住就等于没喂。 */
  HAL_NVIC_SetPriority(WWDG_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(WWDG_IRQn);
  MX_I2C1_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  /* 修正 CubeMX 一律生成低电平的那几个输出脚（片选/方向/LED/继电器）。
     必须在 MX_GPIO_Init() 之后调用，见 gpio.c 里的说明。 */
  MX_GPIO_PostInit();

  /* ⭐ M5：到这里 UART 已经初始化完毕，把开头快照到的复位原因真正打出来。
     ⚠️ 顺序不能反：早于 UART 初始化打印 = 静默丢失（HAL_UART_Transmit 返回 HAL_BUSY）。 */
  print_reset_cause();

  /* ⭐ M5：看门狗裁决器 + OTA 状态 + PWM 风扇 的初始化。
     ⚠️ 顺序：必须在【创建任务之前】完成，
        否则任务一跑起来就可能用到还没初始化的模块（尤其是喂狗裁决器）。 */
  M5_WdgInit();
  M5_OtaInit();
  M5_FanInit();

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

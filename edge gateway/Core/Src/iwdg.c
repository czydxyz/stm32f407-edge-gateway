/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    iwdg.c
  * @brief   This file provides code for the configuration
  *          of the IWDG instances.
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
#include "iwdg.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

IWDG_HandleTypeDef hiwdg;

/* IWDG init function */
void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  /* ⭐ M5：预分频 32 → 64。
     原因：IWDG 用 LSI（内部 RC），标称 32 kHz 但实测可能是 17~47 kHz。
     超时 T = (Reload+1) × Prescaler / f_LSI：
        32 分频：标称 2.00 s，但 LSI 偏快到 47 kHz 时只有【1.36 s】
                 → 喂狗周期一旦超过它就莫名其妙复位
        64 分频：标称 4.00 s，最快(47kHz) 2.72 s，最慢(17kHz) 7.53 s
     M5 的喂狗周期是 1 s（monitor 每秒裁决一次），
     要求"最快情况下超时 > 2 × 喂狗周期" = 2 s → 64 分频刚好满足。 */
  hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
  hiwdg.Init.Reload = 2000;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */


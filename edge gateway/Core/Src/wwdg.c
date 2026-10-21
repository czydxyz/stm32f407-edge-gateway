/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    wwdg.c
  * @brief   This file provides code for the configuration
  *          of the WWDG instances.
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
#include "wwdg.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

WWDG_HandleTypeDef hwwdg;

/* WWDG init function */
void MX_WWDG_Init(void)
{

  /* USER CODE BEGIN WWDG_Init 0 */

  /* USER CODE END WWDG_Init 0 */

  /* USER CODE BEGIN WWDG_Init 1 */

  /* USER CODE END WWDG_Init 1 */
  hwwdg.Instance = WWDG;
  hwwdg.Init.Prescaler = WWDG_PRESCALER_8;
  hwwdg.Init.Window = 80;
  hwwdg.Init.Counter = 127;
  /* ⭐ M5：EWI 打开。
     为什么必须用 EWI 喂狗：F4 上 WWDG 最长超时只有约 50 ms
     （T = 4096×8×(127-63)/42MHz ≈ 49.9 ms），不可能让 1 s 周期的任务去喂。
     所以让硬件在 counter==0x40 时先给中断，在中断里裁决要不要续命。

     ⚠️ Window 保持 80【不要改小】：
        EWI 在 counter==64 时喂狗，而喂狗合法条件是 counter ≤ Window，
        即 64 ≤ Window → Window 必须 ≥ 0x40(64)。
        直觉上"Window 小 = 限制少"是【错的】：Window 是"允许喂狗的 counter 上限"，
        改小到 48 会导致窗口永远打不开，一开 WWDG 就立刻复位。 */
  hwwdg.Init.EWIMode = WWDG_EWI_ENABLE;
  if (HAL_WWDG_Init(&hwwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN WWDG_Init 2 */

  /* USER CODE END WWDG_Init 2 */

}

void HAL_WWDG_MspInit(WWDG_HandleTypeDef* wwdgHandle)
{

  if(wwdgHandle->Instance==WWDG)
  {
  /* USER CODE BEGIN WWDG_MspInit 0 */

  /* USER CODE END WWDG_MspInit 0 */
    /* WWDG clock enable */
    __HAL_RCC_WWDG_CLK_ENABLE();
  /* USER CODE BEGIN WWDG_MspInit 1 */

  /* USER CODE END WWDG_MspInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */


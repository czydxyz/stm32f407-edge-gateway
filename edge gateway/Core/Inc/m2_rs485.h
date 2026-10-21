#ifndef __M2_RS485_H
#define __M2_RS485_H

/*==============================================================================
 * M2 · RS485 半双工驱动（物理层）
 *
 * 职责边界（很重要，别越界）：
 *   本文件只管「怎么把一串字节安全地送上 RS485 总线」以及
 *   「怎么知道一串字节收完了」。它完全不关心字节的含义 ——
 *   那是 Modbus 层（m2_modbus.c）的事。
 *
 * 硬件：
 *   USART6  PC6(TX) / PC7(RX)，AF8，DMA2_Stream1 循环接收
 *   方向脚  PE5 —— 板载 TP8485E 的 DE 与 RE 短接后的那个脚
 *           1 = 发送使能 / 接收禁止
 *           0 = 发送禁止 / 接收使能
 *============================================================================*/

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* ---------- 方向控制 ---------- */
#define RS485_DIR_GPIO_PORT   GPIOE
#define RS485_DIR_GPIO_PIN    GPIO_PIN_5

#define RS485_TX_EN()  HAL_GPIO_WritePin(RS485_DIR_GPIO_PORT, RS485_DIR_GPIO_PIN, GPIO_PIN_SET)
#define RS485_RX_EN()  HAL_GPIO_WritePin(RS485_DIR_GPIO_PORT, RS485_DIR_GPIO_PIN, GPIO_PIN_RESET)

/*---------- DMA 循环接收缓冲 ----------
 * Modbus RTU 单帧上限 256 字节。取 512 而不是 256，是为了让
 * "写满一整圈" 和 "一个字节都没有" 这两种状态在游标上可区分
 * （都是 pos 相等，但 512 的余量让我们几乎不可能真的绕满一圈）。 */
#define RS485_RX_BUF_SIZE   512

/*---------- t3.5：Modbus RTU 的帧间静默时间 ----------
 * RTU 靠「总线空闲 ≥ 3.5 个字符时间」来判定一帧结束（没有帧头帧尾）。
 * 9600bps / 8N1（1 起始 + 8 数据 + 1 停止 = 11 bit/字符）：
 *     t3.5 = 3.5 × 11 / 9600 ≈ 4.0 ms
 * 取 5ms 作为保守值。
 * ⚠️ 换波特率必须重算这个值：19200 下是 2ms，115200 下是 0.34ms
 *    （规范规定 >19200 时固定用 1.75ms）。 */
#define RS485_T35_MS        5

/* ---------- API ---------- */
void     RS485_Init(void);                              /* 方向脚置接收态 + 开 DMA + 开 IDLE 中断 */
void     RS485_Send(const uint8_t *buf, uint16_t len);  /* 阻塞发送，内部做方向切换并等 TC */
void     RS485_WaitSilent(void);                        /* 等到总线静默 ≥ t3.5（发下一帧前调用） */
uint16_t RS485_ReadFrame(uint8_t *out, uint16_t maxlen);/* 取一整帧，返回长度，0 = 还没收完 */
void     RS485_RxFlush(void);                           /* 丢弃当前所有已收字节（发请求前调用） */
void     RS485_IdleCallback(void);                      /* 由 USART6_IRQHandler 在 IDLE 时调用 */

#endif /* __M2_RS485_H */

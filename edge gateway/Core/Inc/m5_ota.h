/*==============================================================================
 * M5 · OTA 下载（分块 + 断点续传 + 累计 CRC + 双 Bank 防砖）
 *
 * 流程：
 *   OTA_BEGIN(magic, total_len, total_crc, block_size)
 *       ├ magic/len 校验
 *       ├ 参数区记录的 crc 与本次一致 且 received>0 → 【跳过擦除，直接续传】
 *       └ 否则擦 Bank B（S8~S11，逐个扇区，扇区间喂狗）
 *   OTA_DATA(offset, seq, block_crc, data)
 *       ├ ⭐ 要求 offset 严格等于 received（乱序直接要求重传）
 *       │   换来的是"断点续传极简单"：进度用 received 一个数就能表示，
 *       │   不需要维护"哪些块收到了"的位图
 *       ├ 校验本块 CRC32 → 不符回 CRC_ERR，上位机重发本块
 *       └ 写 Flash + 累计 CRC，回 ACK(offset=received)
 *   OTA_END
 *       ├ received == total_len？
 *       ├ 累计 CRC == total_crc？
 *       └ 写参数区 ota_state = OTA_READY → 复位 → 由 Bootloader 裁决
 *
 * ⭐ 防砖的第一原则：App 校验通过【不算数】，
 *    必须由 Bootloader 自己再算一遍 Bank B 的 CRC 才允许切换。
 *    因为 App 本身可能是坏的，它写的"我校验通过了"也不可信。
 *============================================================================*/

#ifndef __M5_OTA_H
#define __M5_OTA_H

#include <stdint.h>

/*==============================================================================
 * API（全部由 m1_net 的命令处理器调用）
 *
 * @param d, n   收到的 DATA 字段
 * @param ack    应答帧输出缓冲
 * @param ack_max 缓冲大小
 * @return       >0 = 应答帧长度（调用方负责发出去）；<=0 = 参数错误
 *============================================================================*/
void M5_OtaInit(void);

int  M5_OtaBegin(const uint8_t *d, uint8_t n, uint8_t *ack, uint32_t ack_max);
int  M5_OtaData (const uint8_t *d, uint8_t n, uint8_t *ack, uint32_t ack_max);
int  M5_OtaEnd  (const uint8_t *d, uint8_t n, uint8_t *ack, uint32_t ack_max);

/* 是否正在 OTA（1 = 下载中）。业务层可据此暂停上报，避免与写 Flash 抢时间。 */
int  M5_OtaBusy(void);

/* 当前 OTA 进度（0~100），没在 OTA 时返回 -1 */
int  M5_OtaProgress(void);

#endif /* __M5_OTA_H */

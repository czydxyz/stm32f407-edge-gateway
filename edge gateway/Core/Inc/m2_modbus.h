#ifndef __M2_MODBUS_H
#define __M2_MODBUS_H

/*==============================================================================
 * M2 · Modbus RTU 主站协议层
 *
 * 职责：把「读从机 1 的 0 号寄存器」这类意图，翻译成一帧 RTU 报文，
 *       发出去，收回来，校验，解析成 uint16_t / bit 数组。
 *
 * 它依赖 m2_rs485.c 提供的三个原语：Send / ReadFrame / RxFlush。
 *============================================================================*/

#include <stdint.h>

/* ---------- 功能码 ---------- */
#define MB_FC_READ_COILS            0x01  /* 读线圈（可读可写的 bit，如继电器当前状态） */
#define MB_FC_READ_DISCRETE_INPUTS  0x02  /* 读离散输入（只读的 bit，如 DI） */
#define MB_FC_READ_HOLDING          0x03  /* 读保持寄存器（可读可写的 16 位） */
#define MB_FC_READ_INPUT            0x04  /* 读输入寄存器（只读的 16 位，传感器常用） */
#define MB_FC_WRITE_SINGLE_COIL     0x05
#define MB_FC_WRITE_SINGLE_REG      0x06
#define MB_FC_WRITE_MULTI_COILS     0x0F
#define MB_FC_WRITE_MULTI_REGS      0x10

/* ---------- 异常码（从机返回的错误原因）---------- */
#define MB_EX_ILLEGAL_FUNCTION      0x01
#define MB_EX_ILLEGAL_DATA_ADDR     0x02
#define MB_EX_ILLEGAL_DATA_VALUE    0x03
#define MB_EX_SLAVE_FAILURE         0x04

/* RTU 单帧上限（规范规定 256） */
#define MB_RTU_MAX_FRAME    256

/* ---------- 事务结果 ---------- */
typedef enum {
    MB_OK = 0,          /* 成功 */
    MB_ERR_TIMEOUT,     /* 超时没收到响应（从机掉线/地址错/波特率错/A B 反） */
    MB_ERR_CRC,         /* CRC 校验失败（干扰/波特率偏/帧被截断） */
    MB_ERR_SLAVE_ADDR,  /* 响应的从站地址不匹配（总线上有别的设备抢答） */
    MB_ERR_EXCEPTION,   /* 从站返回异常帧（寄存器地址越界等） */
    MB_ERR_FRAME,       /* 帧格式错误 */
    MB_ERR_PARAM,       /* 调用参数错误 */
} mb_status_t;

/* ---------- API ---------- */

/* 初始化 RS485 硬件 + 总线互斥量。必须在调度器启动后调用。 */
void MB_Init(void);

/* 把状态码翻译成人话，方便串口打印 */
const char *MB_StrError(mb_status_t st);

/* 上一次异常响应的异常码（0 = 没有异常）。调 MB_StrError 返回 EXCEPTION 时查它。 */
uint8_t MB_LastException(void);

/* CRC16（Modbus 多项式 0xA001，低字节在前） */
uint16_t MB_CRC16(const uint8_t *buf, uint16_t len);

/* ---- 读 ---- */
mb_status_t MB_ReadCoils(uint8_t slave, uint16_t addr, uint16_t cnt,
                         uint8_t *out_bits, uint32_t timeout_ms);
mb_status_t MB_ReadDiscreteInputs(uint8_t slave, uint16_t addr, uint16_t cnt,
                                  uint8_t *out_bits, uint32_t timeout_ms);
mb_status_t MB_ReadHoldingRegs(uint8_t slave, uint16_t reg_addr, uint16_t cnt,
                               uint16_t *out_regs, uint32_t timeout_ms);
mb_status_t MB_ReadInputRegs(uint8_t slave, uint16_t reg_addr, uint16_t cnt,
                             uint16_t *out_regs, uint32_t timeout_ms);

/* ---- 写 ---- */
mb_status_t MB_WriteSingleCoil(uint8_t slave, uint16_t addr, uint8_t on,
                               uint32_t timeout_ms);
mb_status_t MB_WriteSingleReg(uint8_t slave, uint16_t addr, uint16_t val,
                              uint32_t timeout_ms);
mb_status_t MB_WriteMultiCoils(uint8_t slave, uint16_t addr, uint16_t cnt,
                               const uint8_t *bits, uint32_t timeout_ms);
mb_status_t MB_WriteMultiRegs(uint8_t slave, uint16_t addr, uint16_t cnt,
                              const uint16_t *vals, uint32_t timeout_ms);

#endif /* __M2_MODBUS_H */

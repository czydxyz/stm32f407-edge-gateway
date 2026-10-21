/*==============================================================================
 * M2 · Modbus RTU 主站协议层实现
 *
 * 一个事务的完整生命周期：
 *     组帧 → 追加 CRC → 清残留 → 等静默 → 发送 → 收响应 → 判帧结束
 *          → CRC 校验 → 地址/功能码校验 → 异常判定 → 解析数据
 *============================================================================*/

#include "m2_modbus.h"
#include "m2_rs485.h"
#include "cmsis_os.h"
#include <string.h>

/*==============================================================================
 * 1. CRC16（Modbus 专用）
 *
 * 多项式 0xA001（= 0x8005 的位反转），初值 0xFFFF。
 * ⚠️ RTU 帧里 CRC 是【低字节在前】：先发 crc&0xFF，再发 crc>>8。
 *    写反了从机根本不响应（现象和波特率错一模一样，极易误判）。
 *============================================================================*/
uint16_t MB_CRC16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= buf[i];                 /* 当前字节与 CRC 低 8 位异或 */
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x0001) crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else              crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/*==============================================================================
 * 内部状态
 *============================================================================*/

/* 请求帧 / 响应帧缓冲。
   ⚠️ 故意用 static 而不是栈：每个 256 字节，放栈上会吃掉 512 字节栈空间。
      之所以敢用 static，是因为整条总线被 g_bus_mutex 串行化了 ——
      同一时刻只可能有一个事务在跑，不存在并发访问。 */
static uint8_t g_tx[MB_RTU_MAX_FRAME];
static uint8_t g_rx[MB_RTU_MAX_FRAME];

/* 总线互斥量：RS485 半双工，任何时刻只能有一个事务。
   M4 阶段网络任务要下发写指令时，也必须通过这个锁排队。 */
static osMutexId_t g_bus_mutex = NULL;

static uint8_t g_last_exc = 0;   /* 最近一次从机异常码 */

void MB_Init(void)
{
    RS485_Init();                       /* 物理层就绪 */
    g_bus_mutex = osMutexNew(NULL);     /* 必须在调度器启动之后调用 */
}

uint8_t MB_LastException(void) { return g_last_exc; }

const char *MB_StrError(mb_status_t st)
{
    switch (st)
    {
    case MB_OK:             return "OK";
    case MB_ERR_TIMEOUT:    return "TIMEOUT";
    case MB_ERR_CRC:        return "CRC";
    case MB_ERR_SLAVE_ADDR: return "ADDR_MISMATCH";
    case MB_ERR_EXCEPTION:  return "EXCEPTION";
    case MB_ERR_FRAME:      return "FRAME";
    case MB_ERR_PARAM:      return "PARAM";
    default:                return "UNKNOWN";
    }
}

/*==============================================================================
 * 2. 期望帧长推断 —— 决定"什么时候可以提前收工"
 *
 * 纯靠超时等响应太慢（每帧都要等满 200ms）。
 * Modbus RTU 的响应长度是可以从功能码推算出来的：
 *   03/04/01/02：[addr][fc][字节数][数据...][CRC低][CRC高] → 3 + 字节数 + 2
 *   05/06/0F/10：固定 8 字节回显
 *   异常响应：    [addr][fc|0x80][异常码][CRC] → 固定 5 字节
 *
 * 返回 0 = 目前还推断不出来（字节不够），调用者继续等。
 *============================================================================*/
static uint16_t mb_expect_len(const uint8_t *f, uint16_t len)
{
    if (len < 2) return 0;

    uint8_t fc = f[1];

    if (fc & 0x80) return 5;                    /* 异常帧 */

    switch (fc)
    {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        if (len < 3) return 0;                  /* 还没收到"字节数"这一位 */
        return (uint16_t)(3 + f[2] + 2);

    case MB_FC_WRITE_SINGLE_COIL:
    case MB_FC_WRITE_SINGLE_REG:
    case MB_FC_WRITE_MULTI_COILS:
    case MB_FC_WRITE_MULTI_REGS:
        return 8;

    default:
        return 0;                               /* 不认识的功能码：只能等超时 */
    }
}

/*==============================================================================
 * 3. 事务核心：发一帧 → 等响应 → 把响应留在 g_rx 里，返回长度（0 = 超时）
 *============================================================================*/
static uint16_t mb_transaction(uint16_t req_len, uint32_t timeout_ms)
{
    uint16_t flen = 0;
    uint32_t t0;

    /* RS485 是单总线，先抢锁 */
    if (g_bus_mutex != NULL) osMutexAcquire(g_bus_mutex, osWaitForever);

    /* ---------- ① 追加 CRC（低字节在前）---------- */
    uint16_t crc = MB_CRC16(g_tx, req_len);
    g_tx[req_len]     = (uint8_t)(crc & 0xFF);
    g_tx[req_len + 1] = (uint8_t)(crc >> 8);

    /* ---------- ② 等静默 → 清残留 → 发送 ----------
       ⚠️ 顺序不能反：必须先等总线静默，再清残留。
          如果先清再等，等待期间进来的杂散字节（别的设备抢答、从机迟到响应）
          会被当成这一次事务的响应，症状是"偶尔读到莫名其妙的值"。
       ⚠️ RxFlush 必须做：否则上一次事务的残留字节会被当成这次响应的开头，
          典型症状是"读到的值永远是上一次的值"。 */
    RS485_WaitSilent();
    RS485_RxFlush();
    RS485_Send(g_tx, (uint16_t)(req_len + 2));

    /* ---------- ③ 收响应 ---------- */
    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms)
    {
        uint16_t n = RS485_ReadFrame(g_rx + flen, (uint16_t)(MB_RTU_MAX_FRAME - flen));

        if (n > 0)
        {
            flen = (uint16_t)(flen + n);

            uint16_t exp = mb_expect_len(g_rx, flen);
            if (exp != 0 && flen >= exp) break;   /* 收够了，提前收工（省掉几十 ms） */
        }
        else
        {
            osDelay(1);                            /* 让出 CPU，别空转 */
        }
    }

    if (g_bus_mutex != NULL) osMutexRelease(g_bus_mutex);

    g_last_exc = 0;
    return flen;
}

/*==============================================================================
 * 4. 响应通用校验：CRC → 地址 → 异常 → 功能码
 *
 * ⚠️ 顺序有讲究：
 *    先 CRC（帧完整性）→ 再地址（是不是我要的那个从机）→ 再异常 → 最后功能码。
 *    异常必须先于功能码判定，否则 0x83 会被当成"功能码不匹配"，
 *    无法区分"从机报错"和"收到别的帧"。
 *============================================================================*/
static mb_status_t mb_check_response(uint16_t len, uint8_t slave, uint8_t fc)
{
    if (len < 4) return MB_ERR_FRAME;                 /* 最短：addr+fc+CRC = 4 */

    uint16_t recv_crc = (uint16_t)(g_rx[len - 1] << 8) | g_rx[len - 2];
    uint16_t calc_crc = MB_CRC16(g_rx, (uint16_t)(len - 2));
    if (recv_crc != calc_crc) return MB_ERR_CRC;

    if (g_rx[0] != slave) return MB_ERR_SLAVE_ADDR;

    if (g_rx[1] & 0x80)                               /* 异常响应 */
    {
        g_last_exc = (len >= 3) ? g_rx[2] : 0;
        return MB_ERR_EXCEPTION;
    }

    if (g_rx[1] != fc) return MB_ERR_FRAME;

    return MB_OK;
}

/*==============================================================================
 * 5. 01 / 02：读 bit（线圈 / 离散输入）
 *
 * bit 打包规则（⚠️ 最容易错的地方）：
 *     第 i 个 bit（i 从 0 开始）位于 resp[3 + i/8] 的第 (i % 8) 位
 *     即：第 1 个 DI 的状态在第 1 个数据字节的最低位（LSB first）
 *============================================================================*/
static mb_status_t mb_read_bits(uint8_t fc, uint8_t slave, uint16_t addr,
                                uint16_t cnt, uint8_t *out_bits, uint32_t timeout_ms)
{
    if (cnt == 0 || cnt > 2000 || out_bits == NULL) return MB_ERR_PARAM;

    g_tx[0] = slave;
    g_tx[1] = fc;
    g_tx[2] = (uint8_t)(addr >> 8);
    g_tx[3] = (uint8_t)(addr & 0xFF);
    g_tx[4] = (uint8_t)(cnt >> 8);
    g_tx[5] = (uint8_t)(cnt & 0xFF);

    uint16_t len = mb_transaction(6, timeout_ms);
    if (len == 0) return MB_ERR_TIMEOUT;

    mb_status_t st = mb_check_response(len, slave, fc);
    if (st != MB_OK) return st;

    uint8_t byte_cnt = g_rx[2];
    if (byte_cnt != (uint8_t)((cnt + 7) / 8))            return MB_ERR_FRAME;
    if (len < (uint16_t)(3 + byte_cnt + 2))              return MB_ERR_FRAME;

    for (uint16_t i = 0; i < cnt; i++)
        out_bits[i] = (uint8_t)((g_rx[3 + (i / 8)] >> (i % 8)) & 0x01);

    return MB_OK;
}

mb_status_t MB_ReadCoils(uint8_t slave, uint16_t addr, uint16_t cnt,
                         uint8_t *out_bits, uint32_t timeout_ms)
{
    return mb_read_bits(MB_FC_READ_COILS, slave, addr, cnt, out_bits, timeout_ms);
}

mb_status_t MB_ReadDiscreteInputs(uint8_t slave, uint16_t addr, uint16_t cnt,
                                  uint8_t *out_bits, uint32_t timeout_ms)
{
    return mb_read_bits(MB_FC_READ_DISCRETE_INPUTS, slave, addr, cnt, out_bits, timeout_ms);
}

/*==============================================================================
 * 6. 03 / 04：读 16 位寄存器（保持 / 输入）
 *
 * ⚠️ 寄存器值是【大端】：高字节在前。写反了会得到 65535 之类的怪值。
 *    例：25.3℃ 存成 253（0x00FD）→ 线上是 [0x00][0xFD] → 值 = (0x00<<8)|0xFD
 *============================================================================*/
static mb_status_t mb_read_regs(uint8_t fc, uint8_t slave, uint16_t reg_addr,
                                uint16_t cnt, uint16_t *out_regs, uint32_t timeout_ms)
{
    if (cnt == 0 || cnt > 125 || out_regs == NULL) return MB_ERR_PARAM;

    g_tx[0] = slave;
    g_tx[1] = fc;
    g_tx[2] = (uint8_t)(reg_addr >> 8);
    g_tx[3] = (uint8_t)(reg_addr & 0xFF);
    g_tx[4] = (uint8_t)(cnt >> 8);
    g_tx[5] = (uint8_t)(cnt & 0xFF);

    uint16_t len = mb_transaction(6, timeout_ms);
    if (len == 0) return MB_ERR_TIMEOUT;

    mb_status_t st = mb_check_response(len, slave, fc);
    if (st != MB_OK) return st;

    uint8_t byte_cnt = g_rx[2];
    if (byte_cnt != (uint8_t)(cnt * 2))            return MB_ERR_FRAME;
    if (len < (uint16_t)(3 + byte_cnt + 2))        return MB_ERR_FRAME;

    for (uint16_t i = 0; i < cnt; i++)
        out_regs[i] = (uint16_t)((g_rx[3 + i * 2] << 8) | g_rx[4 + i * 2]);

    return MB_OK;
}

mb_status_t MB_ReadHoldingRegs(uint8_t slave, uint16_t reg_addr, uint16_t cnt,
                               uint16_t *out_regs, uint32_t timeout_ms)
{
    return mb_read_regs(MB_FC_READ_HOLDING, slave, reg_addr, cnt, out_regs, timeout_ms);
}

mb_status_t MB_ReadInputRegs(uint8_t slave, uint16_t reg_addr, uint16_t cnt,
                             uint16_t *out_regs, uint32_t timeout_ms)
{
    return mb_read_regs(MB_FC_READ_INPUT, slave, reg_addr, cnt, out_regs, timeout_ms);
}

/*==============================================================================
 * 7. 05：写单个线圈
 *    规范：ON = FF00，OFF = 0000。发 0001 是非法的（很多从机会直接不响应）。
 *    正常响应 = 请求帧原样回显（8 字节）。
 *============================================================================*/
mb_status_t MB_WriteSingleCoil(uint8_t slave, uint16_t addr, uint8_t on,
                               uint32_t timeout_ms)
{
    g_tx[0] = slave;
    g_tx[1] = MB_FC_WRITE_SINGLE_COIL;
    g_tx[2] = (uint8_t)(addr >> 8);
    g_tx[3] = (uint8_t)(addr & 0xFF);
    g_tx[4] = on ? 0xFF : 0x00;
    g_tx[5] = 0x00;

    uint16_t len = mb_transaction(6, timeout_ms);
    if (len == 0) return MB_ERR_TIMEOUT;

    mb_status_t st = mb_check_response(len, slave, MB_FC_WRITE_SINGLE_COIL);
    if (st != MB_OK) return st;

    if (len < 8) return MB_ERR_FRAME;
    if (memcmp(g_rx, g_tx, 6) != 0) return MB_ERR_FRAME;   /* 回显对不上 */

    return MB_OK;
}

/*==============================================================================
 * 8. 06：写单个保持寄存器（响应同样是原样回显）
 *============================================================================*/
mb_status_t MB_WriteSingleReg(uint8_t slave, uint16_t addr, uint16_t val,
                              uint32_t timeout_ms)
{
    g_tx[0] = slave;
    g_tx[1] = MB_FC_WRITE_SINGLE_REG;
    g_tx[2] = (uint8_t)(addr >> 8);
    g_tx[3] = (uint8_t)(addr & 0xFF);
    g_tx[4] = (uint8_t)(val >> 8);
    g_tx[5] = (uint8_t)(val & 0xFF);

    uint16_t len = mb_transaction(6, timeout_ms);
    if (len == 0) return MB_ERR_TIMEOUT;

    mb_status_t st = mb_check_response(len, slave, MB_FC_WRITE_SINGLE_REG);
    if (st != MB_OK) return st;

    if (len < 8) return MB_ERR_FRAME;
    if (memcmp(g_rx, g_tx, 6) != 0) return MB_ERR_FRAME;
    return MB_OK;
}

/*==============================================================================
 * 9. 0F：写多个线圈 —— 一次事务写 8 路继电器
 *
 * 为什么不用 05 逐路写：05 每路一个来回（8 次事务 × ~40ms = 320ms），
 * 0F 一次搞定（~40ms）。总线时间是最宝贵的资源，这是能写进简历的优化点。
 *
 * 打包规则与读 bit 一致：第 i 个 bit 放在 data[i/8] 的 (i%8) 位。
 *============================================================================*/
mb_status_t MB_WriteMultiCoils(uint8_t slave, uint16_t addr, uint16_t cnt,
                               const uint8_t *bits, uint32_t timeout_ms)
{
    if (cnt == 0 || cnt > 1968 || bits == NULL) return MB_ERR_PARAM;

    uint8_t byte_cnt = (uint8_t)((cnt + 7) / 8);

    g_tx[0] = slave;
    g_tx[1] = MB_FC_WRITE_MULTI_COILS;
    g_tx[2] = (uint8_t)(addr >> 8);
    g_tx[3] = (uint8_t)(addr & 0xFF);
    g_tx[4] = (uint8_t)(cnt >> 8);
    g_tx[5] = (uint8_t)(cnt & 0xFF);
    g_tx[6] = byte_cnt;

    for (uint8_t b = 0; b < byte_cnt; b++) g_tx[7 + b] = 0;

    for (uint16_t i = 0; i < cnt; i++)
    {
        if (bits[i]) g_tx[7 + (i / 8)] |= (uint8_t)(1u << (i % 8));
    }

    uint16_t len = mb_transaction((uint16_t)(7 + byte_cnt), timeout_ms);
    if (len == 0) return MB_ERR_TIMEOUT;

    mb_status_t st = mb_check_response(len, slave, MB_FC_WRITE_MULTI_COILS);
    if (st != MB_OK) return st;

    if (len < 8) return MB_ERR_FRAME;
    return MB_OK;
}

/*==============================================================================
 * 10. 10 (0x10)：写多个保持寄存器
 *============================================================================*/
mb_status_t MB_WriteMultiRegs(uint8_t slave, uint16_t addr, uint16_t cnt,
                              const uint16_t *vals, uint32_t timeout_ms)
{
    if (cnt == 0 || cnt > 123 || vals == NULL) return MB_ERR_PARAM;

    g_tx[0] = slave;
    g_tx[1] = MB_FC_WRITE_MULTI_REGS;
    g_tx[2] = (uint8_t)(addr >> 8);
    g_tx[3] = (uint8_t)(addr & 0xFF);
    g_tx[4] = (uint8_t)(cnt >> 8);
    g_tx[5] = (uint8_t)(cnt & 0xFF);
    g_tx[6] = (uint8_t)(cnt * 2);

    for (uint16_t i = 0; i < cnt; i++)
    {
        g_tx[7 + i * 2]     = (uint8_t)(vals[i] >> 8);   /* 大端 */
        g_tx[8 + i * 2]     = (uint8_t)(vals[i] & 0xFF);
    }

    uint16_t len = mb_transaction((uint16_t)(7 + cnt * 2), timeout_ms);
    if (len == 0) return MB_ERR_TIMEOUT;

    mb_status_t st = mb_check_response(len, slave, MB_FC_WRITE_MULTI_REGS);
    if (st != MB_OK) return st;

    if (len < 8) return MB_ERR_FRAME;
    return MB_OK;
}

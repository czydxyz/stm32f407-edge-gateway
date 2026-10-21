/*==============================================================================
 * M4 · 应用层协议（跑在 TCP 之上）
 *
 * 为什么 TCP 还要自己分帧：
 *   TCP 是【字节流】，没有消息边界。
 *     - 粘包：发两次 "HB,0" / "HB,1"，对端可能一次收到 "HB,0HB,1"
 *     - 拆包：一次发 100 字节，可能分成 30 + 70 两次到达
 *   所以必须在应用层定义帧格式，自己恢复边界。
 *
 * 帧格式（长度字段的位置必须固定，这是能解析的前提）：
 *
 *   +--------+--------+--------+-------------+----------+
 *   | 0xAA   | 0x55   |  LEN   |  CMD        |  DATA    |  CRC32   |
 *   | 1B     | 1B     |  1B    |  1B         |  N B     |  4B(大端)|
 *   +--------+--------+--------+-------------+----------+
 *
 *   LEN = 1(CMD) + N(DATA)   ← 只算 CMD 和 DATA，不含帧头和 CRC
 *   帧总长 = 3 + LEN + 4 = LEN + 7
 *
 *   CRC32 覆盖范围 = CMD + DATA（标准 CRC32，与 Python zlib.crc32 一致）
 *============================================================================*/

#ifndef __M4_PROTO_H
#define __M4_PROTO_H

#include <stdint.h>

/* 帧头 */
#define M4_SOF0            0xAAu
#define M4_SOF1            0x55u

/* ⭐ 254 —— 这个值【不是随便取的，是被协议的 LEN 字段宽度逼出来的】（BUG-066）
   LEN 只有 1 个字节，且 LEN = 1(CMD) + N(DATA)，所以 N ≤ 255 - 1 = 254。
   之前写成 1088（按"1024 分块 + 10 头 + 54 余量"算的），
   完全没考虑 LEN 只有 1 字节 —— 于是上位机按 1024 组帧时直接抛
   ValueError: byte must be in range(0, 256)，OTA_DATA 一帧都发不出去。

   ⚠️ 由此推出的【所有上层分块上限】：
      OTA_DATA 的 DATA = offset(4)+seq(2)+block_crc(4)+payload(N) = 10 + N
      → 1 + 10 + N ≤ 255  →  payload ≤ 244  （工程取 240，见 OTA_BLOCK_SIZE）

   ⚠️ 若将来要支持更大的块，必须先【把 LEN 扩成 2 字节】，
      并同步改 MAX_DATA / OTA_BLOCK_SIZE / 上位机 pack() —— 三处一起改，别只改一处。 */
#define M4_PROTO_MAX_DATA  254u
#define M4_PROTO_MAX_FRAME (3u + 1u + M4_PROTO_MAX_DATA + 4u)

/* 常规（非 OTA）发送帧缓冲大小：DATA 最大 64 字节就够（快照 39 / ACK 9 / NAK 1）
   ⭐ 栈上声明发送缓冲一律用这个，不用 M4_PROTO_MAX_FRAME */
#define M4_TX_MAX_FRAME    (3u + 1u + 64u + 4u)

/* 解析超时：超过这么久没收新字节就复位状态机（防"只发半帧"把状态机卡死） */
#define M4_PROTO_TIMEOUT_MS 200u

/*==============================================================================
 * 命令字
 *============================================================================*/
#define M4_CMD_REPORT       0x01u   /* 板 → PC：周期上报（DATA = 快照） */
#define M4_CMD_SET_RELAY    0x02u   /* PC → 板：设置继电器（DATA[0] = 位图） */
#define M4_CMD_QUERY        0x03u   /* PC → 板：查询数据池 */
#define M4_CMD_SNAPSHOT     0x81u   /* 板 → PC：数据池快照 */
#define M4_CMD_ACK          0x82u   /* 板 → PC：确认 */
#define M4_CMD_NAK          0xFFu   /* 板 → PC：拒绝（DATA[0] = 错误码） */

/*==============================================================================
 * M5 · OTA 命令（⭐ 复用 M4 的帧格式，不另起协议）
 *
 * 复用带来的直接收益：粘包/拆包、CRC32、每连接独立状态机【全部现成】，
 * OTA 只要加 4 个 CMD 分支 —— 这就是"协议设计时留扩展位"的实际价值。
 *============================================================================*/
#define M4_CMD_OTA_BEGIN    0x20u   /* PC→板：magic(4)+total_len(4)+total_crc(4)+block_size(2) */
#define M4_CMD_OTA_DATA     0x21u   /* PC→板：offset(4)+seq(2)+block_crc(4)+data(N) */
#define M4_CMD_OTA_END      0x22u   /* PC→板：（空）结束并做整体校验 */
#define M4_CMD_OTA_ACK      0xA0u   /* 板→PC：status(1)+offset(4)+extra(4) */

/* OTA 应答 status */
#define M4_OTA_OK           0x00u
#define M4_OTA_ERR_CRC      0x01u   /* 分块 CRC 错 / 整体 CRC 错 */
#define M4_OTA_ERR_RANGE    0x02u   /* 越界（长度超 Bank、offset 不等于 received） */
#define M4_OTA_ERR_FLASH    0x03u   /* Flash 擦写失败 */
#define M4_OTA_ERR_STATE    0x04u   /* 状态不对（没 BEGIN 就发 DATA） */
#define M4_OTA_ERR_PARAM    0x05u   /* 参数非法（magic 不对、block_size 非法） */

/* NAK 错误码 */
#define M4_ERR_BADCRC       0x01u
#define M4_ERR_BADLEN       0x02u
#define M4_ERR_UNSUPPORTED  0x03u
#define M4_ERR_BADPARAM     0x04u

/*==============================================================================
 * 解析上下文
 *
 * ⚠️ 必须【每条连接一个 ctx】。用 static 全局变量会导致两条 TCP 连接的数据
 *    互相污染状态机，解出乱码帧 —— 这是多连接时最容易踩的坑。
 *============================================================================*/
typedef struct {
    /* 解析状态（外部只读，别改） */
    uint8_t   state;
    uint8_t   len;                      /* 收到的 LEN */
    uint8_t   cmd;                      /* 收到的 CMD */
    uint8_t   data[M4_PROTO_MAX_DATA];  /* 收到的 DATA */
    uint8_t   data_len;                 /* DATA 实际长度 = len - 1 */
    uint32_t  last_ms;                  /* 上次收到字节的时刻（超时复位用） */

    /* 内部计数 */
    uint8_t   idx;                      /* DATA 收了多少 */
    uint8_t   crc_idx;                  /* CRC 收了多少字节 */
    uint32_t  crc_acc;                  /* 收到的 CRC */
    uint32_t  crc_calc;                 /* 算出来的 CRC */
} m4_proto_ctx_t;

/*==============================================================================
 * API
 *============================================================================*/

/* 标准 CRC32（初值 0xFFFFFFFF，多项式 0xEDB88320，结果异或 0xFFFFFFFF） */
uint32_t M4_Crc32(const uint8_t *data, uint32_t len);

/* ⭐ 增量 CRC32（OTA 边收边算用）。
   state 初值必须是 0xFFFFFFFF，返回值【未取反】，最终值要 ^ 0xFFFFFFFF。 */
uint32_t M4_Crc32Update(uint32_t state, const uint8_t *data, uint32_t len);

/* OTA 应答帧打包：status(1) + offset(4) + extra(4)，返回帧长 */
int M4_ProtoPackOtaAck(uint8_t status, uint32_t offset, uint32_t extra,
                       uint8_t *out, uint32_t out_max);

/* 复位解析上下文（新建连接时必须调一次） */
void M4_ProtoInit(m4_proto_ctx_t *ctx);

/**
 * @brief 喂一个字节进状态机
 * @return  0  = 还在收（没凑齐）
 *          >0 = 收到一帧完整且 CRC 正确的帧，返回值是 CMD
 *               （数据从 ctx->data / ctx->data_len 取）
 *          -1 = CRC 校验失败（已自动复位）
 *          -2 = 长度非法（LEN 为 0 或超过缓冲，已自动复位）
 */
int M4_ProtoFeed(m4_proto_ctx_t *ctx, uint8_t b);

/**
 * @brief 打包一帧
 * @return >0 = 帧长度；-1 = 参数非法/缓冲不够
 */
int M4_ProtoPack(uint8_t cmd, const uint8_t *data, uint8_t n,
                 uint8_t *out, uint32_t out_max);

/*---- 业务层便捷打包 ----*/

/* 打包"数据池快照"帧（CMD = 0x81）。会把 g_pool 的快照序列化进去。
   ⚠️ 内部会加锁取快照，取完立刻解锁，不会持锁慢慢地打包。 */
int M4_ProtoPackSnapshot(uint8_t *out, uint32_t out_max);

/* ⭐ M6 补传用：按【指定的历史值 + 指定的 seq】组一帧快照。
 *
 * @param seq            帧里填的序号（补传必须传缓存条目的原 seq，见 .c 说明）
 * @param hist_temp      历史温度；传 INT16_MIN = "用当前数据池的值"（正常上报路径）
 * @param hist_humi      历史湿度，同上
 * @param hist_di        历史 DI 位图
 * @param hist_relay     历史继电器状态
 * @param hist_uptime_s  该条数据的采集时刻（运行秒数）
 * @param age_s_override 该条数据已经有多老（秒）；正常上报时忽略
 *
 * ⚠️ 为什么单独开这个函数而不是给 PackSnapshot 加参数：
 *    补传是少数路径，让它显式传一堆历史值；正常上报继续用原来那个
 *    零参数的 API，调用方完全不用关心补传的存在。 */
int M4_ProtoPackSnapshotEx(uint8_t *out, uint32_t out_max,
                           uint32_t seq,
                           int16_t  hist_temp,
                           int16_t  hist_humi,
                           uint8_t  hist_di,
                           uint8_t  hist_relay,
                           uint32_t hist_uptime_s,
                           uint32_t age_s_override);

/* 打包一个 ACK / NAK（DATA 可空） */
int M4_ProtoPackAck(uint8_t *out, uint32_t out_max);
int M4_ProtoPackNak(uint8_t err, uint8_t *out, uint32_t out_max);

/* 解析 SET_RELAY 的参数 */
int M4_ProtoParseSetRelay(const m4_proto_ctx_t *ctx, uint8_t *bitmap);

#endif /* __M4_PROTO_H */

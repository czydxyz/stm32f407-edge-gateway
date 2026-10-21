/*==============================================================================
 * Flash 布局 —— ⭐ 全工程的唯一来源（single source of truth）
 *
 * ⚠️ 为什么必须有这一个文件：
 *    Bootloader 和 App 是【两个独立的 Keil 工程】。
 *    如果两边各写一份地址常量，改了一处忘了另一处，后果是：
 *        · 跳转地址错 → 一跳就 HardFault
 *        · 擦除扇区错 → 擦掉 Bootloader 自己 / 擦掉正在运行的固件 → 变砖
 *    所以两边都 include 这一份，地址只在这里定义一次。
 *
 * ⚠️ F407 的扇区大小【不均匀】，这是所有 Flash 分区 bug 的根源：
 *      Sector 0~3 = 16 KB，Sector 4 = 64 KB，Sector 5~11 = 128 KB
 *    擦除的最小单位是扇区，所以【分区边界必须落在扇区边界上】，
 *    否则擦 Bank B 时会把 Bank A 的尾巴一起擦掉。
 *
 * 本工程布局（STM32F407ZGT6，1 MB Flash）：
 *
 *   0x08000000 ┌─────────────────┐
 *              │  Bootloader     │  S0+S1   32 KB   ← 永不自更新
 *   0x08008000 ├─────────────────┤
 *              │  参数/黑匣子     │  S2+S3   32 KB   ← 复位计数/死因/OTA 状态
 *   0x08010000 ├─────────────────┤
 *              │  Bank A (App)   │  S4~S7  448 KB   ← 当前运行的固件
 *   0x08080000 ├─────────────────┤
 *              │  Bank B (OTA)   │  S8~S11 512 KB   ← 新固件暂存
 *   0x08100000 └─────────────────┘
 *
 * 边界验算（这一步别省，算错就是砖）：
 *   S4 = 0x08010000 (64K)  → 0x08020000
 *   S5 = 0x08020000 (128K) → 0x08040000
 *   S6 = 0x08040000 (128K) → 0x08060000
 *   S7 = 0x08060000 (128K) → 0x08080000  ← Bank A 结束，正好是 Bank B 起始 ✅
 *   S8~S11 = 0x08080000 → 0x08100000     ← Bank B = 512 KB ✅
 *============================================================================*/

#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include <stdint.h>

/* ⚠️ 扇区号这里直接用数字，不依赖 HAL 的 FLASH_SECTOR_x 宏：
   这样 flash_layout.h 可以在任何包含顺序下被 include（包括 Bootloader 里
   在 stm32f4xx_hal.h 之前），不会因为宏未定义而静默出错。
   对照：S0=0 S1=1 S2=2 S3=3 S4=4 ... S11=11（HAL 的宏值就是这些数字） */
#define M5_SECTOR_0          0u
#define M5_SECTOR_1          1u
#define M5_SECTOR_2          2u
#define M5_SECTOR_3          3u
#define M5_SECTOR_4          4u
#define M5_SECTOR_7          7u
#define M5_SECTOR_8          8u
#define M5_SECTOR_11         11u

#define FLASH_BASE_ADDR      0x08000000u
#define FLASH_TOTAL_SIZE     (1024u * 1024u)

/*==============================================================================
 * Bootloader
 *============================================================================*/
#define BL_ADDR              0x08000000u
#define BL_SIZE              (32u * 1024u)
#define BL_SECTOR_FIRST      M5_SECTOR_0
#define BL_SECTOR_LAST       M5_SECTOR_1

/*==============================================================================
 * 参数区（黑匣子 + OTA 状态 + 确认字）
 *
 * ⚠️ 为什么参数区必须在 Bank A 之前：
 *    App 代码从 0x08010000 开始，如果参数区放在 App 后面（比如 0x08080000），
 *    那 Bank A 一旦变大就会覆盖参数区。放在前面则永远不会被 App 代码碰到。
 *============================================================================*/
/* ⚠️ S2 与 S3 必须【分开用】，不能共用一个扇区放两类数据：
    内部 Flash 擦除的最小单位是扇区，改 1 个字节也要擦整个 16 KB。
    如果"死因黑匣子"和"OTA 状态 + 确认字"放在同一扇区，
    那么死一次（写黑匣子）就会把 OTA 状态和确认字一起擦掉 —— 直接导致回滚误判。
    所以：S2 = 参数页（OTA 状态 + 确认字），S3 = 死因黑匣子，互不干扰。 */
#define PARAM_ADDR           0x08008000u    /* S2：参数页（OTA 状态 + 确认字） */
#define PARAM_SIZE           (16u * 1024u)
#define PARAM_SECTOR         M5_SECTOR_2

#define BB_ADDR              0x0800C000u    /* S3：死因黑匣子（独立扇区） */
#define BB_SIZE              (16u * 1024u)
#define BB_SECTOR            M5_SECTOR_3

/* 参数区内部偏移（都在 S2 内，改一个字段擦一次 S2 再整页写回） */
#define PARAM_OFF_MAGIC      0u      /* 4B：参数页有效标志 */
#define PARAM_OFF_VER        4u      /* 4B：布局版本（改结构时 +1，旧数据自动作废） */
#define PARAM_OFF_CONF       16u     /* app_conf_t：确认字 / reset_count */
#define PARAM_OFF_OTA        64u     /* ota_info_t：OTA 状态 */

#define PARAM_MAGIC          0xA5A5A5A5u

/*==============================================================================
 * 双 Bank
 *============================================================================*/
#define APP_A_ADDR           0x08010000u
#define APP_A_SIZE           (448u * 1024u)
#define APP_A_SECTOR_FIRST   M5_SECTOR_4
#define APP_A_SECTOR_LAST    M5_SECTOR_7

#define APP_B_ADDR           0x08080000u
#define APP_B_SIZE           (512u * 1024u)
#define APP_B_SECTOR_FIRST   M5_SECTOR_8
#define APP_B_SECTOR_LAST    M5_SECTOR_11

/*==============================================================================
 * OTA 状态
 *============================================================================*/
#define OTA_MAGIC            0x4F544155u   /* "OTAU" */

typedef enum {
    OTA_IDLE        = 0,   /* 无 OTA */
    OTA_DOWNLOADING = 1,   /* 下载中（含已收字节数） */
    OTA_READY       = 2,   /* 已收完且整体 CRC 通过，等待 Bootloader 处理 */
    OTA_COMMITTED   = 3    /* 已切换并收到 App 确认 */
} ota_state_t;

/* OTA 状态结构体（存在 PARAM_OFF_OTA_STATE，32 字节对齐写） */
typedef struct {
    uint32_t magic;        /* OTA_MAGIC */
    uint32_t state;        /* ota_state_t */
    uint32_t total_len;    /* 固件总长度 */
    uint32_t total_crc;    /* 整体 CRC32 */
    uint32_t received;     /* 已成功写入字节数 = 断点续传起点 */
    uint32_t block_size;   /* 分块大小（默认 1024） */
    uint32_t bank;         /* 接收区：0=A 1=B（当前固定 B） */
    uint32_t reserved;
} ota_state_t_def;
typedef ota_state_t_def ota_info_t;

/*==============================================================================
 * App 确认 / 复位计数（存在 PARAM_OFF_APP_CONF）
 *============================================================================*/
typedef struct {
    uint32_t magic;          /* PARAM_MAGIC */
    uint32_t reset_count;    /* 连续复位次数（Bootloader 每次上电 ++） */
    uint32_t app_confirmed;  /* 1 = App 已跑满 30s 且自检通过 */
    uint32_t rollback_count; /* 累计回滚次数（只增，用于诊断） */
} app_conf_t;

/*==============================================================================
 * 参数页整体结构（S2 里就放这些数据）
 *
 * ⚠️ 为什么这个结构定义在【这里】而不是 m5_blackbox.h：
 *    Bootloader 不链接 m5_blackbox.c（它不依赖 HAL、不依赖 RTOS），
 *    但它必须能读参数页。把结构定义放在布局头里，两边才能真正共用同一份布局。
 *============================================================================*/
typedef struct {
    uint32_t   magic;          /* PARAM_MAGIC：判断页是否有效 */
    uint32_t   version;        /* 布局版本，改结构时 +1，旧数据自动作废 */
    app_conf_t conf;           /* 确认字 / reset_count / rollback_count */
    ota_info_t ota;            /* OTA 状态 */
} param_page_t;

/*==============================================================================
 * 共用常量
 *============================================================================*/
/* ⚠️ 240 不是拍脑袋定的，是被协议 LEN 字段宽度卡死的（BUG-066）：
     帧 = AA 55 LEN(1) CMD(1) DATA(N) CRC32(4)，LEN 单字节 → N ≤ 254
     OTA_DATA 的 DATA = offset(4)+seq(2)+block_crc(4)+payload(N) = 10 + N
     → 1 + 10 + N ≤ 255  →  N ≤ 244，取 240 留 4 字节余量。
   ⭐ 更好的写法是让它从 M4_PROTO_MAX_DATA 推导，但 flash_layout.h 是
      Bootloader 和 App 共用的、不依赖 m4_proto.h，所以这里只能写死 ——
      代价就是【改协议时必须两处一起改】，已在本文件与 m4_proto.h 双向注明。 */
#define OTA_BLOCK_SIZE       240u       /* ⚠️ 上限 244，改大前先看 m4_proto.h 的说明 */
#define APP_CONFIRM_SEC      30u        /* App 跑满多少秒算"确认成功" */
#define RESET_COUNT_ROLLBACK 3u         /* 连续复位几次判定新固件不可用 → 回滚 */

#endif /* FLASH_LAYOUT_H */

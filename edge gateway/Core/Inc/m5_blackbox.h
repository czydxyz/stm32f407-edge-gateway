/*==============================================================================
 * M5 · 内部 Flash 参数区驱动（参数页 + 死因黑匣子）
 *
 * ⚠️ 这个文件管两块东西，它们都在内部 Flash，但【必须分处两个扇区】：
 *
 *   S2 (0x08008000) = 参数页：OTA 状态 + App 确认字 + 连续复位计数
 *   S3 (0x0800C000) = 死因黑匣子：复位前最后一次"谁没报到"的现场快照
 *
 *   为什么不能放一起：Flash 擦除最小单位是扇区（16 KB），
 *   改 1 字节要擦整个扇区。若两者同扇区，死一次（写黑匣子）就会把
 *   OTA 状态和确认字一起抹掉 → Bootloader 认为"固件没确认" → 误回滚。
 *
 * ⚠️ 为什么用内部 Flash 而不是 W25Q128 存死因：
 *   死因要的是"极简且必然成功"。SPI Flash 走毫秒级传输且可能失败，
 *   而内部 Flash 只是解锁 + 一次字编程。人都要死了，别指望复杂外设。
 *
 * ⚠️ 擦除期间 CPU 会被 stall（16 KB 扇区数百毫秒），所有任务停摆
 *   → 擦写前必须 M5_WdgHold()，否则看门狗会在擦除过程中咬人。
 *============================================================================*/

#ifndef __M5_BLACKBOX_H
#define __M5_BLACKBOX_H

#include <stdint.h>
#include "shared/flash_layout.h"

/* ⚠️ param_page_t 定义在 shared/flash_layout.h —— Bootloader 也要用同一份，
   不要在这里重复定义（重复定义一旦不同步就是静默错位）。 */

/*==============================================================================
 * 死因黑匣子（S3）
 *============================================================================*/
typedef struct {
    uint32_t magic;            /* PARAM_MAGIC */
    uint32_t miss_mask;        /* ⭐ 缺了哪些存活位 → 直接定位是哪个任务 */
    uint32_t alive_snapshot;   /* 当时的完整存活位图 */
    uint32_t heap_free;        /* 是不是堆耗尽导致任务没起来 */
    uint32_t uptime_s;         /* 跑了多久才挂 → 区分"启动失败"和"跑几小时挂" */
    uint32_t stack_min[8];     /* 各任务栈水线（word 数）→ 是不是栈溢出 */
    uint32_t reset_count;      /* 当时的连续复位次数 */
} m5_blackbox_t;

/*==============================================================================
 * API
 *============================================================================*/

/* ---- 参数页 ---- */

/* 读参数页。magic/version 不合法时用默认值填好返回（不会返回失败）。 */
void M5_ParamLoad(param_page_t *page);

/* 写回【确认字部分】。内部：读整页 → 改 conf → 擦 S2 → 整页写回。
   ⚠️ 会擦一次 S2，别在高频路径调用。 */
int  M5_ParamSaveConf(const app_conf_t *conf);

/* 写回【OTA 状态部分】。同上，会擦一次 S2。 */
int  M5_ParamSaveOta(const ota_info_t *ota);

/* 便捷：读/改/写 reset_count（Bootloader 上电 ++，App 确认后清零） */
int  M5_ParamBumpResetCount(uint32_t *out_count);
int  M5_ParamClearResetCount(void);
int  M5_ParamSetConfirmed(uint32_t confirmed);

/* ---- 死因黑匣子 ---- */

/* 写死因。⚠️ 只在"确认要死了"的那一刻调用一次 —— 擦写有寿命也有耗时。 */
int  M5_BlackboxWrite(uint32_t miss_mask, uint32_t alive_snapshot);

/* 读黑匣子（magic 不合法返回 0） */
int  M5_BlackboxRead(m5_blackbox_t *bb);

/* 打印黑匣子内容（复位后第一行调用，排障的钥匙） */
void M5_BlackboxPrint(void);

/* 清掉黑匣子（App 确认稳定运行后调用，避免旧死因误导下次排障） */
int  M5_BlackboxClear(void);

#endif /* __M5_BLACKBOX_H */

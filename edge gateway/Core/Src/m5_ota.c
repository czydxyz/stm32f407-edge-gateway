/*==============================================================================
 * M5 · OTA 下载实现
 *============================================================================*/

#include "m5_ota.h"
#include "m4_proto.h"
#include "m5_blackbox.h"
#include "m5_wdg.h"
#include "shared/flash_layout.h"
#include "stm32f4xx_hal.h"

#include <stdio.h>
#include <string.h>

/* DATA 字段头长度：offset(4) + seq(2) + block_crc(4) */
#define OTA_DATA_HDR       10u

/* 每写多少字节把进度记一次参数区（折中：太频繁擦写慢且磨损，不写断电就白传） */
#define OTA_CHECKPOINT     8192u

/*==============================================================================
 * 内部状态
 *============================================================================*/
static ota_info_t s_ota;        /* 当前 OTA 会话 */
static uint32_t   s_run_crc;    /* 累计 CRC（未取反的中间状态） */
static uint8_t    s_active;     /* 1 = 正在下载 */
static uint32_t   s_last_cp;    /* 上次写参数区时的 received */

/*==============================================================================
 * 小工具
 *============================================================================*/
static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* 统一应答：status + offset + extra */
static int ota_ack(uint8_t status, uint32_t offset, uint32_t extra,
                   uint8_t *ack, uint32_t ack_max)
{
    return M4_ProtoPackOtaAck(status, offset, extra, ack, ack_max);
}

/*==============================================================================
 * 擦 Bank B（S8~S11，4 个 128 KB 扇区）
 *
 * ⚠️ 为什么要一个扇区一个扇区地擦，且扇区间喂狗：
 *    128 KB 扇区擦除耗时在【数百毫秒到数秒】量级，期间 CPU 被 stall，
 *    所有任务停摆。4 个扇区一口气擦很可能超过 IWDG 超时（最快 2.72 s）→ 擦到一半复位。
 *    所以：每个扇区擦之前申请 hold，擦完立刻裁决喂一次狗。
 *============================================================================*/
static int ota_erase_bank_b(void)
{
    uint32_t sec;

    for (sec = (uint32_t)APP_B_SECTOR_FIRST; sec <= (uint32_t)APP_B_SECTOR_LAST; sec++)
    {
        FLASH_EraseInitTypeDef e;
        uint32_t               serr = 0u;
        uint32_t               t0, dt;

        /* ⭐ hold 必须【小于 IWDG 最快超时 2.72 s】，否则失去意义 */
        M5_WdgHold(2000u);

        HAL_FLASH_Unlock();

        e.TypeErase    = FLASH_TYPEERASE_SECTORS;
        e.Sector       = sec;
        e.NbSectors    = 1u;
        e.VoltageRange = FLASH_VOLTAGE_RANGE_3;   /* ⚠️ 3.3V 必须 _3 */

        t0 = HAL_GetTick();
        if (HAL_FLASHEx_Erase(&e, &serr) != HAL_OK)
        {
            HAL_FLASH_Lock();
            printf("[ota] erase sector %lu FAILED (err=%lu)\r\n",
                   (unsigned long)sec, (unsigned long)serr);
            return -1;
        }
        HAL_FLASH_Lock();

        dt = HAL_GetTick() - t0;
        printf("[ota] erased sector %lu in %lums\r\n", (unsigned long)sec,
               (unsigned long)dt);

        /* ⭐ 扇区之间喂一次狗：hold 生效，所以这里一定会喂出去 */
        M5_WdgKick();
    }
    return 0;
}

/*==============================================================================
 * 初始化
 *============================================================================*/
void M5_OtaInit(void)
{
    param_page_t page;

    memset(&s_ota, 0, sizeof(s_ota));
    s_run_crc = 0xFFFFFFFFu;
    s_active  = 0u;
    s_last_cp = 0u;

    M5_ParamLoad(&page);

    /* ⭐ 上电时如果参数区里还留着 DOWNLOADING 状态，说明上次传输中断了
       （断电/断网）。这里【不自动继续】，只把状态暴露出来：
       上位机重发 OTA_BEGIN 且 crc 一致时会自动续传。 */
    if ((page.ota.magic == OTA_MAGIC) &&
        (page.ota.state == (uint32_t)OTA_DOWNLOADING))
    {
        printf("[ota] found interrupted download: %lu/%lu bytes\r\n",
               (unsigned long)page.ota.received,
               (unsigned long)page.ota.total_len);
    }
}

int M5_OtaBusy(void)
{
    return s_active ? 1 : 0;
}

int M5_OtaProgress(void)
{
    if (!s_active || (s_ota.total_len == 0u)) return -1;
    return (int)((s_ota.received * 100u) / s_ota.total_len);
}

/*==============================================================================
 * OTA_BEGIN
 *============================================================================*/
int M5_OtaBegin(const uint8_t *d, uint8_t n, uint8_t *ack, uint32_t ack_max)
{
    uint32_t     magic, total_len, total_crc;
    uint16_t     bs;
    param_page_t page;

    if ((d == NULL) || (n < 14u)) return ota_ack(M4_OTA_ERR_PARAM, 0u, 0u, ack, ack_max);

    magic     = get_be32(&d[0]);
    total_len = get_be32(&d[4]);
    total_crc = get_be32(&d[8]);
    bs        = get_be16(&d[12]);

    if (magic != OTA_MAGIC)
    {
        printf("[ota] BEGIN: bad magic 0x%08lX\r\n", (unsigned long)magic);
        return ota_ack(M4_OTA_ERR_PARAM, 0u, 0u, ack, ack_max);
    }

    /* ⚠️ 边界检查必须在擦除之前：越界就直接拒绝，不能先把 Bank B 擦了再说 */
    if ((total_len == 0u) || (total_len > (uint32_t)APP_B_SIZE))
    {
        printf("[ota] BEGIN: len %lu out of range (max %lu)\r\n",
               (unsigned long)total_len, (unsigned long)APP_B_SIZE);
        return ota_ack(M4_OTA_ERR_RANGE, 0u, (uint32_t)APP_B_SIZE, ack, ack_max);
    }

    if ((bs == 0u) || (bs > 1024u)) bs = OTA_BLOCK_SIZE;

    M5_ParamLoad(&page);

    /* ---------- ⭐ 断点续传判定 ----------
       同一个固件（crc 一致）+ 上次没传完 + 这次又是从头开始请求
       → 跳过擦除，直接从 received 继续。 */
    if ((page.ota.magic == OTA_MAGIC) &&
        (page.ota.state == (uint32_t)OTA_DOWNLOADING) &&
        (page.ota.total_crc == total_crc) &&
        (page.ota.received > 0u) &&
        (page.ota.received < total_len))
    {
        printf("[ota] resume at %lu/%lu (skip erase)\r\n",
               (unsigned long)page.ota.received, (unsigned long)total_len);

        s_ota            = page.ota;
        s_ota.block_size = bs;
        s_active         = 1u;
        s_last_cp        = s_ota.received;

        /* ⭐ 累计 CRC 断电就丢了，必须从已写入的 Flash 内容【重算】回来。
           读内部 Flash 很快（100 KB 只要几毫秒），比重传一遍划算得多。 */
        s_run_crc = M4_Crc32Update(0xFFFFFFFFu,
                                   (const uint8_t *)APP_B_ADDR,
                                   s_ota.received);

        return ota_ack(M4_OTA_OK, s_ota.received, total_len, ack, ack_max);
    }

    /* ---------- 全新下载：擦 Bank B ---------- */
    printf("[ota] BEGIN len=%lu crc=0x%08lX bs=%u, erasing bank B...\r\n",
           (unsigned long)total_len, (unsigned long)total_crc, (unsigned)bs);

    if (ota_erase_bank_b() != 0)
    {
        return ota_ack(M4_OTA_ERR_FLASH, 0u, 0u, ack, ack_max);
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.magic      = OTA_MAGIC;
    s_ota.state      = (uint32_t)OTA_DOWNLOADING;
    s_ota.total_len  = total_len;
    s_ota.total_crc  = total_crc;
    s_ota.received   = 0u;
    s_ota.block_size = bs;
    s_ota.bank       = 1u;         /* 固定收在 Bank B */
    s_run_crc        = 0xFFFFFFFFu;
    s_active         = 1u;
    s_last_cp        = 0u;

    /* 记一次参数区：这样中途断电也知道"正在传且已收 0 字节" */
    M5_ParamSaveOta(&s_ota);

    return ota_ack(M4_OTA_OK, 0u, total_len, ack, ack_max);
}

/*==============================================================================
 * OTA_DATA
 *============================================================================*/
int M5_OtaData(const uint8_t *d, uint8_t n, uint8_t *ack, uint32_t ack_max)
{
    uint32_t offset, bcrc, addr;
    uint16_t seq;
    const uint8_t *payload;
    uint16_t       plen;
    uint32_t       i;

    if (!s_active) return ota_ack(M4_OTA_ERR_STATE, 0u, 0u, ack, ack_max);
    if ((d == NULL) || (n <= OTA_DATA_HDR))
    {
        return ota_ack(M4_OTA_ERR_PARAM, s_ota.received, 0u, ack, ack_max);
    }

    offset  = get_be32(&d[0]);
    seq     = get_be16(&d[4]);
    bcrc    = get_be32(&d[6]);
    payload = &d[OTA_DATA_HDR];
    plen    = (uint16_t)(n - OTA_DATA_HDR);
    (void)seq;

    /* ---------- ① 严格顺序检查 ---------- */
    if (offset != s_ota.received)
    {
        /* extra 带回"我期望的 offset"，上位机据此回退/跳到正确位置 */
        return ota_ack(M4_OTA_ERR_RANGE, s_ota.received, s_ota.received, ack, ack_max);
    }

    if ((s_ota.received + plen) > s_ota.total_len)
    {
        return ota_ack(M4_OTA_ERR_RANGE, s_ota.received, s_ota.total_len, ack, ack_max);
    }

    /* ---------- ② 本块 CRC ---------- */
    if (M4_Crc32(payload, plen) != bcrc)
    {
        printf("[ota] block crc err @%lu\r\n", (unsigned long)offset);
        return ota_ack(M4_OTA_ERR_CRC, s_ota.received, 0u, ack, ack_max);
    }

    /* ---------- ③ 写 Flash ---------- */
    addr = (uint32_t)APP_B_ADDR + offset;

    HAL_FLASH_Unlock();
    for (i = 0u; i + 4u <= plen; i += 4u)
    {
        uint32_t v;
        memcpy(&v, &payload[i], 4u);      /* ⚠️ 用 memcpy 避免非对齐访问 HardFault */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, v) != HAL_OK)
        {
            HAL_FLASH_Lock();
            printf("[ota] flash write fail @%lu\r\n", (unsigned long)(addr + i));
            return ota_ack(M4_OTA_ERR_FLASH, s_ota.received, 0u, ack, ack_max);
        }
    }
    /* 最后不足 4 字节的尾巴：补 0xFF 凑一个 word（Flash 擦后本来就是 0xFF） */
    if (i < plen)
    {
        uint32_t v = 0xFFFFFFFFu;
        uint16_t k;
        for (k = 0u; (i + k) < plen; k++)
        {
            ((uint8_t *)&v)[k] = payload[i + k];
        }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, v) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return ota_ack(M4_OTA_ERR_FLASH, s_ota.received, 0u, ack, ack_max);
        }
    }
    HAL_FLASH_Lock();

    /* ---------- ④ 推进进度 ---------- */
    s_ota.received += plen;
    s_run_crc       = M4_Crc32Update(s_run_crc, payload, plen);

    /* ---------- ⑤ 定期记进度（每 8 KB） ----------
       ⚠️ 不能每块都写：擦一次扇区数百毫秒，100 KB / 1024 = 100 块，
          每块都写会让下载慢到没法用，还会把 Flash 磨坏。
       ⚠️ 也不能完全不写：断电后不知道传到哪，只能从头再来。 */
    if ((s_ota.received - s_last_cp) >= OTA_CHECKPOINT)
    {
        s_last_cp = s_ota.received;
        M5_ParamSaveOta(&s_ota);
    }

    return ota_ack(M4_OTA_OK, s_ota.received, 0u, ack, ack_max);
}

/*==============================================================================
 * OTA_END
 *============================================================================*/
int M5_OtaEnd(const uint8_t *d, uint8_t n, uint8_t *ack, uint32_t ack_max)
{
    uint32_t final_crc;

    (void)d; (void)n;

    if (!s_active) return ota_ack(M4_OTA_ERR_STATE, 0u, 0u, ack, ack_max);

    /* ---------- ① 长度 ---------- */
    if (s_ota.received != s_ota.total_len)
    {
        printf("[ota] END: len mismatch %lu/%lu\r\n",
               (unsigned long)s_ota.received, (unsigned long)s_ota.total_len);
        return ota_ack(M4_OTA_ERR_RANGE, s_ota.received, s_ota.total_len, ack, ack_max);
    }

    /* ---------- ② 整体 CRC ----------
       ⭐ 注意最后的 ^ 0xFFFFFFFF：M4_Crc32Update 返回的是中间状态。 */
    final_crc = s_run_crc ^ 0xFFFFFFFFu;

    if (final_crc != s_ota.total_crc)
    {
        printf("[ota] END: crc mismatch got 0x%08lX want 0x%08lX -> reject\r\n",
               (unsigned long)final_crc, (unsigned long)s_ota.total_crc);

        /* 校验失败：状态回 IDLE，老固件继续跑 —— 这就是"防砖"的第一道防线 */
        s_ota.state = (uint32_t)OTA_IDLE;
        s_active    = 0u;
        M5_ParamSaveOta(&s_ota);

        return ota_ack(M4_OTA_ERR_CRC, s_ota.received, final_crc, ack, ack_max);
    }

    /* ---------- ③ 置 OTA_READY，交给 Bootloader 裁决 ----------
       ⚠️ 这里【不】自己搬运、也【不】自己切换：
          切换必须由 Bootloader 完成，且 Bootloader 会【自己再算一遍 CRC】
          （不信任 App 的判断 —— App 本身可能是坏的）。 */
    s_ota.state = (uint32_t)OTA_READY;
    M5_ParamSaveOta(&s_ota);

    printf("[ota] END ok, %lu bytes, crc 0x%08lX -> READY, rebooting\r\n",
           (unsigned long)s_ota.total_len, (unsigned long)final_crc);

    s_active = 0u;

    /* ⭐ 延时让 ACK 先发出去再复位（否则上位机收不到最后这一帧 ACK） */
    return ota_ack(M4_OTA_OK, s_ota.received, final_crc, ack, ack_max);
}

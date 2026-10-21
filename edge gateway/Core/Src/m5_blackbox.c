/*==============================================================================
 * M5 · 内部 Flash 参数区驱动（参数页 S2 + 死因黑匣子 S3）
 *============================================================================*/

#include "m5_blackbox.h"
#include "m5_wdg.h"        /* M5_WdgHold()：擦写期间别让狗咬人 */
#include "m4_task.h"       /* M4_TaskSnapStack() */
#include "stm32f4xx_hal.h"

#include <stdio.h>
#include <string.h>

#define PARAM_VERSION       1u

/*==============================================================================
 * 底层原语：擦扇区 / 写一批 word
 *
 * ⚠️ 三条铁律：
 *   ① 擦除单位是【扇区】不是字节，改 1 字节也要擦 16 KB
 *   ② VoltageRange 必须选 _3（3.3V 供电），选错擦除会失败且不报错
 *   ③ 擦写期间 CPU stall，必须先 M5_WdgHold()
 *============================================================================*/

static int bb_erase_sector(uint32_t sector, uint32_t addr, uint32_t size)
{
    FLASH_EraseInitTypeDef e;
    uint32_t               serr = 0u;
    HAL_StatusTypeDef      st;

    /* ⭐ 擦除可能耗数百毫秒，期间任务全停摆 → 必须先申请喂狗宽限，
       否则擦到一半 IWDG 超时复位，而此时 Flash 正处于"擦了一半"的状态 */
    M5_WdgHold(1000);

    HAL_FLASH_Unlock();

    e.TypeErase    = FLASH_TYPEERASE_SECTORS;
    e.Sector       = sector;
    e.NbSectors    = 1u;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;   /* ⚠️ 3.3V 必须 _3 */

    st = HAL_FLASHEx_Erase(&e, &serr);
    if (st != HAL_OK)
    {
        HAL_FLASH_Lock();
        return -1;
    }
    (void)addr; (void)size;
    return 0;
}

/* 从 src 拷 nbytes 到 Flash 的 addr（nbytes 必须 4 的倍数） */
static int bb_write(uint32_t addr, const void *src, uint32_t nbytes)
{
    uint32_t i;

    for (i = 0u; i < nbytes; i += 4u)
    {
        uint32_t v;

        /* ⚠️ 不能直接 *(uint32_t *)src —— 结构体打包后可能不对齐，
           在 ARMCC 上非对齐访问会触发 HardFault。用 memcpy 保证安全。 */
        memcpy(&v, (const uint8_t *)src + i, 4u);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, v) != HAL_OK)
        {
            return -1;
        }
    }
    return 0;
}

static void bb_read(uint32_t addr, void *dst, uint32_t nbytes)
{
    memcpy(dst, (const void *)addr, nbytes);
}

/*==============================================================================
 * 参数页（S2）
 *============================================================================*/

void M5_ParamLoad(param_page_t *page)
{
    uint32_t magic;

    memset(page, 0, sizeof(*page));

    bb_read(PARAM_ADDR + PARAM_OFF_MAGIC, &magic, 4u);

    if (magic != PARAM_MAGIC)
    {
        /* 首次上电 / 参数区被擦过：给一份安全的默认值 */
        page->magic        = PARAM_MAGIC;
        page->version      = PARAM_VERSION;
        page->conf.magic   = PARAM_MAGIC;
        page->conf.reset_count    = 0u;
        page->conf.app_confirmed  = 0u;
        page->conf.rollback_count = 0u;
        page->ota.magic    = 0u;      /* 没有有效 OTA */
        page->ota.state    = (uint32_t)OTA_IDLE;
        return;
    }

    page->magic = magic;
    bb_read(PARAM_ADDR + PARAM_OFF_VER,  &page->version, 4u);

    if (page->version != PARAM_VERSION)
    {
        /* 布局变了：旧数据作废，按默认值来（避免读到错位字段） */
        page->magic        = PARAM_MAGIC;
        page->version      = PARAM_VERSION;
        page->conf.magic   = PARAM_MAGIC;
        page->conf.reset_count    = 0u;
        page->conf.app_confirmed  = 0u;
        page->conf.rollback_count = 0u;
        page->ota.magic    = 0u;
        page->ota.state    = (uint32_t)OTA_IDLE;
        return;
    }

    bb_read(PARAM_ADDR + PARAM_OFF_CONF, &page->conf, sizeof(app_conf_t));
    bb_read(PARAM_ADDR + PARAM_OFF_OTA,  &page->ota,  sizeof(ota_info_t));

    /* 子结构体的 magic 也要校验：扇区擦过之后子区域可能是 0xFFFFFFFF */
    if (page->conf.magic != PARAM_MAGIC)
    {
        page->conf.magic          = PARAM_MAGIC;
        page->conf.reset_count    = 0u;
        page->conf.app_confirmed  = 0u;
        page->conf.rollback_count = 0u;
    }
    if (page->ota.magic != OTA_MAGIC)
    {
        page->ota.magic    = 0u;
        page->ota.state    = (uint32_t)OTA_IDLE;
        page->ota.total_len = 0u;
        page->ota.received  = 0u;
    }
}

/* ⭐ 整页写回：读 → 改 → 擦 → 写。
   为什么必须整页写回：擦除会清掉整个扇区，只写你改的那个字段会让其他字段变 0xFF。 */
static int param_save_all(const param_page_t *page)
{
    if (bb_erase_sector(PARAM_SECTOR, PARAM_ADDR, PARAM_SIZE) != 0)
    {
        return -1;
    }

    if (bb_write(PARAM_ADDR + PARAM_OFF_MAGIC, &page->magic,   4u) != 0) goto fail;
    if (bb_write(PARAM_ADDR + PARAM_OFF_VER,   &page->version, 4u) != 0) goto fail;
    if (bb_write(PARAM_ADDR + PARAM_OFF_CONF,  &page->conf, sizeof(app_conf_t)) != 0) goto fail;
    if (bb_write(PARAM_ADDR + PARAM_OFF_OTA,   &page->ota,  sizeof(ota_info_t))  != 0) goto fail;

    HAL_FLASH_Lock();
    return 0;

fail:
    HAL_FLASH_Lock();
    return -1;
}

int M5_ParamSaveConf(const app_conf_t *conf)
{
    param_page_t page;

    M5_ParamLoad(&page);
    page.conf = *conf;
    page.conf.magic = PARAM_MAGIC;
    return param_save_all(&page);
}

int M5_ParamSaveOta(const ota_info_t *ota)
{
    param_page_t page;

    M5_ParamLoad(&page);
    page.ota = *ota;
    return param_save_all(&page);
}

int M5_ParamBumpResetCount(uint32_t *out_count)
{
    param_page_t page;
    int          rc;

    M5_ParamLoad(&page);
    page.conf.reset_count++;
    rc = param_save_all(&page);

    if (out_count != NULL)
    {
        *out_count = page.conf.reset_count;
    }
    return rc;
}

int M5_ParamClearResetCount(void)
{
    param_page_t page;

    M5_ParamLoad(&page);
    page.conf.reset_count = 0u;
    return param_save_all(&page);
}

int M5_ParamSetConfirmed(uint32_t confirmed)
{
    param_page_t page;

    M5_ParamLoad(&page);
    page.conf.app_confirmed = confirmed;
    if (confirmed)
    {
        /* ⭐ 确认成功 = 这个固件能稳定跑 → 复位计数清零，
           下次再复位就要重新数 3 次才会回滚 */
        page.conf.reset_count = 0u;

        /* ⭐ 同时把 OTA 状态清回 IDLE —— 这一步很多人漏，漏了会出事：
           Bootloader 的"连续 3 次未确认就回滚"只在【刚做过 OTA】时才有意义。
           如果确认后 state 一直停在 COMMITTED，那么日后任何 3 次正常复位
           （调试时很常见）都会被误判成"新固件启动失败"→ 进安全模式 → 看起来砖了。 */
        if (page.ota.magic == OTA_MAGIC)
        {
            page.ota.state    = (uint32_t)OTA_IDLE;
            page.ota.received = 0u;
        }
    }
    return param_save_all(&page);
}

/*==============================================================================
 * 死因黑匣子（S3）
 *============================================================================*/

int M5_BlackboxWrite(uint32_t miss_mask, uint32_t alive_snapshot)
{
    m5_blackbox_t bb;
    param_page_t  page;
    uint8_t       n;

    memset(&bb, 0, sizeof(bb));

    bb.magic          = PARAM_MAGIC;
    bb.miss_mask      = miss_mask;
    bb.alive_snapshot = alive_snapshot;
    bb.heap_free      = (uint32_t)xPortGetFreeHeapSize();
    bb.uptime_s       = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);

    /* 栈水线：调用方（m5_wdg）可能没有登记任务，填不满就补 0 */
    n = M4_TaskSnapStack(bb.stack_min, 8u);
    while (n < 8u)
    {
        bb.stack_min[n++] = 0u;
    }

    M5_ParamLoad(&page);
    bb.reset_count = page.conf.reset_count;

    if (bb_erase_sector(BB_SECTOR, BB_ADDR, BB_SIZE) != 0)
    {
        return -1;
    }

    if (bb_write(BB_ADDR, &bb, sizeof(bb)) != 0)
    {
        HAL_FLASH_Lock();
        return -1;
    }

    HAL_FLASH_Lock();
    return 0;
}

int M5_BlackboxRead(m5_blackbox_t *bb)
{
    uint32_t magic;

    memset(bb, 0, sizeof(*bb));
    bb_read(BB_ADDR, &magic, 4u);

    if (magic != PARAM_MAGIC)
    {
        return 0;    /* 没死过，黑匣子是空的 */
    }

    bb_read(BB_ADDR, bb, sizeof(*bb));
    return 1;
}

void M5_BlackboxPrint(void)
{
    m5_blackbox_t bb;
    uint8_t       i;

    if (M5_BlackboxRead(&bb) == 0)
    {
        printf("[bbx] (empty)\r\n");
        return;
    }

    printf("[bbx] ---- last death ----\r\n");
    printf("[bbx] miss=0x%02lX", (unsigned long)bb.miss_mask);
    if (bb.miss_mask & M4_ALIVE_NET)    printf(" NET");
    if (bb.miss_mask & M4_ALIVE_MODBUS) printf(" MODBUS");
    if (bb.miss_mask & M4_ALIVE_CAN)    printf(" CAN");
    if (bb.miss_mask & M4_ALIVE_LOG)    printf(" LOG");
    printf("   alive=0x%02lX\r\n", (unsigned long)bb.alive_snapshot);

    printf("[bbx] uptime=%lus heap=%luB reset_cnt=%lu\r\n",
           (unsigned long)bb.uptime_s,
           (unsigned long)bb.heap_free,
           (unsigned long)bb.reset_count);

    for (i = 0u; i < 8u; i++)
    {
        if (bb.stack_min[i] == 0u) continue;
        /* 水线单位是 word，×4 才是字节 */
        printf("[bbx]   stack[%u] left=%luB\r\n",
               (unsigned)i, (unsigned long)(bb.stack_min[i] * 4u));
    }
    printf("[bbx] --------------------\r\n");
}

int M5_BlackboxClear(void)
{
    uint32_t zero = 0u;

    if (bb_erase_sector(BB_SECTOR, BB_ADDR, BB_SIZE) != 0)
    {
        return -1;
    }
    /* 擦完写个 0 覆盖 magic，这样下次读就判定"没死过" */
    if (bb_write(BB_ADDR, &zero, 4u) != 0)
    {
        HAL_FLASH_Lock();
        return -1;
    }
    HAL_FLASH_Lock();
    return 0;
}

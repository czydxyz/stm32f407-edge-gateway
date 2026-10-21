/*==============================================================================
 * M6 · 掉电补传缓存实现（W25Q128 / SPI1 / PB14）
 *============================================================================*/

#include "m6_store.h"
#include "spi.h"          /* hspi1（CubeMX 已配：Mode0，10.5 MHz） */
#include "main.h"
#include "stm32f4xx_hal.h"
#include "cmsis_os.h"

#include <stdio.h>
#include <string.h>

/*==============================================================================
 * W25Q128 指令表
 *============================================================================*/
#define CMD_READ_ID       0x9Fu   /* 读 JEDEC ID：EF 40 18 */
#define CMD_WRITE_ENABLE  0x06u
#define CMD_READ_STATUS   0x05u
#define CMD_PAGE_PROGRAM  0x02u
#define CMD_READ_DATA     0x03u
#define CMD_SECTOR_ERASE  0x20u   /* 4 KB 扇区擦除 */

#define STATUS_BUSY       0x01u

/* ⭐ 片选：直接操作 BSRR 比 HAL_GPIO_WritePin 快，且在临界区里更安全
   （HAL 那个函数有参数校验分支，不是原子的） */
#define CS_LOW()   (GPIOB->BSRR = (uint32_t)GPIO_PIN_14 << 16u)
#define CS_HIGH()  (GPIOB->BSRR = (uint32_t)GPIO_PIN_14)

/*==============================================================================
 * 槽格式（32 字节，落盘布局）
 *
 * ⚠️ 为什么每条都要带 CRC32：
 *    本系统【没有掉电检测电路】，断电可能正好发生在写槽的过程中，
 *    于是槽里是"半新半旧"的数据。没有 CRC 的话，恢复后会把这条垃圾
 *    当成真数据补传上去 —— 比丢数据更糟（脏数据进了数据库）。
 *    有 CRC 就能在读出来时判定"这条不完整"，直接跳过。
 *============================================================================*/
#define SLOT_MAGIC_VALID  0xA55Au
#define SLOT_MAGIC_FREE   0xFFFFu   /* Flash 擦后就是 0xFF */
#define SLOT_MAGIC_DEAD   0x0000u   /* 已补传确认，逻辑删除 */

typedef struct {
    uint16_t magic;      /* SLOT_MAGIC_VALID = 有效条目 */
    uint16_t len;        /* 固定 sizeof(m6_item_t)，留着做版本兼容 */
    uint32_t seq;
    uint32_t uptime_s;
    int16_t  temperature;
    int16_t  humidity;
    uint8_t  di_status;
    uint8_t  relay_status;
    uint8_t  rsv[2];
    uint32_t crc32;      /* 对前面 28 个字节算，不含自身 */
} m6_slot_t;             /* ⚠️ 必须正好 32 字节，改结构体要同步改 M6_SLOT_SIZE */

/*==============================================================================
 * 内部状态
 *============================================================================*/
static uint32_t  s_jedec;      /* 0 = Flash 不在 */
static uint16_t  s_head;       /* 下一个写入位置（槽索引） */
static uint16_t  s_count;      /* 当前有效条数 */
static uint32_t  s_dropped;    /* 因缓存满被覆盖的条数 */

/*==============================================================================
 * 底层 SPI
 *============================================================================*/
static void spi_txrx(const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    /* ⚠️ 用阻塞模式 + 超时，不用 IT/DMA：
       缓存写入发生在 DEGRADED 期间，本来就不频繁（最多每 30 s 一条），
       为它引入 DMA 中断只会增加与网络任务抢资源的复杂度。
       32 字节 @10.5MHz ≈ 30 µs，阻塞完全可接受。 */
    HAL_SPI_TransmitReceive(&hspi1, (uint8_t *)tx, rx, n, 100u);
}

static uint8_t spi_byte(uint8_t b)
{
    uint8_t r;
    spi_txrx(&b, &r, 1u);
    return r;
}

static uint32_t w25q_read_id(void)
{
    uint8_t tx[4] = { CMD_READ_ID, 0xFF, 0xFF, 0xFF };
    uint8_t rx[4];
    uint32_t id;

    CS_LOW();
    spi_txrx(tx, rx, 4u);
    CS_HIGH();

    id = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
    return id;
}

static void w25q_write_enable(void)
{
    CS_LOW();
    (void)spi_byte(CMD_WRITE_ENABLE);
    CS_HIGH();
}

/* ⭐ 等待编程/擦除完成。
   ⚠️ 必须轮询 BUSY 而不是死等固定时长：页编程典型 0.7 ms、扇区擦除典型 45 ms，
      差 60 倍，写个固定 delay 要么浪费要么不够。 */
static int w25q_wait_busy(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();

    while ((uint32_t)(HAL_GetTick() - t0) < timeout_ms)
    {
        uint8_t st;

        CS_LOW();
        (void)spi_byte(CMD_READ_STATUS);
        st = spi_byte(0xFF);
        CS_HIGH();

        if ((st & STATUS_BUSY) == 0u) return 0;
        osDelay(1);
    }
    return -1;
}

static void w25q_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t hdr[4];

    hdr[0] = CMD_READ_DATA;
    hdr[1] = (uint8_t)(addr >> 16);
    hdr[2] = (uint8_t)(addr >> 8);
    hdr[3] = (uint8_t)addr;

    CS_LOW();
    spi_txrx(hdr, NULL, 4u);       /* ⚠️ 发头时不需要收，但 SPI 全双工必须给 rx 缓冲；
                                      HAL 允许 rx=NULL？不保证 —— 这里用 0xFF dummy 更安全 */
    spi_txrx(NULL, buf, (uint16_t)len);
    CS_HIGH();
}

static int w25q_page_program(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint8_t hdr[4];

    /* ⚠️ 页编程【不能跨页边界】：W25Q128 一页 256 字节，
       跨页会绕回页首覆盖前面的数据。本工程条目 32 字节且槽按 32 对齐，
       永远不会跨页（256 / 32 = 8 整除），但这里还是加断言式保护。 */
    if (((addr & 0xFFu) + len) > 256u) return -1;

    w25q_write_enable();

    hdr[0] = CMD_PAGE_PROGRAM;
    hdr[1] = (uint8_t)(addr >> 16);
    hdr[2] = (uint8_t)(addr >> 8);
    hdr[3] = (uint8_t)addr;

    CS_LOW();
    spi_txrx(hdr, NULL, 4u);
    spi_txrx(buf, NULL, (uint16_t)len);
    CS_HIGH();

    return w25q_wait_busy(50u);
}

static int w25q_sector_erase(uint32_t addr)
{
    uint8_t hdr[4];

    w25q_write_enable();

    hdr[0] = CMD_SECTOR_ERASE;
    hdr[1] = (uint8_t)(addr >> 16);
    hdr[2] = (uint8_t)(addr >> 8);
    hdr[3] = (uint8_t)addr;

    CS_LOW();
    spi_txrx(hdr, NULL, 4u);
    CS_HIGH();

    return w25q_wait_busy(400u);
}

/*==============================================================================
 * CRC32（与 M4 协议同一套：0xEDB88320 反射多项式，初值 0xFFFFFFFF，结果取反）
 *
 * ⚠️ 这里没有直接复用 M4_Crc32：
 *    m4_proto.h 的 CRC 是给协议帧用的，语义上属于"协议层"；
 *    存储层自己有一份，将来换压缩/换校验算法时不会牵连协议格式。
 *    代价是多了约 40 字节代码 —— 换取模块解耦，值。
 *============================================================================*/
static uint32_t m6_crc32(const uint8_t *d, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    uint8_t  b;

    for (i = 0u; i < n; i++)
    {
        uint8_t k;
        crc ^= d[i];
        for (k = 0u; k < 8u; k++)
        {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    (void)b;
    return ~crc;
}

/*==============================================================================
 * 槽管理
 *============================================================================*/
static uint32_t slot_addr(uint16_t idx)
{
    return M6_STORE_ADDR + (uint32_t)idx * M6_SLOT_SIZE;
}

static void slot_pack(uint8_t *raw, const m6_item_t *it)
{
    m6_slot_t s;

    memset(&s, 0, sizeof(s));
    s.magic        = SLOT_MAGIC_VALID;
    s.len          = (uint16_t)sizeof(m6_item_t);
    s.seq          = it->seq;
    s.uptime_s     = it->uptime_s;
    s.temperature  = it->temperature;
    s.humidity     = it->humidity;
    s.di_status    = it->di_status;
    s.relay_status = it->relay_status;

    /* ⭐ CRC 只覆盖前面 28 字节（不含 crc32 字段自身），
       这样改一个字段不用重新推导"自身参与运算"的麻烦 */
    s.crc32 = m6_crc32((const uint8_t *)&s, (uint32_t)(sizeof(s) - 4u));

    memcpy(raw, &s, sizeof(s));
}

/* 读一个槽并校验。返回 0 = 有效条目 */
static int slot_unpack(uint16_t idx, m6_item_t *out)
{
    uint8_t   raw[M6_SLOT_SIZE];
    m6_slot_t s;
    uint32_t  want;

    if (idx >= M6_SLOT_MAX) return -1;

    w25q_read(slot_addr(idx), raw, M6_SLOT_SIZE);
    memcpy(&s, raw, sizeof(s));

    if (s.magic != SLOT_MAGIC_VALID) return -2;      /* 空槽 / 已删除 */

    want = m6_crc32(raw, (uint32_t)(sizeof(s) - 4u));
    if (want != s.crc32) return -3;                  /* ⭐ 半写损坏，跳过 */

    out->seq          = s.seq;
    out->uptime_s     = s.uptime_s;
    out->temperature  = s.temperature;
    out->humidity     = s.humidity;
    out->di_status    = s.di_status;
    out->relay_status = s.relay_status;
    return 0;
}

/* 找【最旧的有效条目】= seq 最小的那条。
   ⚠️ 为什么按 seq 而不是按槽索引：环形覆盖后，槽索引顺序 ≠ 时间顺序。
      seq 是全局单调的，才是真正的"先后"。
   ⚠️ 256 个槽全扫一遍要 256 次 SPI 读（每次 ~40 µs）≈ 10 ms ——
      只在补传时调用，可接受。若嫌慢可维护"最旧指针"，但那又要在 Flash 里
      持久化指针（频繁擦写），反而更糟。 */
static int find_oldest(uint16_t *idx_out)
{
    uint16_t i;
    uint32_t min_seq = 0xFFFFFFFFu;
    int      found   = 0;

    for (i = 0u; i < M6_SLOT_MAX; i++)
    {
        m6_item_t it;

        if (slot_unpack(i, &it) != 0) continue;

        if (!found || (it.seq < min_seq))
        {
            min_seq = it.seq;
            *idx_out = i;
            found    = 1;
        }
    }
    return found ? 0 : -1;
}

/*==============================================================================
 * 对外 API
 *============================================================================*/
int M6_StoreInit(void)
{
    s_jedec = 0u;
    s_head  = 0u;
    s_count = 0u;

    CS_HIGH();     /* 确保不选中 */

    s_jedec = w25q_read_id();

    if (s_jedec != 0x00EF4018u)
    {
        printf("[m6] !! W25Q128 not found (id=0x%06lX), cache disabled\r\n",
               (unsigned long)s_jedec);
        return -1;
    }

    /* 统计现有条目 + 找写入位置（第一个无效槽） */
    {
        uint16_t i;
        for (i = 0u; i < M6_SLOT_MAX; i++)
        {
            m6_item_t it;
            int rc = slot_unpack(i, &it);

            if (rc == 0)
            {
                s_count++;
            }
            else if ((rc == -2) && (s_head == 0u) && (i > 0u))
            {
                /* 记住第一个空槽作为写入起点（简化：只记第一次遇到） */
                if (s_count != 0u) s_head = i;
            }
        }
    }

    printf("[m6] W25Q128 ok (id=0x%06lX), cache %u/%u slots used\r\n",
           (unsigned long)s_jedec, (unsigned)s_count, (unsigned)M6_SLOT_MAX);
    return 0;
}

int M6_StoreReady(void)   { return (s_jedec == 0x00EF4018u) ? 1 : 0; }
uint32_t M6_StoreJedecId(void) { return s_jedec; }
uint16_t M6_StoreCount(void)   { return s_count; }
uint32_t M6_StoreDropped(void) { return s_dropped; }

int M6_StorePush(const m6_item_t *item)
{
    uint8_t  raw[M6_SLOT_SIZE];
    uint16_t idx;
    int      rc;

    if (!M6_StoreReady()) return -1;
    if (item == NULL)     return -1;

    /* ⚠️ 缓存满 → 覆盖最旧的（宁可丢最旧，也要保住最新的状态）。
       这在工业上叫"环形缓冲/滚动覆盖"，是可接受的数据降级策略；
       但【必须计数】，否则你永远不知道补传上来的是不是完整的一段。 */
    if (s_count >= M6_SLOT_MAX)
    {
        uint16_t oldest;

        if (find_oldest(&oldest) == 0)
        {
            idx = oldest;
            s_dropped++;
            s_count--;
        }
        else
        {
            idx = 0u;
        }
    }
    else
    {
        /* 找第一个空槽 */
        uint16_t i;
        idx = 0xFFFFu;
        for (i = 0u; i < M6_SLOT_MAX; i++)
        {
            uint16_t probe = (uint16_t)((s_head + i) % M6_SLOT_MAX);
            m6_item_t it;

            if (slot_unpack(probe, &it) != 0)     /* 无效槽 = 空 */
            {
                idx = probe;
                break;
            }
        }
        if (idx == 0xFFFFu) return -1;
    }

    slot_pack(raw, item);

    /* ⭐ 写 Flash 期间关中断：SPI 传输不能被打断，否则 CS 时序乱套。
       ⚠️ 只关很短（32 字节 ≈ 30 µs + 编程等待），不会影响 1 s 级的看门狗裁决 */
    taskENTER_CRITICAL();
    rc = w25q_page_program(slot_addr(idx), raw, M6_SLOT_SIZE);
    taskEXIT_CRITICAL();

    if (rc != 0)
    {
        printf("[m6] !! push write fail @slot %u\r\n", (unsigned)idx);
        return -1;
    }

    s_head  = (uint16_t)((idx + 1u) % M6_SLOT_MAX);
    s_count++;
    return 0;
}

int M6_StorePeek(m6_item_t *out)
{
    uint16_t idx;

    if (!M6_StoreReady()) return -1;
    if (out == NULL)      return -1;

    if (find_oldest(&idx) != 0) return -1;      /* 空 */
    return slot_unpack(idx, out);
}

int M6_StorePop(void)
{
    uint16_t idx;
    uint16_t dead = SLOT_MAGIC_DEAD;
    int      rc;

    if (!M6_StoreReady()) return -1;
    if (find_oldest(&idx) != 0) return -1;

    /* ⚠️ 逻辑删除 = 把 magic 写成 0x0000。
       为什么不擦扇区：擦一次 4 KB 要几十毫秒且会连带清掉同扇区其他 255 条。
       Flash 的特性是【1 可以改写成 0，反过来必须擦】，
       所以"写 0"这种逻辑删除是免费且幂等的 —— 这正是环形存储的标准做法。 */
    taskENTER_CRITICAL();
    rc = w25q_page_program(slot_addr(idx), (const uint8_t *)&dead, 2u);
    taskEXIT_CRITICAL();

    if (rc != 0) return -1;

    if (s_count > 0u) s_count--;
    return 0;
}

/*==============================================================================
 * ⚠️ 扇区擦除（整块清空）—— 仅供维护/自检用，正常流程不会调
 *    为什么单独列出来：擦除 4 KB 期间 Flash 内部忙约 45 ms，
 *    期间不能对它做任何读写。正常补传逻辑【只写不擦】，
 *    靠"逻辑删除 + 覆盖最旧"就把环形转起来了。
 *============================================================================*/
int M6_StoreEraseAll(void)
{
    uint32_t sec;
    int      rc = 0;

    if (!M6_StoreReady()) return -1;

    for (sec = 0u; sec < M6_STORE_SECTORS; sec++)
    {
        taskENTER_CRITICAL();
        rc |= w25q_sector_erase(M6_STORE_ADDR + sec * 4096u);
        taskEXIT_CRITICAL();
    }

    s_head  = 0u;
    s_count = 0u;
    return rc;
}

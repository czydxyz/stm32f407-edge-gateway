/*==============================================================================
 * Bootloader · 边缘智能网关（32 KB，Sector 0/1，0x08000000）
 *
 * ⭐ 设计原则：Bootloader 越简单越安全。
 *    它只做 4 件事：读状态 → 校验 → 拷贝 → 跳转。
 *    不做网络、不做协议解析、不跑 RTOS、不打印复杂日志。
 *    原因很实在：Bootloader 里的 bug 【无法通过 OTA 修复】，
 *    它每多 100 行代码，就多一分"必须拆机烧录"的风险。
 *
 * ⛔ 不用 HAL：本文件直接操作寄存器（Flash / RCC / GPIO / USART1）。
 *    理由：① HAL 会拖进一堆用不到的外设驱动，32 KB 虽够但没必要
 *         ② 少一个依赖就少一个变数 —— 越靠底层越可控
 *
 * 启动裁决流程：
 *   ① 读参数区：reset_count++ 并写回
 *   ② ota_state == OTA_READY？
 *        ├ 是 → ⭐ 自己重算 Bank B 的 CRC32（不信任 App 的判断）
 *        │       ├ 通过 → 拷 B→A，置 COMMITTED，reset_count=0
 *        │       └ 不通过 → 保持 Bank A，置 IDLE（拒绝切换）
 *        └ 否 → 直接走 Bank A
 *   ③ 自检 Bank A：初始 MSP 是否落在 SRAM + 复位向量是否落在 Flash 且为 Thumb
 *        ├ 通过 → 跳转 App
 *        └ 不通过 → 进安全模式（LED 慢闪 + 停住，绝不反复重启）
 *============================================================================*/

#include "stm32f4xx.h"
#include "shared/flash_layout.h"

#include <stdint.h>
#include <string.h>

/*==============================================================================
 * 极简串口（USART1 PA9/PA10，115200）—— 只为调试看得见，不是功能的一部分
 *============================================================================*/
static void uart1_init(void)
{
    /* PCLK2 = 84 MHz（SystemInit 里配的）。BRR = 84e6/115200 ≈ 45.57 → 0x2D7 */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    GPIOA->MODER   &= ~((3u << (9 * 2)) | (3u << (10 * 2)));
    GPIOA->MODER   |=  ((2u << (9 * 2)) | (2u << (10 * 2)));   /* AF */
    GPIOA->OTYPER  &= ~((1u << 9) | (1u << 10));
    GPIOA->OSPEEDR |=  ((3u << (9 * 2)) | (3u << (10 * 2)));
    GPIOA->AFR[1]  &= ~((0xFu << ((9 - 8) * 4)) | (0xFu << ((10 - 8) * 4)));
    GPIOA->AFR[1]  |=  ((7u << ((9 - 8) * 4)) | (7u << ((10 - 8) * 4)));  /* AF7 */

    USART1->BRR  = (uint16_t)(84000000u / 115200u);
    USART1->CR1  = USART_CR1_TE | USART_CR1_UE;
}

static void uart_putc(char c)
{
    while (!(USART1->SR & USART_SR_TXE)) { }
    USART1->DR = (uint16_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) { uart_putc(*s++); }
    uart_putc('\r'); uart_putc('\n');
}

static void uart_hex32(uint32_t v)
{
    const char *d = "0123456789ABCDEF";
    int i;
    uart_putc('0'); uart_putc('x');
    for (i = 7; i >= 0; i--)
    {
        uart_putc(d[(v >> (i * 4)) & 0xFu]);
    }
}

/*==============================================================================
 * CRC32（与 App 侧 M4_Crc32 完全一致：初值 0xFFFFFFFF / 多项式 0xEDB88320 / 结果异或）
 *
 * ⚠️ 为什么 Bootloader 要【自己再算一遍】而不能信 App：
 *    App 本身可能是坏的（比如它写的 total_crc 是垃圾值、或者根本没算就置了 READY）。
 *    "Bootloader 独立校验、不信任 App" 是 OTA 防砖的第一原则。
 *============================================================================*/
static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0u; j < 8u; j++)
        {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else          crc = (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/*==============================================================================
 * 内部 Flash 驱动（寄存器版）
 *============================================================================*/
static void flash_unlock(void)
{
    if (FLASH->CR & FLASH_CR_LOCK)
    {
        FLASH->KEYR = 0x45670123u;
        FLASH->KEYR = 0xCDEF89ABu;
    }
}

static void flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

static int flash_erase_sector(uint8_t sector)
{
    uint32_t t = 0;

    while (FLASH->SR & FLASH_SR_BSY)
    {
        if (++t > 0x1000000u) return -1;
    }

    FLASH->CR  = FLASH_CR_SER | ((uint32_t)sector << 3) | FLASH_CR_PSIZE_1;  /* PSIZE=32bit */
    FLASH->CR |= FLASH_CR_STRT;

    t = 0;
    while (FLASH->SR & FLASH_SR_BSY)
    {
        if (++t > 0x1000000u) return -1;
    }

    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB);
    return 0;
}

static int flash_program_word(uint32_t addr, uint32_t v)
{
    uint32_t t = 0;

    while (FLASH->SR & FLASH_SR_BSY)
    {
        if (++t > 0x1000000u) return -1;
    }

    FLASH->CR = FLASH_CR_PG | FLASH_CR_PSIZE_1;
    *(volatile uint32_t *)addr = v;

    t = 0;
    while (FLASH->SR & FLASH_SR_BSY)
    {
        if (++t > 0x1000000u) return -1;
    }

    FLASH->CR &= ~FLASH_CR_PG;

    /* ⭐ 写完一定要读回来比对：Flash 编程失败不会报错，只能靠回读发现 */
    return (*(volatile uint32_t *)addr == v) ? 0 : -1;
}

/*==============================================================================
 * 参数区（S2, 0x08008000）读写 —— 与 App 共用 flash_layout.h 的偏移定义
 *
 * ⚠️ 擦除单位是扇区：改一个字段也要擦整个 S2 再整页写回。
 *============================================================================*/
static void param_read(param_page_t *p)
{
    uint32_t base = PARAM_ADDR;

    memcpy(&p->magic,   (const void *)(base + PARAM_OFF_MAGIC), 4u);
    memcpy(&p->version, (const void *)(base + PARAM_OFF_VER),   4u);

    if (p->magic != PARAM_MAGIC)
    {
        memset(p, 0, sizeof(*p));
        p->magic   = PARAM_MAGIC;
        p->version = 1u;
        p->conf.magic = PARAM_MAGIC;
        p->ota.state  = (uint32_t)OTA_IDLE;
        return;
    }

    memcpy(&p->conf, (const void *)(base + PARAM_OFF_CONF), sizeof(app_conf_t));
    memcpy(&p->ota,  (const void *)(base + PARAM_OFF_OTA),  sizeof(ota_info_t));

    if (p->conf.magic != PARAM_MAGIC)
    {
        memset(&p->conf, 0, sizeof(app_conf_t));
        p->conf.magic = PARAM_MAGIC;
    }
    if (p->ota.magic != OTA_MAGIC)
    {
        memset(&p->ota, 0, sizeof(ota_info_t));
        p->ota.state = (uint32_t)OTA_IDLE;
    }
}

static int param_write(const param_page_t *p)
{
    uint32_t base = PARAM_ADDR;
    uint32_t i;
    const uint8_t *src;
    uint32_t nbytes;

    flash_unlock();

    if (flash_erase_sector((uint8_t)PARAM_SECTOR) != 0)
    {
        flash_lock();
        return -1;
    }

    /* magic */
    if (flash_program_word(base + PARAM_OFF_MAGIC, p->magic) != 0) goto fail;
    if (flash_program_word(base + PARAM_OFF_VER,   p->version) != 0) goto fail;

    /* conf */
    src = (const uint8_t *)&p->conf;
    nbytes = sizeof(app_conf_t);
    for (i = 0u; i < nbytes; i += 4u)
    {
        uint32_t v;
        memcpy(&v, src + i, 4u);
        if (flash_program_word(base + PARAM_OFF_CONF + i, v) != 0) goto fail;
    }

    /* ota */
    src = (const uint8_t *)&p->ota;
    nbytes = sizeof(ota_info_t);
    for (i = 0u; i < nbytes; i += 4u)
    {
        uint32_t v;
        memcpy(&v, src + i, 4u);
        if (flash_program_word(base + PARAM_OFF_OTA + i, v) != 0) goto fail;
    }

    flash_lock();
    return 0;

fail:
    flash_lock();
    return -1;
}

/*==============================================================================
 * App 合法性检查（防砖的最后一道保险）
 *
 * 防的是什么：Bank 是空的（0xFFFFFFFF）或只下载了一半。
 * 这两种情况下向量表的前两个字都是垃圾，直接跳必然 HardFault。
 *============================================================================*/
static int app_is_valid(uint32_t addr)
{
    uint32_t msp = *(volatile uint32_t *)(addr + 0u);   /* 初始栈顶 */
    uint32_t rst = *(volatile uint32_t *)(addr + 4u);   /* 复位向量 */

    /* MSP 必须落在 SRAM 范围内（F407: 128 KB SRAM @ 0x20000000） */
    if ((msp < 0x20000000u) || (msp > 0x20020000u)) return 0;

    /* 复位向量必须落在 Flash 范围内 */
    if ((rst < FLASH_BASE_ADDR) || (rst > (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE))) return 0;

    /* ⚠️ 最低位必须是 1（Thumb 状态）—— Cortex-M 不允许 ARM 状态的向量 */
    if ((rst & 1u) == 0u) return 0;

    return 1;
}

/*==============================================================================
 * 跳转
 *============================================================================*/
static void jump_to_app(uint32_t addr)
{
    uint32_t   msp = *(volatile uint32_t *)(addr + 0u);
    uint32_t   rst = *(volatile uint32_t *)(addr + 4u);
    void (*jump)(void);

    /* ① 先关中断：跳转过程中不能有任何中断打进来。
       ⚠️ 顺序很重要：先关中断再关外设。反过来的话，关外设期间一个中断进来，
          而它的 ISR 在新固件里地址已经变了 → 直接飞。 */
    __disable_irq();

    /* ② 停掉 SysTick（Bootloader 用它做延时，App 会重新配） */
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;

    /* ③ 关掉 Bootloader 期间开过的外设（这里只有 USART1 和 GPIOA 时钟） */
    USART1->CR1 = 0u;
    RCC->APB2ENR &= ~RCC_APB2ENR_USART1EN;

    /* ④ 设主栈指针 + ⭐ 中断向量表偏移
       ⚠️ SCB->VTOR 是最多人漏的一行。漏了之后现象极具迷惑性：
          App 能跑，但【一发生中断就死】—— 因为中断向量还指向 Bootloader 区。 */
    __set_MSP(msp);
    SCB->VTOR = addr;

    /* ⑤ 跳转（正常不会返回） */
    jump = (void (*)(void))rst;
    jump();

    while (1) { }
}

/*==============================================================================
 * 安全模式：停住 + LED 慢闪 + 只等 OTA
 *
 * ⭐ 为什么"停住"而不是"不停重启"：
 *    ① 不停重启会让运维看不到任何稳定状态，现场无法诊断
 *    ② 每次重启都可能写参数区，加速 Flash 磨损
 *    ③ 反复重启还可能把最后一次可救的机会折腾没了
 *============================================================================*/
static void safe_mode(void)
{
    /* LED0 = PF9（正点原子探索者） */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;
    GPIOF->MODER   &= ~(3u << (9 * 2));
    GPIOF->MODER   |=  (1u << (9 * 2));      /* 输出 */
    GPIOF->OTYPER  &= ~(1u << 9);
    GPIOF->OSPEEDR &= ~(3u << (9 * 2));
    GPIOF->BSRR     = (1u << (9 + 16));      /* 先灭 */

    uart_puts("[BL] SAFE MODE: LED slow blink, waiting for OTA");

    for (;;)
    {
        volatile uint32_t d;
        GPIOF->BSRR = (1u << 9);             /* 亮 */
        for (d = 0; d < 2000000u; d++) { }
        GPIOF->BSRR = (1u << (9 + 16));      /* 灭 */
        for (d = 0; d < 2000000u; d++) { }
    }
}

/*==============================================================================
 * 拷贝 Bank B → Bank A
 *============================================================================*/
static int copy_b_to_a(uint32_t len)
{
    uint32_t sec;
    uint32_t off;
    uint32_t words = (len + 3u) / 4u;

    flash_unlock();

    /* ① 擦 Bank A（S4~S7） */
    for (sec = (uint32_t)APP_A_SECTOR_FIRST; sec <= (uint32_t)APP_A_SECTOR_LAST; sec++)
    {
        if (flash_erase_sector((uint8_t)sec) != 0)
        {
            flash_lock();
            return -1;
        }
    }

    /* ② 逐 word 搬（每写完读回比对，失败立刻停） */
    for (off = 0u; off < words; off++)
    {
        uint32_t v = *(volatile uint32_t *)(APP_B_ADDR + off * 4u);

        if (v == 0xFFFFFFFFu) continue;      /* 空区跳过，省一点时间 */

        if (flash_program_word(APP_A_ADDR + off * 4u, v) != 0)
        {
            flash_lock();
            return -1;
        }
    }

    flash_lock();
    return 0;
}

/*==============================================================================
 * main
 *============================================================================*/
int main(void)
{
    param_page_t page;
    uint32_t     rst_cnt;

    uart1_init();
    uart_puts("[BL] boot");

    /* ---------- ① 读参数 + 复位计数 ++ ---------- */
    param_read(&page);
    rst_cnt = page.conf.reset_count + 1u;
    page.conf.reset_count = rst_cnt;
    page.conf.magic = PARAM_MAGIC;
    (void)param_write(&page);

    uart_puts("[BL] reset_count=");
    uart_hex32(rst_cnt);

    /* ---------- ② OTA 裁决 ---------- */
    if (page.ota.state == (uint32_t)OTA_READY)
    {
        uint32_t crc;

        uart_puts("[BL] OTA_READY, verifying bank B...");

        if ((page.ota.total_len == 0u) || (page.ota.total_len > APP_B_SIZE))
        {
            uart_puts("[BL] bad total_len -> reject");
            page.ota.state = (uint32_t)OTA_IDLE;
            (void)param_write(&page);
        }
        else
        {
            /* ⭐ 独立校验：自己按 total_len 整片算一遍，与参数区里的 total_crc 比 */
            crc = crc32_calc((const uint8_t *)APP_B_ADDR, page.ota.total_len);

            uart_puts("[BL] crc got=");
            uart_hex32(crc);
            uart_puts("[BL] crc want=");
            uart_hex32(page.ota.total_crc);

            if (crc != page.ota.total_crc)
            {
                /* 校验失败 → 拒绝切换，老固件继续跑（防砖第一道防线） */
                uart_puts("[BL] CRC MISMATCH -> reject, keep old fw");
                page.ota.state = (uint32_t)OTA_IDLE;
                (void)param_write(&page);
            }
            else
            {
                uart_puts("[BL] CRC ok, copying B -> A...");
                if (copy_b_to_a(page.ota.total_len) != 0)
                {
                    uart_puts("[BL] COPY FAILED -> reject");
                    page.ota.state = (uint32_t)OTA_IDLE;
                    (void)param_write(&page);
                }
                else
                {
                    uart_puts("[BL] copy done, COMMITTED");
                    page.ota.state          = (uint32_t)OTA_COMMITTED;
                    page.conf.reset_count   = 0u;
                    page.conf.app_confirmed = 0u;   /* 等新固件自己跑满 30s 再确认 */
                    (void)param_write(&page);

                    /* 拷贝完立刻读回一次校验，防止"擦写半途掉电"留下坏固件 */
                    if (crc32_calc((const uint8_t *)APP_A_ADDR, page.ota.total_len) != crc)
                    {
                        uart_puts("[BL] !! bank A verify failed after copy");
                        safe_mode();
                    }
                }
            }
        }
    }

    /* ---------- ③ 连续复位保护 ----------
       新固件启动即挂 → 没确认 → 复位 → reset_count 累加 → 到 3 判定不可用。
       ⚠️ 只在 ota_state == OTA_COMMITTED（即"刚切换完新固件"）时才启用。
          否则正常运行中的 3 次普通复位（调试时很常见）也会被误判成
          "新固件启动失败" → 进安全模式 → 看起来像砖了。

       ⚠️ 本工程是"接收区拷贝式"双 Bank（不是 ping-pong），切换时 Bank A 的
          旧固件已被覆盖，所以【没有"恢复上一个版本"这回事】。
          这里的"回滚"做的是：清掉 Bank B、状态置 IDLE、进安全模式等运维重传。
          这一点必须如实写进文档，不能声称"自动回滚到上一版本"。 */
    if ((page.conf.app_confirmed == 0u) && (rst_cnt >= RESET_COUNT_ROLLBACK) &&
        (page.ota.state == (uint32_t)OTA_COMMITTED))
    {
        uart_puts("[BL] !! app not confirmed after 3 resets -> safe mode");

        page.ota.state  = (uint32_t)OTA_IDLE;
        page.ota.magic  = 0u;
        (void)param_write(&page);

        safe_mode();   /* 不返回 */
    }

    /* ---------- ④ 自检并跳转 ---------- */
    if (!app_is_valid(APP_A_ADDR))
    {
        uart_puts("[BL] !! bank A invalid (msp/rst out of range) -> safe mode");
        safe_mode();
    }

    uart_puts("[BL] jump to app 0x08010000");
    jump_to_app(APP_A_ADDR);

    while (1) { }
}

/*==============================================================================
 * M5 · ESP8266 备份链路实现（AT 状态机 + USART3 DMA 循环 + IDLE 分帧）
 *============================================================================*/

#include "m5_esp.h"
#include "usart.h"        /* huart3 */
#include "stm32f4xx_hal.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/*==============================================================================
 * 状态机
 *============================================================================*/
typedef enum {
    ESP_ST_OFF = 0,     /* 未初始化 */
    ESP_ST_AT,          /* 发 AT 探测 */
    ESP_ST_CWMODE,      /* 设 Station 模式 */
    ESP_ST_CWJAP,       /* 连路由器（可能要几秒） */
    ESP_ST_CIPMUX,      /* 单连接 */
    ESP_ST_CIPSTART,    /* 连上位机 TCP */
    ESP_ST_READY,       /* ⭐ 可发数据 */
    ESP_ST_WAIT,        /* 等待应答（s_next_ok / s_next_fail 决定去向） */
    ESP_ST_SENDWAIT     /* 等 ">" 提示符 */
} esp_state_t;

/*==============================================================================
 * 内部变量
 *============================================================================*/
static DMA_HandleTypeDef hdma_usart3_rx;

/* ⚠️ DMA 缓冲不能放 CCM（CCM 不挂 DMA 总线，BUG-021） */
static uint8_t  s_rx_buf[M5_ESP_RXBUF];
static uint16_t s_rx_last;          /* 上次处理到的位置（环形游标） */

static char     s_resp[M5_ESP_RESPBUF];   /* 线性应答缓冲 */
static uint16_t s_resp_len;

static esp_state_t s_state = ESP_ST_OFF;
static esp_state_t s_next_ok;
static esp_state_t s_next_fail;
static const char *s_wait_key;      /* 等待的关键字（"OK" / ">" / "CONNECT"） */
static uint32_t    s_deadline;      /* 超时时刻（HAL_GetTick 域） */

static uint8_t     s_send_pending;  /* 1 = 有数据在等 ">" */
static const uint8_t *s_send_data;
static uint16_t       s_send_len;

static uint32_t    s_tick_acc;      /* 粗略计时（用于超时，避免依赖精确 tick） */

/* ⭐ BUG-063：AT 重试的指数退避状态。
   模块没接 / P2 跳线没跳时，"失败"是【稳态】而不是事件，
   固定间隔重试会每 2~3 s 刷一条日志，把 [fan] / [m4] 的关键行全冲掉；
   而且 USART3 的 DMA+IDLE 会被无谓地反复打断，白吃 CPU。
   改成：失败越多等越久（1s→2s→4s…上限 30s），且日志只打状态变化。 */
static uint32_t    s_fail_streak;   /* 连续失败次数（成功一次即清零） */
static uint32_t    s_backoff_until; /* 退避到期时刻：这之前不发 AT */

/*==============================================================================
 * 底层：DMA 循环接收 + IDLE
 *============================================================================*/

/* 把 DMA 环形缓冲里的新字节搬到线性应答缓冲。
   ⚠️ 为什么要搬：DMA 是环形的，收到的数据在环里是"绕圈"的，
      关键字匹配需要一个连续的内存，所以拷到线性缓冲里再做 strstr。 */
static void esp_pump_rx(void)
{
    uint16_t ndtr;
    uint16_t pos;
    uint16_t i;

    /* NDTR = 剩余待传数量；已收 = SIZE - NDTR */
    ndtr = (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_usart3_rx);
    pos  = (uint16_t)(M5_ESP_RXBUF - ndtr);

    if (pos == s_rx_last) return;    /* 没有新字节 */

    while (s_rx_last != pos)
    {
        uint8_t b = s_rx_buf[s_rx_last];

        s_rx_last++;
        if (s_rx_last >= M5_ESP_RXBUF) s_rx_last = 0u;

        /* ⭐ 收到 '\n' 就认为一条应答结束，重置线性缓冲，
           避免旧的 "OK" 一直留在缓冲里导致误匹配 */
        if (b == '\n')
        {
            if (s_resp_len < (M5_ESP_RESPBUF - 1u))
            {
                s_resp[s_resp_len] = '\0';
            }
            /* 保留内容供 strstr 匹配，不在这里清零；
               清零时机在 esp_send_cmd() 里（发新命令前清空） */
            continue;
        }
        if (b == '\r') continue;

        if (s_resp_len < (M5_ESP_RESPBUF - 1u))
        {
            s_resp[s_resp_len++] = (char)b;
            s_resp[s_resp_len]   = '\0';
        }
    }
    (void)i;
}

/* 应答里是否含关键字 */
static int esp_resp_has(const char *key)
{
    return (strstr(s_resp, key) != NULL) ? 1 : 0;
}

static void esp_uart_send(const char *fmt, ...)
{
    char    buf[128];
    int     n;
    va_list ap;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0) return;

    HAL_UART_Transmit(&huart3, (uint8_t *)buf, (uint16_t)n, 500u);
}

/* 发一条 AT 命令并进入等待态 */
static void esp_send_cmd(const char *cmd, const char *key,
                         esp_state_t ok, esp_state_t fail,
                         uint32_t timeout_ms)
{
    s_resp_len = 0u;
    s_resp[0]  = '\0';

    esp_uart_send("%s", cmd);

    s_wait_key  = key;
    s_next_ok   = ok;
    s_next_fail = fail;
    s_deadline  = HAL_GetTick() + timeout_ms;
    s_state     = ESP_ST_WAIT;
}

/* ⭐ BUG-063：一次 AT 交互失败（超时 / ERROR / busy）后的统一处理 */
static void esp_on_fail(void)
{
    uint32_t shift;
    uint32_t backoff;

    s_fail_streak++;

    if (s_fail_streak == 1u)
    {
        /* 第一次失败：只在这里说一次"开始退避重试" */
        printf("[esp] not responding, retrying with backoff\r\n");
    }
    else if (s_fail_streak >= 3u)
    {
        /* ⭐ 之后只在【退避周期】上打（4s→8s→16s→30s→30s…），
           不是每次重试都打 —— 否则又变回刷屏。
           为什么还要打：完全静默会让人误以为模块任务挂了，
           留一条低频心跳能区分"在退避"和"死机"。 */
        printf("[esp] still no response (retry #%lu)\r\n",
               (unsigned long)s_fail_streak);
    }

    /* 指数退避：1s → 2s → 4s → 8s → 16s → 30s（封顶）
       ⚠️ 用移位而不是 pow()：这是中断/任务上下文，不引浮点库 */
    shift = (s_fail_streak - 1u);
    if (shift > 5u) shift = 5u;               /* 2^5 = 32 × 1s ≈ 30s 封顶 */
    backoff = (1000u << shift);
    if (backoff > 30000u) backoff = 30000u;

    s_backoff_until = HAL_GetTick() + backoff;

    /* ⚠️ 注意：这里【不】立刻发 AT。状态回到 ESP_ST_AT，
       由 ESP_ST_AT 分支自己检查退避是否到期（见下）。
       把"等待"和"重发"分开，是为了让 defaultTask 在这期间照样能跑别的。 */
    s_state = s_next_fail;
}

/*==============================================================================
 * 初始化
 *============================================================================*/
void M5_EspInit(void)
{
    /* ---------- 手写 USART3 RX 的 DMA（CubeMX 没给 USART3 配 DMA） ----------
       USART3_RX = DMA1_Stream1 / Channel 4
       USART3_TX 用阻塞发送就够了（AT 命令都很短） */
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_usart3_rx.Instance                 = DMA1_Stream1;
    hdma_usart3_rx.Init.Channel             = DMA_CHANNEL_4;
    hdma_usart3_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_usart3_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_usart3_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart3_rx.Init.Mode                = DMA_CIRCULAR;   /* ⭐ 循环 */
    hdma_usart3_rx.Init.Priority            = DMA_PRIORITY_LOW;
    hdma_usart3_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    hdma_usart3_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    hdma_usart3_rx.Init.MemBurst            = DMA_MBURST_SINGLE;
    hdma_usart3_rx.Init.PeriphBurst         = DMA_PBURST_SINGLE;

    HAL_DMA_Init(&hdma_usart3_rx);
    __HAL_LINKDMA(&huart3, hdmarx, hdma_usart3_rx);

    HAL_UART_Receive_DMA(&huart3, s_rx_buf, M5_ESP_RXBUF);

    /* ⭐ 开 IDLE 中断：总线空闲 = 一帧收完。RS485 那套思路的直接复用。 */
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_IDLE);

    s_rx_last  = 0u;
    s_resp_len = 0u;
    s_state    = ESP_ST_AT;
    s_tick_acc = 0u;

    printf("[esp] init, USART3 DMA+IDLE ready\r\n");
}

void M5_EspIdleCallback(void)
{
    /* IDLE 触发：说明模块刚发完一串。这里只做"把字节搬进线性缓冲"，
       真正的状态推进交给 M5_EspTick()（不要在 ISR 里做字符串匹配）。 */
    esp_pump_rx();
}

void M5_EspIrqHandler(void)
{
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_IDLE))
    {
        __HAL_UART_CLEAR_IDLEFLAG(&huart3);   /* 清 IDLE 标志（读 SR+DR 的封装） */
        M5_EspIdleCallback();
    }
}

/*==============================================================================
 * 状态机推进（每 200 ms）
 *============================================================================*/
void M5_EspTick(uint32_t period_ms)
{
    if (s_state == ESP_ST_OFF) return;

    s_tick_acc += period_ms;

    /* 先把 DMA 里的新字节搬进来（IDLE 可能没触发，比如模块一次发了很多） */
    esp_pump_rx();

    switch (s_state)
    {
    case ESP_ST_AT:
        /* ⭐ BUG-063：退避期内【不发】AT。
           模块不在时这是稳态，狂发 AT 只会刷屏 + 白占 USART3。 */
        if ((int32_t)(HAL_GetTick() - s_backoff_until) < 0) break;
        esp_send_cmd("AT\r\n", "OK", ESP_ST_CWMODE, ESP_ST_AT, 1000u);
        break;

    case ESP_ST_CWMODE:
        esp_send_cmd("AT+CWMODE=1\r\n", "OK", ESP_ST_CWJAP, ESP_ST_AT, 2000u);
        break;

    case ESP_ST_CWJAP:
        /* 连路由器：这一步可能要 5~10 s，超时给足 */
        esp_send_cmd("AT+CWJAP=\"" M5_ESP_SSID "\",\"" M5_ESP_PASS "\"\r\n",
                     "OK", ESP_ST_CIPMUX, ESP_ST_AT, 15000u);
        break;

    case ESP_ST_CIPMUX:
        esp_send_cmd("AT+CIPMUX=0\r\n", "OK", ESP_ST_CIPSTART, ESP_ST_AT, 2000u);
        break;

    case ESP_ST_CIPSTART:
        /* ⚠️ 这里用【字符串拼接】而不是 "%d" 格式化：
           esp_send_cmd 内部是 esp_uart_send("%s", cmd)，格式串里的 %d 不会被展开。 */
        esp_send_cmd("AT+CIPSTART=\"TCP\",\"" M5_ESP_PC_IP "\"," M5_ESP_PC_PORT_STR "\r\n",
                     "CONNECT", ESP_ST_READY, ESP_ST_AT, 8000u);
        break;

    case ESP_ST_WAIT:
        if (esp_resp_has(s_wait_key))
        {
            uint8_t had_fail = (s_fail_streak != 0u) ? 1u : 0u;

            /* ⭐ 成功一次就把退避计数清零 —— 退避只在【连续】失败时加深 */
            s_fail_streak  = 0u;
            s_backoff_until = 0u;

            s_state = s_next_ok;
            if (s_state == ESP_ST_READY)
            {
                printf("[esp] READY (TCP %s:%d)%s\r\n",
                       M5_ESP_PC_IP, M5_ESP_PC_PORT,
                       had_fail ? " [recovered]" : "");
            }
        }
        else if (esp_resp_has("ERROR") || esp_resp_has("FAIL") ||
                 esp_resp_has("busy"))
        {
            /* ⭐ 模块明确报错也算失败 —— 必须能退，不能干等 */
            esp_on_fail();
        }
        else if ((int32_t)(HAL_GetTick() - s_deadline) >= 0)
        {
            /* ⭐ 超时分支必须有：模块不回应的情况太常见了 */
            esp_on_fail();
        }
        break;

    case ESP_ST_SENDWAIT:
        if (esp_resp_has(">"))
        {
            if (s_send_pending && (s_send_data != NULL))
            {
                HAL_UART_Transmit(&huart3, (uint8_t *)s_send_data, s_send_len, 1000u);
            }
            s_send_pending = 0u;
            s_resp_len = 0u;
            s_resp[0]  = '\0';
            s_deadline = HAL_GetTick() + 3000u;
            s_state    = ESP_ST_WAIT;
            s_wait_key = "SEND OK";
            s_next_ok  = ESP_ST_READY;
            s_next_fail = ESP_ST_AT;   /* 发失败 → 回 AT 重连 */
        }
        else if ((int32_t)(HAL_GetTick() - s_deadline) >= 0)
        {
            /* 等 ">" 超时 → 重连（也走退避，别和别的失败路径两套行为） */
            s_send_pending = 0u;
            s_next_fail    = ESP_ST_AT;
            esp_on_fail();
        }
        break;

    case ESP_ST_READY:
        /* 保活：ESP8266 的 TCP 不会自动保活，长时间不发会被路由踢掉 */
        if (s_tick_acc >= 30000u)
        {
            s_tick_acc = 0u;
            /* 发一个空探测：失败会被下面的 AT 流程重新连 */
            if (!esp_resp_has("CLOSED"))
            {
                s_resp_len = 0u;
                s_resp[0]  = '\0';
            }
        }
        /* 收到 CLOSED 说明连接被对端/路由断开 → 重新走一遍连接流程 */
        if (esp_resp_has("CLOSED"))
        {
            printf("[esp] CLOSED, reconnect\r\n");
            s_state = ESP_ST_CIPSTART;
        }
        break;

    default:
        break;
    }
}

int M5_EspIsUp(void)
{
    return (s_state == ESP_ST_READY) ? 1 : 0;
}

/*==============================================================================
 * 发数据
 *============================================================================*/
int M5_EspSend(const uint8_t *data, uint16_t len)
{
    if (s_state != ESP_ST_READY) return -1;
    if (data == NULL || len == 0u) return -1;

    /* ⚠️ AT+CIPSEND 单次上限 2048 */
    if (len > 2048u) return -1;

    s_resp_len = 0u;
    s_resp[0]  = '\0';

    /* ① 告诉模块要发多少字节 */
    esp_uart_send("AT+CIPSEND=%u\r\n", (unsigned)len);

    /* ② 等模块回 ">" 才能发数据（不是回 OK！这是最容易搞错的一步） */
    s_send_pending = 1u;
    s_send_data    = data;
    s_send_len     = len;
    s_deadline     = HAL_GetTick() + 2000u;
    s_state        = ESP_ST_SENDWAIT;

    /* ⚠️ 这里【不等待结果】：状态机在后续 tick 里完成发送。
       返回值 0 只表示"已受理"，真正的成败由 M5_LinkReportResult() 统计。 */
    return 0;
}

void M5_EspDisconnect(void)
{
    esp_uart_send("AT+CIPCLOSE\r\n");
    s_state    = ESP_ST_CIPSTART;
    s_resp_len = 0u;
    s_resp[0]  = '\0';
}

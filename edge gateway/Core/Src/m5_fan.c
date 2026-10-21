/*==============================================================================
 * M5 · PWM 风扇调速（TIM3 CH1 / PA6，1 kHz）
 *============================================================================*/

#include "m5_fan.h"
#include "m4_pool.h"
#include "tim.h"          /* htim3 */
#include "stm32f4xx_hal.h"

#include <stdio.h>

#define FAN_ARR          999u        /* 与 CubeMX 里 TIM3 的 Period 一致 */

static uint8_t s_duty;        /* 当前占空比百分比 */
static uint8_t s_running;     /* 风扇是否在转（滞回用） */
static uint8_t s_cutoff;      /* 超温保护是否已触发 */
static uint8_t s_prev_run;    /* 上一次的运行态（打印 START/STOP 用） */
static uint8_t s_inited;

void M5_FanInit(void)
{
    s_duty     = 0u;
    s_running  = 0u;
    s_prev_run = 0u;
    s_cutoff   = 0u;

    /* ⭐ 必须先 Start 才有输出；CubeMX 只生成初始化，不会帮你 Start */
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 0u);

    s_inited = 1u;
    printf("[fan] PWM ready, TIM3 CH1 @1kHz\r\n");

#if M5_FAN_TEST_INJECT
    printf("[fan] !! TEST INJECT MODE ON, T is FAKE - set M5_FAN_TEST_INJECT=0 when done\r\n");
#endif
}

uint8_t M5_FanDuty(void)    { return s_duty; }
uint8_t M5_FanCutoff(void)  { return s_cutoff; }

/* ⭐ BUG-064：对外状态上报【只能】用这个函数，不要用内部状态位 s_running。
   为什么：s_running 是"滞回记忆位"，它的语义是"上次是转着的"，
   在 24.0~26.0℃ 这段回差区间里它仍为 1，但 duty 已经是 0、PWM 实际停转了
   （M5-g 实测：注入 25.0℃ 打的是 `duty=0%`，但没有打 `fan STOP`）。
   它记的是历史，不是现在 —— 拿它当"风扇状态"上报会误报"运行中"。 */
uint8_t M5_FanRunning(void) { return (s_duty > 0u) ? 1u : 0u; }

/*==============================================================================
 * ⚠️ M5-g 验收钩子：伪造数据池温度，自动扫完整条风扇曲线
 *
 * ⭐ 为什么"伪造数据池"而不是"直接改 s_duty"：
 *    直接改 duty 只验了 25% 那行算术，链路是假的。
 *    写进数据池再让 M5_FanUpdate() 读出来，验的是
 *    【采集 → 数据池（加锁+时间戳）→ 调速算法 → 写 CCR】整条真实路径，
 *    真传感器接上以后这段代码一个字都不用改。
 *============================================================================*/
#if M5_FAN_TEST_INJECT

/* 扫描序列（单位 ×10℃）—— 一档一个验收点，一轮 8 档 × 5 s = 40 s 验完 5 条路径：
 *   ① 24.0 冷机   duty=0，不转
 *   ② 27.0 启动   越过 FAN_T_START(26.0) → duty = 25 + (10*75/80) = 34
 *   ③ 30.0 超温   触发 cutoff 切断继电器          duty = 25 + (40*75/80) = 62
 *   ④ 34.0 全速   线性段上边界 → duty = 100
 *   ⑤ 31.0 回落   duty = 71；仍 > 28.0 → 超温保护【不】解除（验证滞回不是单边）
 *   ⑥ 26.5 低位   duty = 29；< 28.0 → 打印 over-temp cleared（保护解除）
 *   ⑦ 25.0 低于启动点 duty = 0，但【仍在运行态】（要降到 24.0 才停）← 滞回关键点
 *   ⑧ 23.0 停机   < 24.0 → s_running = 0，风扇真正停                 ← 滞回关键点
 */
static const int16_t s_inject_seq[] = {
    240, 270, 300, 340, 310, 265, 250, 230
};
#define INJECT_N         (sizeof(s_inject_seq) / sizeof(s_inject_seq[0]))
#define INJECT_HOLD_MS   5000u     /* 每档保持 5 s（monitor 是 1 s 一轮，够采样 5 次） */

static void fan_test_inject(void)
{
    static uint32_t s_t0;
    static uint8_t  s_idx;
    int16_t         t;

    if (s_t0 == 0u)
    {
        s_t0 = HAL_GetTick();
    }

    /* ⚠️ 用无符号减法读时间差：HAL_GetTick 49 天回绕一次，这样写永远正确 */
    if ((uint32_t)(HAL_GetTick() - s_t0) >= INJECT_HOLD_MS)
    {
        s_t0 = HAL_GetTick();
        s_idx++;
        if (s_idx >= INJECT_N) s_idx = 0u;   /* 扫完一轮从头再来，方便长时间观察 */

        t = s_inject_seq[s_idx];
        printf("[fan-test] >> inject T=%ld.%ldC\r\n",
               (long)(t / 10), (long)(t % 10));
    }

    /* 写数据池（内部自带锁 + 更新时间戳），后续 M4_PoolGetSnapshot() 读的就是它 */
    M4_PoolSetTempHumi(s_inject_seq[s_idx], 500);
}
#endif /* M5_FAN_TEST_INJECT */

/*==============================================================================
 * 温度 → 占空比（每 1 s 由 monitor 调用）
 *============================================================================*/
void M5_FanUpdate(void)
{
    m4_pool_t snap;
    uint8_t   duty;

    if (!s_inited) return;

#if M5_FAN_TEST_INJECT
    /* ⚠️ 验收钩子：注入假温度。验完把 m5_fan.h 的 M5_FAN_TEST_INJECT 改回 0 */
    fan_test_inject();
#endif

    /* ⭐ 拿快照，不要在锁里做慢操作（M4_PoolGetSnapshot 内部一次 memcpy 拷走） */
    M4_PoolGetSnapshot(&snap);

    /* ⚠️ 温度是 0 说明采集侧还没数据（M2 未启用 / 从机离线）。
       此时不要当成"0℃"去算（0℃ 也会走到 duty=0，结果一样），
       但【超温保护】必须跳过 —— 否则"没数据"会被误判成"没超温"，恰好反过来。 */
    if ((snap.temperature <= 0) && (snap.ts_temp == 0u))
    {
        return;    /* 从来没有采集过温度，不做任何判断 */
    }

    /* ---------- ① 超温保护：>30℃ 切断继电器 ----------
       ⚠️ 如实评估：这条路径挂在 monitor 的 1 s 周期上，实际动作延迟是
          【一个 monitor 周期 + 一个 Modbus 轮询周期】（约 1.1 s），
          不是"10 ms 内切断"。要真做 10 ms 必须 GPIO 直控继电器，不经 Modbus。
          简历上只能写"采集周期内触发保护"，不能写 10 ms。 */
    if (snap.temperature >= FAN_T_CUTOFF)
    {
        if (!s_cutoff)
        {
            s_cutoff = 1u;
            printf("[fan] !! over-temp %d.%dC >= %d.%dC, relay cut off\r\n",
                   snap.temperature / 10, snap.temperature % 10,
                   FAN_T_CUTOFF / 10, FAN_T_CUTOFF % 10);
            /* ⭐ 只写数据池目标位 + 置脏位，绝不在这里发 Modbus 帧 ——
               锁内不做慢操作，由 mbPoll 任务捎带下发（见 m2_master.c 说明） */
            M4_PoolSetRelayTarget(0u);
        }
    }
    else if (s_cutoff && (snap.temperature < (FAN_T_CUTOFF - FAN_T_HYST)))
    {
        /* 滞回：降到 28℃ 以下才解除保护（否则会在 30℃ 抖动处反复切） */
        s_cutoff = 0u;
        printf("[fan] over-temp cleared\r\n");
    }

    /* ---------- ② 温度 → 占空比（线性映射） ---------- */
    if (snap.temperature <= FAN_T_START)
    {
        duty = 0u;                                    /* 凉快 → 停 */
    }
    else if (snap.temperature >= FAN_T_FULL)
    {
        duty = FAN_DUTY_MAX;                          /* 热 → 全速 */
    }
    else
    {
        /* 26.0~34.0℃ 线性映射到 25%~100% */
        uint32_t span_t = (uint32_t)(FAN_T_FULL - FAN_T_START);    /* 80 */
        uint32_t span_d = (uint32_t)(FAN_DUTY_MAX - FAN_DUTY_MIN); /* 75 */
        uint32_t over   = (uint32_t)(snap.temperature - FAN_T_START);

        duty = (uint8_t)(FAN_DUTY_MIN + (over * span_d) / span_t);
    }

    /* ---------- ③ ⭐ 滞回：防止在阈值附近反复启停 ----------
       没有滞回时，温度在 26.0℃ 附近抖动会让风扇"转一下停一下"，
       机械和电气寿命急剧下降 —— 这就是工业控制里的"回差/死区"。 */
    if (s_running && (snap.temperature < (FAN_T_START - FAN_T_HYST)))
    {
        s_running = 0u;       /* 已经转着 → 要降到 24.0℃ 才停 */
    }
    else if (!s_running && (snap.temperature > FAN_T_START))
    {
        s_running = 1u;       /* 停着 → 要超过 26.0℃ 才启动 */
    }

    if (!s_running) duty = 0u;

    /* ⭐⭐ 运行态翻转单独打一行（M5-g 验收重点）
       ⚠️ 为什么不能只看 duty：滞回的语义是"什么时候启 / 什么时候停"。
          duty=0 既可能是"冷机没启动"也可能是"启动了但温度落到线性段以下"，
          光看 duty 区分不出来。打印 START/STOP 才能证明
          【26.0 启 / 24.0 停】这 2.0℃ 回差真的生效了。 */
    if (s_running != s_prev_run)
    {
        int16_t t = snap.temperature;

        s_prev_run = s_running;
        printf("[fan] fan %s at T=%ld.%ldC (start %d.%dC / stop %d.%dC)\r\n",
               s_running ? "START" : "STOP",
               (long)(t / 10), (long)(t % 10),
               (int)(FAN_T_START / 10),  (int)(FAN_T_START % 10),
               (int)((FAN_T_START - FAN_T_HYST) / 10),
               (int)((FAN_T_START - FAN_T_HYST) % 10));
    }

    /* ---------- ④ 变化超过 2% 才真正写寄存器 ----------
       ⚠️ 为什么要有死区：温度在 26.5℃ 抖动 0.1℃ 时 duty 会在 25/26 之间跳，
          每次都写 CCR 毫无意义，还会在日志里制造噪声。 */
    if ((duty > s_duty + 2u) || (duty + 2u < s_duty))
    {
        int16_t t = snap.temperature;

        s_duty = duty;
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1,
                              (uint32_t)duty * (FAN_ARR + 1u) / 100u);

        /* ⭐ M5-g 验收用：占空比一变化就打一行。
           【没有风扇也能验完整条曲线】—— 串口看 duty 随温度线性变化，
           再用万用表直流档量 PA6 对地电压（≈ 3.3V × duty）交叉验证即可。

           ⚠️ 为什么用整数拆分而不用 %f：
              ARMCC5 一旦用到浮点格式化会把整套软浮点库链进来（+数 KB），
              而且 monitor 栈只有 1 KB，sprintf 浮点很容易把它压爆。
              温度是 0.1℃ 单位的 int16_t，手工拆整数/小数位最稳。 */
        printf("[fan] duty=%u%%  T=%ld.%ldC  (PA6 avg %lu.%luV)\r\n",
               (unsigned)duty,
               (long)(t / 10),
               (long)((t < 0) ? (-(t % 10)) : (t % 10)),
               (unsigned long)(((uint32_t)duty * 33u) / 1000u),
               (unsigned long)((((uint32_t)duty * 33u) / 100u) % 10u));
    }
}

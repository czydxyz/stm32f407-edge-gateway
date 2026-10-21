/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    m1_net.c
  * @brief   M1 阶段应用层（M4 已升级为真正的业务任务）
  *
  * M1 时的形态：
  *   1. echo server      —— 收什么回什么（只验证下行通道）
  *   2. heartbeat client —— 发 "HB,seq,uptime"（只验证上行通道）
  *   3. link monitor     —— 5s 打印一次状态
  *
  * M4 升级后：
  *   1. m1_cmd_task    （TCP Server 5000，下行）
  *        上位机连进来发【应用层协议帧】→ 解析 → 写数据池控制区 → 回 ACK/NAK
  *        支持：0x03 查询数据池（回 0x81 快照帧）、0x02 设置继电器（置脏位）
  *   2. m1_report_task （TCP Client 5001，上行）
  *        每 3s 把【数据池快照】打包成协议帧主动上报；9s 没回应判断线重连
  *   3. （原 link monitor 已并入 M4 的 m4_mon 监控任务，这里删除）
  *
  * ⚠️ 两条铁律（M4 最重要的设计约束）：
  *   - 网络任务【绝不直接发 Modbus 帧】：RS485 半双工，会和轮询撞车。
  *     只写数据池控制区（置脏位），由 mbPoll 在下一个 100ms 节拍捎带下发。
  *   - 锁内【不做慢操作】：取快照用 M4_PoolGetSnapshot（一次 memcpy），
  *     取完立刻解锁再 netconn_write。
  ******************************************************************************
  */
/* USER CODE END Header */

#include "m1_net.h"
#include "m5_link.h"      /* M5：链路抽象层（主备切换 + 全局 seq） */
#include "m5_ota.h"       /* M5：OTA 分块下载 */
#include "m6_store.h"     /* M6：断网期间的掉电补传缓存（W25Q128） */

#include <stdio.h>
#include <string.h>

#include "cmsis_os.h"
#include "main.h"

#include "lwip/opt.h"
#include "lwip/api.h"        /* netconn / netbuf 全部在这里 */
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

/* M4 依赖 */
#include "m4_pool.h"
#include "m4_proto.h"
#include "m4_task.h"
#include "m2_master.h"       /* M2_SetRelayTarget（只置脏位） */

/* -------------------------------------------------------------------------- */
/* 上报格式开关                                                                */
/*  0 = 二进制协议帧（正式形态，配合上位机软件）                                */
/*  1 = ASCII 文本 "HB,seq,uptime"（用 NetAssist 手工观察时用这个）            */
/* -------------------------------------------------------------------------- */
#ifndef M1_REPORT_ASCII
/* TODO(M4 验收)：用 m4_host_test.py --listen 验完二进制上报后，
   如果要回到 NetAssist 手工看文本，改回 1 重新编译。 */
#define M1_REPORT_ASCII     0
#endif

/* -------------------------------------------------------------------------- */
/* lwip.c 里定义的全局网卡结构体，这里借来判链路状态                          */
/* -------------------------------------------------------------------------- */
extern struct netif gnetif;

/* M5：OTA 收完后的复位标志（延时在命令处理末尾统一做，确保 ACK 先发出去） */
static uint8_t s_ota_reboot_pending = 0u;

/* M5：上报任务当前持有的以太网连接。
   ⚠️ 为什么要做成文件级 static：链路抽象层 m5_link 只认一个函数指针
      （M5_LinkRegisterEthSend 注册的回调），它不认识 netconn。
      所以把 conn 放在这里，让回调能拿到它；断线时必须【同步置 NULL】，
      否则回调会拿着一个已 delete 的 netconn 去写 → 内存踩踏。 */
static struct netconn *s_rpt_conn = NULL;

/* M5：主链路（以太网）的发送实现，注册给 m5_link */
static int m1_eth_send(const uint8_t *data, uint16_t len)
{
  if (s_rpt_conn == NULL) return -1;   /* 没连上 = 发送失败，交给滞回计数 */

  return (netconn_write(s_rpt_conn, data, (size_t)len, NETCONN_COPY) == ERR_OK) ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* 内部小工具                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief 阻塞等待网卡真正可用
 *
 * 为什么必须有这一步：
 *   MX_LWIP_Init() 是在 defaultTask 里调用的（见 freertos.c），而这两个任务
 *   是在 MX_FREERTOS_Init() 里创建的 —— 创建完立刻就跑起来了，很可能比
 *   defaultTask 先得到 CPU。这时 lwIP 还没初始化，netconn_bind() 会直接
 *   拿到一个无效 netif（常见现象是 bind 返回 ERR_VAL 或硬 fault）。
 *
 *   注意要判两个标志，它们不是一回事：
 *     netif_is_up()      —— 软件上把网卡"打开"了
 *     netif_is_link_up() —— 物理链路真正通了（读到 PHY 的 Link 位后才置位）
 *   网线没插时，前者为真、后者为假。必须两个都为真才能开始通信。
 */
static void m1_wait_link_ready(void)
{
  while (!(netif_is_up(&gnetif) && netif_is_link_up(&gnetif)))
  {
    osDelay(200);
  }
  printf("[net] link ready\r\n");
}

/* ========================================================================== */
/* 命令分发：解析出一帧后干什么                                                */
/* ========================================================================== */

/**
 * @brief 处理一条已校验通过的命令，并回一帧响应
 *
 * ⚠️ 这里所有分支都遵守"只写数据池、不碰外设"的原则。
 *    真正的下发动作由 mbPoll 在下一个节拍做（见 m2_master.c）。
 */
static void m1_handle_cmd(struct netconn *conn, uint8_t cmd, const m4_proto_ctx_t *ctx)
{
  uint8_t tx[M4_TX_MAX_FRAME];
  int     n = -1;

  /* ⭐ 响应类帧过滤（2026-10-21 实测发现的坑，BUG-055）
     0x81 SNAPSHOT / 0x82 ACK / 0xFF NAK 全都是【板 → PC】方向的帧。
     但 5001 上报连接是**双向**的：上位机收到快照后回一个 ACK，
     这个 ACK 会顺着同一条连接回到板子，被喂进解析状态机 → 当成"上位机下发的命令"。

     后果（实测全中）：
       - 打 `[cmd] unsupported cmd 0x82`
       - 计一次坏帧 `M4_PoolAddCmd(1)` → 每上报一帧 cmd_bad 就 +1
       - 还回一个 NAK 给上位机 → 上位机侧看到满屏 NAK
     → 坏帧率统计彻底失真（实测 cmd 23 / bad 15，几乎全是 ACK 贡献的）。

     修法：响应类帧直接忽略，不当命令、不计坏帧、不回 NAK。 */
  if ((cmd == M4_CMD_SNAPSHOT) || (cmd == M4_CMD_ACK) || (cmd == M4_CMD_NAK))
  {
    (void)conn;
    (void)ctx;
    return;
  }

  switch (cmd)
  {
  case M4_CMD_QUERY:
    /* 查询数据池 → 回 0x81 快照帧（内部会加锁取快照，取完立刻解锁） */
    n = M4_ProtoPackSnapshot(tx, sizeof(tx));
    M4_PoolAddCmd(0);
    break;

  case M4_CMD_SET_RELAY:
    {
      uint8_t bmp = 0u;

      if (M4_ProtoParseSetRelay(ctx, &bmp) == 0)
      {
        /* ⭐ 只置目标 + 脏位。绝在这里发 Modbus 帧 —— 半双工总线会被撞乱 */
        M2_SetRelayTarget(bmp);
        printf("[cmd] relay target=0x%02X (dirty set, sent by poll task)\r\n", bmp);
        n = M4_ProtoPackAck(tx, sizeof(tx));
        M4_PoolAddCmd(0);
      }
      else
      {
        n = M4_ProtoPackNak(M4_ERR_BADPARAM, tx, sizeof(tx));
        M4_PoolAddCmd(1);
      }
    }
    break;

  /* ======================================================================
     M5 · OTA（复用 M4 的帧格式，只加 3 个 CMD 分支 —— 这就是当初把
     LEN/CMD/CRC32 设计成通用格式的收益：粘包、CRC、每连接状态机全现成）
     ====================================================================== */
  case M4_CMD_OTA_BEGIN:
    n = M5_OtaBegin(ctx->data, ctx->data_len, tx, sizeof(tx));
    M4_PoolAddCmd(0);
    break;

  case M4_CMD_OTA_DATA:
    n = M5_OtaData(ctx->data, ctx->data_len, tx, sizeof(tx));
    M4_PoolAddCmd(0);
    break;

  case M4_CMD_OTA_END:
    n = M5_OtaEnd(ctx->data, ctx->data_len, tx, sizeof(tx));
    M4_PoolAddCmd(0);
    /* ⭐ 收完且整体 CRC 通过 → 复位，让 Bootloader 去裁决切换。
       复位必须等 ACK 发出去之后做，否则上位机收不到最后一帧应答。 */
    s_ota_reboot_pending = 1u;
    break;

  default:
    printf("[cmd] unsupported cmd 0x%02X\r\n", cmd);
    n = M4_ProtoPackNak(M4_ERR_UNSUPPORTED, tx, sizeof(tx));
    M4_PoolAddCmd(1);
    break;
  }

  if (n > 0)
  {
    netconn_write(conn, tx, (size_t)n, NETCONN_COPY);
  }

  /* OTA 完成 → 延时 500 ms 让上面那帧 ACK 真正发出去，再复位 */
  if (s_ota_reboot_pending)
  {
    osDelay(500);
    printf("[sys] OTA ready -> NVIC_SystemReset()\r\n");
    osDelay(50);
    NVIC_SystemReset();
  }
}

/* ========================================================================== */
/* 任务 1：命令服务（TCP Server 5000）—— 上位机 → 板子                        */
/* ========================================================================== */

static void m1_cmd_task(void *argument)
{
  struct netconn *listen_conn = NULL;
  struct netconn *conn        = NULL;
  err_t err;

  /* ⚠️ 每条连接必须有【自己】的解析上下文。
     用全局 static 的话，多条连接的数据会互相污染状态机，解出乱码帧。
     这里是单连接（accept 后只服务一个客户端），所以一个全局 ctx 够用，
     但每次 accept 都要 M4_ProtoInit 复位一次 —— 否则会带着上一条连接的
     半截状态继续解析。 */
  static m4_proto_ctx_t cmd_ctx;

  (void)argument;

  m1_wait_link_ready();

  listen_conn = netconn_new(NETCONN_TCP);
  if (listen_conn == NULL)
  {
    printf("[cmd] netconn_new failed (MEMP_NUM_NETCONN too small?)\r\n");
    for (;;) osDelay(1000);
  }

  err = netconn_bind(listen_conn, NULL, M1_ECHO_PORT);
  if (err != ERR_OK)
  {
    printf("[cmd] bind failed (%d)\r\n", (int)err);
    netconn_delete(listen_conn);
    for (;;) osDelay(1000);
  }

  err = netconn_listen(listen_conn);
  if (err != ERR_OK)
  {
    printf("[cmd] listen failed (%d)\r\n", (int)err);
    netconn_delete(listen_conn);
    for (;;) osDelay(1000);
  }

  printf("[cmd] listening on port %d\r\n", M1_ECHO_PORT);

  /* ⭐ 给监听 socket 也设 3s 接收超时 —— 对 accept 同样生效。
     不设的话 netconn_accept() 会永久阻塞，没有客户端时循环体一次都跑不到，
     存活位永远置不上（M5 喂狗时会误判"网络任务死了"）。实测已确认此坑。 */
  netconn_set_recvtimeout(listen_conn, 3000);

  for (;;)
  {
    /* 存活位放在循环开头：只要本任务还在转，就说明协议栈和本任务都活着。 */
    M4_AliveSet(M4_ALIVE_NET);

    /* accept 是阻塞的：没有客户端连进来就一直挂起，不占 CPU。
       但因为上面设了 3s 超时，最多挂 3s 就会返回 ERR_TIMEOUT。 */
    err = netconn_accept(listen_conn, &conn);
    if (err != ERR_OK)
    {
      /* ERR_TIMEOUT = 没人来连，正常，继续等（不要 close listen_conn！） */
      if (err != ERR_TIMEOUT)
      {
        osDelay(10);
      }
      continue;
    }

    printf("[cmd] client connected\r\n");
    M4_ProtoInit(&cmd_ctx);

    netconn_set_recvtimeout(conn, 3000);

    for (;;)
    {
      struct netbuf *nb = NULL;
      void *data;
      u16_t len;

      err = netconn_recv(conn, &nb);

      if (err == ERR_OK)
      {
        /* 一个 netbuf 内部可能是链式 pbuf，必须 do...while 遍历，
           不能写成 if —— 否则大数据包只处理第一段。 */
        do
        {
          uint16_t i;
          uint8_t *p;

          netbuf_data(nb, &data, &len);
          p = (uint8_t *)data;

          /* ⭐ 逐字节喂状态机。TCP 是字节流，粘包/拆包全靠它恢复帧边界 */
          for (i = 0; i < len; i++)
          {
            int rc = M4_ProtoFeed(&cmd_ctx, p[i]);

            if (rc > 0)
            {
              m1_handle_cmd(conn, (uint8_t)rc, &cmd_ctx);
            }
            else if (rc == -1)
            {
              uint8_t tx[M4_TX_MAX_FRAME];
              int n;

              printf("[cmd] crc error, frame dropped\r\n");
              M4_PoolAddCmd(1);
              n = M4_ProtoPackNak(M4_ERR_BADCRC, tx, sizeof(tx));
              if (n > 0) netconn_write(conn, tx, (size_t)n, NETCONN_COPY);
            }
            else if (rc == -2)
            {
              uint8_t tx[M4_TX_MAX_FRAME];
              int n;

              printf("[cmd] bad length, frame dropped\r\n");
              M4_PoolAddCmd(1);
              n = M4_ProtoPackNak(M4_ERR_BADLEN, tx, sizeof(tx));
              if (n > 0) netconn_write(conn, tx, (size_t)n, NETCONN_COPY);
            }
            /* rc == 0：还在收，继续 */
          }
        } while (netbuf_next(nb) >= 0);

        netbuf_delete(nb);   /* 用完必须释放，否则 pbuf 池会被慢慢吃光 */
      }
      else if (err == ERR_TIMEOUT)
      {
        /* 3s 没数据，正常。继续等，不要当成断线 */
        M4_AliveSet(M4_ALIVE_NET);
        continue;
      }
      else
      {
        printf("[cmd] recv err %d\r\n", (int)err);
        break;
      }

      M4_AliveSet(M4_ALIVE_NET);
    }

    /* ---- 连接结束：close + delete 两步都不能少 ----
       ⚠️ 只 close 不 delete，PCB 不会还给 MEMP 池。MEMP_NUM_TCP_PCB 只有 10，
       反复连断 10 次以后整块板子再也连不上，而且现象是"ping 通但连不上"。 */
    netconn_close(conn);
    netconn_delete(conn);
    conn = NULL;
    printf("[cmd] client disconnected\r\n");
  }
}

/* ========================================================================== */
/* 任务 2：周期上报（TCP Client 5001）—— 板子 → 上位机                        */
/* ========================================================================== */

/*==============================================================================
 * M6 · 断网补传（W25Q128 缓存）
 *
 * ⭐ 完整闭环是三步，缺一步就是假实现：
 *   ① 存 —— DEGRADED 期间发送失败，把精简数据写进 W25Q128
 *   ② 补 —— 链路恢复后，逐条取出来用【原 seq】重新组帧发出去
 *   ③ 删 —— 只有【发送成功】才 pop
 *
 * ⚠️ 最容易写错的是 ③：很多人"取出来就删"，结果链路又断 → 数据永久丢失。
 *    正确顺序是【发出去 → 确认成功 → 才删】，和 TCP 的"收到 ACK 才清发送缓冲"
 *    是同一个道理。失败就啥也不做，下轮自然重试同一条（Peek 不删）。
 *============================================================================*/

/* ① 存：把当前数据池的采集值缓存一条 */
static void m1_cache_current(void)
{
    m4_pool_t snap;
    m6_item_t it;

    if (!M6_StoreReady()) return;

    M4_PoolGetSnapshot(&snap);

    it.seq          = M5_UpSeqPeek();   /* ⭐ 存"当前该用的序号"，补传时沿用 */
    it.uptime_s     = snap.uptime_s;
    it.temperature  = snap.temperature;
    it.humidity     = snap.humidity;
    it.di_status    = snap.di_status;
    it.relay_status = snap.relay_status;

    if (M6_StorePush(&it) == 0)
    {
        printf("[m6] cached seq=%lu (now %u/%u, dropped %lu)\r\n",
               (unsigned long)it.seq,
               (unsigned)M6_StoreCount(), (unsigned)M6_SLOT_MAX,
               (unsigned long)M6_StoreDropped());
    }
}

/* ②+③ 补一条并在成功后删除。返回 1 = 补了一条（调用方应让出时间片） */
static int m1_replay_one(void)
{
    m6_item_t it;
    m4_pool_t snap;
    uint8_t   tx[M4_TX_MAX_FRAME];
    uint32_t  age_s;
    int       n;
    int       rc;

    if (!M6_StoreReady())    return 0;
    if (M6_StoreCount() == 0u) return 0;
    if (M6_StorePeek(&it) != 0) return 0;

    /* 这条数据有多老：用 uptime 差算（缓存里存的是采集时刻） */
    M4_PoolGetSnapshot(&snap);
    age_s = (snap.uptime_s >= it.uptime_s) ? (snap.uptime_s - it.uptime_s) : 0u;

    n = M4_ProtoPackSnapshotEx(tx, sizeof(tx), it.seq,
                               it.temperature, it.humidity,
                               it.di_status, it.relay_status,
                               it.uptime_s, age_s);
    if (n <= 0) return 0;

    rc = M5_LinkSend(tx, (uint16_t)n);
    M5_LinkReportResult((rc == 0) ? 1 : 0);

    /* ⚠️ 这里【不】调 M5_UpSeqCommit：
       补传的是历史序号，不能推进全局单调序号，
       否则当前实时数据的 seq 会被补传带跑偏。 */

    if (rc != 0)
    {
        return 0;      /* 没发出去 —— 不 pop，下轮重试同一条 */
    }

    /* ⭐ 成功了才删 */
    (void)M6_StorePop();
    printf("[m6] replay seq=%lu (age %lus, T=%d.%dC, %u left)\r\n",
           (unsigned long)it.seq, (unsigned long)age_s,
           (int)(it.temperature / 10), (int)(it.temperature % 10),
           (unsigned)M6_StoreCount());
    return 1;
}

static void m1_report_task(void *argument)
{
  struct netconn *conn = NULL;
  ip_addr_t server_ip;
  uint32_t seq = 0;
  uint32_t retry_log_mask = 0;   /* 重连日志节流，避免刷屏 */

  /* 上行连接也要能收指令（上位机可以在这条连接上下发），所以也要一个 ctx */
  static m4_proto_ctx_t rep_ctx;

  (void)argument;

  m1_wait_link_ready();

  IP4_ADDR(&server_ip, M1_PC_IP0, M1_PC_IP1, M1_PC_IP2, M1_PC_IP3);

  for (;;)
  {
    /* ---- 链路状态写进数据池（上位机能看到"板子认为的链路状态"）---- */
    M4_PoolSetEthLink(netif_is_link_up(&gnetif) ? 1u : 0u);

    /* ⭐ 存活位置在循环开头：本任务还在转 = 活着。
       即使 PC 没开 5001 服务（connect 一直失败），任务本身是健康的，
       不能因为连不上就不置位 —— 否则 M5 事件组永远等不到 NET，会误复位。
       （实测：PC 没开服务时 monitor 一直报 "alive missing NET"，即此坑。） */
    M4_AliveSet(M4_ALIVE_NET);

    /* ---------- 1. 没有连接就先连 ---------- */
    if (conn == NULL)
    {
      conn = netconn_new(NETCONN_TCP);
      if (conn == NULL)
      {
        printf("[rpt] netconn_new failed\r\n");
        osDelay(1000);
        continue;
      }

      netconn_set_recvtimeout(conn, M1_HB_TIMEOUT_MS);

      if (netconn_connect(conn, &server_ip, M1_HB_PORT) != ERR_OK)
      {
        /* ⚠️ 必须先 delete 再置 NULL，直接置 NULL 就泄漏一个 netconn + 一个 PCB */
        netconn_delete(conn);
        conn        = NULL;
        s_rpt_conn  = NULL;

        if ((retry_log_mask++ % 10u) == 0u)
        {
          printf("[rpt] connect failed, retry...\r\n");
        }
        osDelay(1000);
        continue;
      }

      retry_log_mask = 0;
      s_rpt_conn     = conn;      /* ⭐ 交给链路抽象层的以太网发送回调 */
      M4_ProtoInit(&rep_ctx);
      printf("[rpt] connected to %d.%d.%d.%d:%d\r\n",
             M1_PC_IP0, M1_PC_IP1, M1_PC_IP2, M1_PC_IP3, M1_HB_PORT);
    }

    /* ---------- 2. 发一帧上报 ---------- */
    {
      uint8_t tx[M4_TX_MAX_FRAME];
      int     n;

#if M1_REPORT_ASCII
        /* 调试用：发人能看懂的文本，方便 NetAssist 观察 */
        {
          char   buf[96];
          m4_pool_t snap;

          (void)tx;   /* ASCII 模式下二进制帧缓冲用不到，显式消除 unused 警告 */

        M4_PoolGetSnapshot(&snap);
        n = snprintf(buf, sizeof(buf), "HB,%lu,%lu,T=%d,H=%d,DI=%d,RL=%d\r\n",
                     (unsigned long)seq,
                     (unsigned long)(osKernelGetTickCount() / 1000u),
                     snap.temperature, snap.humidity,
                     snap.di_status, snap.relay_status);
        if (netconn_write(conn, buf, (size_t)n, NETCONN_COPY) != ERR_OK)
        {
          printf("[rpt] send failed, reconnect\r\n");
          netconn_close(conn);
          netconn_delete(conn);
          conn        = NULL;
          s_rpt_conn  = NULL;
          osDelay(1000);
          continue;
        }
      }
#else
      /* ⭐⭐ M6：补传优先 —— 先把断网期间缓存的历史数据吐出去，再报当前值。
         ⚠️ 为什么要"优先"：缓存区只有 256 条，而 DEGRADED 期间数据还在产生。
            如果先报当前的、慢慢再补传，很可能补到一半缓存又被新数据覆盖，
            结果时序上出现空洞。先补旧的 = 保证时间线连续。
         ⚠️ 每轮只补一条：补传和正常上报抢同一条链路，一次塞几十条会
            把当前实时性拖垮（备份链路上报周期可是 30 s 一条）。 */
      if (m1_replay_one() == 1)
      {
        osDelay(200);      /* 补了一条，让出一点时间，下轮继续补 */
        continue;
      }

      n = M4_ProtoPackSnapshot(tx, sizeof(tx));
      if (n <= 0)
      {
        printf("[rpt] pack failed\r\n");
        osDelay(1000);
        continue;
      }

      /* ⭐ M5：改走链路抽象层 —— 业务代码不再关心"现在是以太网还是 WiFi"
         主链路：内部回调到 m1_eth_send()（用 s_rpt_conn）
         备份链路：走 ESP8266 的 AT+CIPSEND
         ⛔ 不要在这里直接 netconn_write()，否则链路切换等于没做。 */
      {
        int rc = M5_LinkSend(tx, (uint16_t)n);

        /* 结果回给链路层做滞回计数（连续失败 N 次 → 切备份） */
        M5_LinkReportResult((rc == 0) ? 1 : 0);

        /* ⭐ 全局单调 seq：只有发送成功才递增。
           失败就用同一个号重发 → 上位机按 seq 去重，重复无害、缺失可补。 */
        M5_UpSeqCommit((rc == 0) ? 1 : 0);

        /* ⭐⭐ M6 ① 存：两条链路都不可用（DEGRADED）时才缓存。
           ⚠️ 为什么限定 DEGRADED：主链路偶发一次失败会立刻重连并成功，
              那种情况缓存纯属浪费槽位。真正的"数据没人接"是 DEGRADED。
           ⚠️ 即使判断失误多缓存了一条也无害 —— 补传时用【原 seq】，
              上位机按 seq 去重，重复那条会被直接丢弃。这就是带 seq 的好处。 */
        if ((rc != 0) && (M5_LinkCurrent() == M5_LINK_DEGRADED))
        {
          m1_cache_current();
        }

        if ((rc != 0) && (M5_LinkCurrent() == M5_LINK_PRIMARY))
        {
          printf("[rpt] send failed, reconnect\r\n");
          netconn_close(conn);
          netconn_delete(conn);
          conn        = NULL;
          s_rpt_conn  = NULL;      /* ⚠️ 必须同步清，否则回调会写已释放的 netconn */
          osDelay(1000);
          continue;
        }
      }
#endif
      (void)seq;
    }

    /* ---------- 3. 等上位机回应 ---------- */
    {
      struct netbuf *nb = NULL;
      void *data;
      u16_t len;
      err_t err;

      err = netconn_recv(conn, &nb);

      if (err == ERR_OK)
      {
        do
        {
          uint16_t i;
          uint8_t *p;

          netbuf_data(nb, &data, &len);
          p = (uint8_t *)data;

          printf("[rpt] rx %u bytes\r\n", (unsigned)len);

          for (i = 0; i < len; i++)
          {
            int rc = M4_ProtoFeed(&rep_ctx, p[i]);

            if (rc > 0)
            {
              /* 上位机可以在这条连接上下发指令，处理方式与 5000 端口完全一致 */
              m1_handle_cmd(conn, (uint8_t)rc, &rep_ctx);
            }
            else if (rc < 0)
            {
              M4_PoolAddCmd(1);
            }
          }
        } while (netbuf_next(nb) >= 0);

        netbuf_delete(nb);
      }
      else
      {
        /* ERR_TIMEOUT = 9s 没等到回应 → 判定链路已断；其余 = 对端主动断 */
        printf("[rpt] link lost (err %d), reconnect\r\n", (int)err);
        netconn_close(conn);
        netconn_delete(conn);
        conn        = NULL;
        s_rpt_conn  = NULL;
        osDelay(1000);
        continue;
      }
    }

    M4_AliveSet(M4_ALIVE_NET);

    /* ⭐ M5：上报周期由链路层决定 —— 这就是"降级"的落点。
         主链路 3 s 一帧；切到 ESP8266 备份后自动拉长到 30 s，
         因为备份链路有效吞吐比以太网低 3 个数量级，按原速率会堵死。 */
    osDelay(M5_LinkReportInterval());
  }
}

/* ========================================================================== */
/* 对外入口：创建上面两个任务                                                 */
/* ========================================================================== */

void m1_net_start(void)
{
  osThreadId_t tid;

  /* ⭐ M5：把以太网的发送实现注册给链路抽象层。
     这样 m5_link 不需要知道 netconn 的存在，两条链路对业务层是同一张脸。 */
  M5_LinkRegisterEthSend(m1_eth_send);

  /* 栈大小单位是字节（CMSIS-RTOS v2 规定）。
     ⭐ M4 起 printf 经 m4_log 的环形缓冲，栈消耗从"几百字节 × 嵌套深度"
        降到"一次 memcpy"，所以这里从 2KB 降到 1280 是安全的。
        剩下的大头是 lwIP netconn 内部调用链（约 600~800 字节）。

     ⚠️ 2026-10-21 实测栈水线（HighWaterMark）：
          m1_cmd  left=928B（首轮）→ **456B**（稳定后）
          m1_rpt  left=480B
        说明 1280B 栈在"连接 + 收帧 + 协议解析 + 打包快照"这条最深路径上
        只剩 456B（约 35%）。M5 还要加链路冗余/重传逻辑，余量不够，
        这里统一提到 1536B（各 +256B，heap 从 8696 降到约 8184，仍安全）。 */
  static const osThreadAttr_t cmd_attr = {
    .name       = "m1_cmd",
    .stack_size = 1536u,
    .priority   = (osPriority_t)osPriorityAboveNormal,   /* 响应上位机指令，优先级最高 */
  };
  static const osThreadAttr_t rpt_attr = {
    .name       = "m1_rpt",
    .stack_size = 1536u,
    .priority   = (osPriority_t)osPriorityNormal,
  };

  tid = osThreadNew(m1_cmd_task, NULL, &cmd_attr);
  if (tid == NULL) printf("[net] create cmd task FAILED\r\n");
  else             M4_TaskRegister("m1_cmd", (TaskHandle_t)tid);

  tid = osThreadNew(m1_report_task, NULL, &rpt_attr);
  if (tid == NULL) printf("[net] create report task FAILED\r\n");
  else             M4_TaskRegister("m1_rpt", (TaskHandle_t)tid);
}

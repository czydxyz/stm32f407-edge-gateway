/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : ethernetif.c
  * Description        : This file provides code for the configuration
  *                      of the ethernetif.c MiddleWare.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "lwip/opt.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "lwip/ethip6.h"
#include "ethernetif.h"
/* USER CODE BEGIN Include for User BSP */
#include <stdio.h>      /* ETH_PHY_SCAN 打开时 printf 需要 */
/* USER CODE END Include for User BSP */
#include <string.h>
#include "cmsis_os.h"
#include "lwip/tcpip.h"

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */

/* ===== 边缘网关：板载 PHY 为 YT8512C（裕太微），不是 CubeMX 默认的 LAN8742 ===== */

/* 【2026-10-18 已实测确认】
   扫描结果：只有 addr=0 应答，其余 31 个地址全是 1FFF/3FFF/FFFF（MDIO 上拉垃圾）。
     a= 0  BMCR=1140  BSR=7949  ID1=0000  ID2=0128
   ID = 0x0000_0128 → Linux motorcomm 驱动里的 PHY_ID_YT8512，确认就是 YT8512。
   BMCR=0x1140 = bit12(AN enable) + bit8(全双工) + bit6(100M) → 复位默认值正常。
   BSR =0x7949 → bit2(Link)=0，因为当时没插网线；插上后应变成 7969/786D 之类（bit2=1）。
   => PHY 地址 = 0，与之前填的一致，无需修改。 */
#define ETH_PHY_ADDR    0U

/* 置 1 可通过串口打印扫描到的 PHY 地址（需在别处实现 printf 重定向）。
   地址已确认，改回 0 以避免每次上电多花 32 次 MDIO 读的时间。 */
#define ETH_PHY_SCAN    0

/* USER CODE END 0 */

/* Private define ------------------------------------------------------------*/
/* The time to block waiting for input. */
#define TIME_WAITING_FOR_INPUT ( portMAX_DELAY )
/* Time to block waiting for transmissions to finish */
#define ETHIF_TX_TIMEOUT (2000U)
/* USER CODE BEGIN OS_THREAD_STACK_SIZE_WITH_RTOS */
/* Stack size of the interface thread */
#define INTERFACE_THREAD_STACK_SIZE ( 350 )
/* USER CODE END OS_THREAD_STACK_SIZE_WITH_RTOS */
/* Network interface name */
#define IFNAME0 's'
#define IFNAME1 't'

/* ETH Setting  */
#define ETH_DMA_TRANSMIT_TIMEOUT               ( 20U )
#define ETH_TX_BUFFER_MAX             ((ETH_TX_DESC_CNT) * 2U)

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/* Private variables ---------------------------------------------------------*/
/*
@Note: This interface is implemented to operate in zero-copy mode only:
        - Rx Buffers will be allocated from LwIP stack Rx memory pool,
          then passed to ETH HAL driver.
        - Tx Buffers will be allocated from LwIP stack memory heap,
          then passed to ETH HAL driver.

@Notes:
  1.a. ETH DMA Rx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_RX_DESC_CNT in ETH GUI (Rx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h
  1.b. ETH DMA Tx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_TX_DESC_CNT in ETH GUI (Tx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h

  2.a. Rx Buffers number must be between ETH_RX_DESC_CNT and 2*ETH_RX_DESC_CNT
  2.b. Rx Buffers must have the same size: ETH_RX_BUF_SIZE, this value must
       passed to ETH DMA in the init field (heth.Init.RxBuffLen)
  2.c  The RX Ruffers addresses and sizes must be properly defined to be aligned
       to L1-CACHE line size (32 bytes).
*/

/* Data Type Definitions */
typedef enum
{
  RX_ALLOC_OK       = 0x00,
  RX_ALLOC_ERROR    = 0x01
} RxAllocStatusTypeDef;

typedef struct
{
  struct pbuf_custom pbuf_custom;
  uint8_t buff[(ETH_RX_BUF_SIZE + 31) & ~31] __ALIGNED(32);
} RxBuff_t;

/* Memory Pool Declaration */
#define ETH_RX_BUFFER_CNT             12U
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

/* Variable Definitions */
static uint8_t RxAllocStatus;

ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */

osSemaphoreId RxPktSemaphore = NULL;   /* Semaphore to signal incoming packets */
osSemaphoreId TxPktSemaphore = NULL;   /* Semaphore to signal transmit packet complete */

/* Global Ethernet handle */
ETH_HandleTypeDef heth;
ETH_TxPacketConfig TxConfig;

/* Private function prototypes -----------------------------------------------*/

/* USER CODE BEGIN 3 */

/* USER CODE END 3 */

/* Private functions ---------------------------------------------------------*/
void pbuf_free_custom(struct pbuf *p);

/**
  * @brief  Ethernet Rx Transfer completed callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  osSemaphoreRelease(RxPktSemaphore);
}
/**
  * @brief  Ethernet Tx Transfer completed callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  osSemaphoreRelease(TxPktSemaphore);
}
/**
  * @brief  Ethernet DMA transfer error callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *handlerEth)
{
  if((HAL_ETH_GetDMAError(handlerEth) & ETH_DMASR_RBUS) == ETH_DMASR_RBUS)
  {
     osSemaphoreRelease(RxPktSemaphore);
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/*******************************************************************************
                       LL Driver Interface ( LwIP stack --> ETH)
*******************************************************************************/
/**
 * @brief In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void low_level_init(struct netif *netif)
{
  HAL_StatusTypeDef hal_eth_init_status = HAL_OK;
/* USER CODE BEGIN OS_THREAD_ATTR_CMSIS_RTOS_V2 */
  osThreadAttr_t attributes;
/* USER CODE END OS_THREAD_ATTR_CMSIS_RTOS_V2 */
/* USER CODE BEGIN low_level_init Variables Initialization for User BSP */

/* USER CODE END low_level_init Variables Initialization for User BSP */
  /* Start ETH HAL Init */

   uint8_t MACAddr[6] ;
  heth.Instance = ETH;
  MACAddr[0] = 0x00;
  MACAddr[1] = 0x80;
  MACAddr[2] = 0xE1;
  MACAddr[3] = 0x00;
  MACAddr[4] = 0x00;
  MACAddr[5] = 0x00;
  heth.Init.MACAddr = &MACAddr[0];
  heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  heth.Init.TxDesc = DMATxDscrTab;
  heth.Init.RxDesc = DMARxDscrTab;
  heth.Init.RxBuffLen = 1536;

  /* USER CODE BEGIN MACADDRESS */

  /* YT8512C 硬复位：PD3（板载 10K 下拉，默认低电平 = 复位中）
     拉低 >= 1ms → 释放 → 等 >= 100ms 再读 PHY 寄存器 */
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_3, GPIO_PIN_RESET);
  HAL_Delay(50);
  HAL_GPIO_WritePin(GPIOD, GPIO_PIN_3, GPIO_PIN_SET);
  HAL_Delay(200);

  /* 注意：MDIO 扫描不能放在这里 —— 此刻 HAL_ETH_Init() 还没执行，
     ETH 的时钟和 RMII 引脚都还没被 MspInit 打开，读出来全是垃圾。
     扫描已移到 HAL_ETH_Init() 成功之后的 low_level_init Code 2 段。 */

/* USER CODE END MACADDRESS */

  hal_eth_init_status = HAL_ETH_Init(&heth);

  memset(&TxConfig, 0 , sizeof(ETH_TxPacketConfig));
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

  /* End ETH HAL Init */

  /* Initialize the RX POOL */
  LWIP_MEMPOOL_INIT(RX_POOL);

#if LWIP_ARP || LWIP_ETHERNET

  /* set MAC hardware address length */
  netif->hwaddr_len = ETH_HWADDR_LEN;

  /* set MAC hardware address */
  netif->hwaddr[0] =  heth.Init.MACAddr[0];
  netif->hwaddr[1] =  heth.Init.MACAddr[1];
  netif->hwaddr[2] =  heth.Init.MACAddr[2];
  netif->hwaddr[3] =  heth.Init.MACAddr[3];
  netif->hwaddr[4] =  heth.Init.MACAddr[4];
  netif->hwaddr[5] =  heth.Init.MACAddr[5];

  /* maximum transfer unit */
  netif->mtu = ETH_MAX_PAYLOAD;

  /* Accept broadcast address and ARP traffic */
  /* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
  #if LWIP_ARP
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
  #else
    netif->flags |= NETIF_FLAG_BROADCAST;
  #endif /* LWIP_ARP */

  /* create a binary semaphore used for informing ethernetif of frame reception */
  RxPktSemaphore = osSemaphoreNew(1, 0, NULL);

  /* create a binary semaphore used for informing ethernetif of frame transmission */
  TxPktSemaphore = osSemaphoreNew(1, 0, NULL);

  /* create the task that handles the ETH_MAC */
/* USER CODE BEGIN OS_THREAD_NEW_CMSIS_RTOS_V2 */
  memset(&attributes, 0x0, sizeof(osThreadAttr_t));
  attributes.name = "EthIf";
  attributes.stack_size = INTERFACE_THREAD_STACK_SIZE;
  attributes.priority = osPriorityRealtime;
  osThreadNew(ethernetif_input, netif, &attributes);
/* USER CODE END OS_THREAD_NEW_CMSIS_RTOS_V2 */

/* USER CODE BEGIN low_level_init Code 1 for User BSP */

/* USER CODE END low_level_init Code 1 for User BSP */

  if (hal_eth_init_status == HAL_OK)
  {
/* USER CODE BEGIN low_level_init Code 2 for User BSP */

    /* 【M1 必补】F4 HAL 不设置 MDC 时钟分频（MACMIIAR 的 CR[2:0]）！
       证据：HAL_ETH_ReadPHYRegister() 里只有一行把 CR 掩码清掉（tmpreg1 &= ~CR_MASK），
             从头到尾没有一行把 CR 写成正确值。
       后果：CR[2:0] = 000，按 RM0090 是 HCLK/42 → 168MHz/42 = 4 MHz，
             而 IEEE 802.3 规定 MDC 最高 2.5MHz，超了 PHY 就不应答，
             MDIO 数据线被上拉，读出来全是 0xFFFF —— 现象和"MDIO 断了"一模一样，
             极易误判成接线或跳线问题。
       修法：F407 @168MHz 应选 CR=100（HCLK/102 ≈ 1.65MHz）。
             CR 占 MACMIIAR 的 bit2~bit4（掩码 0x1C），值 4 左移 2 位 = 0x10。 */
    heth.Instance->MACMIIAR = (heth.Instance->MACMIIAR & ~0x1CU) | (4U << 2U);

#if ETH_PHY_SCAN
    /* MDIO 地址扫描：必须在 HAL_ETH_Init() 之后（时钟与引脚已就绪）才有效。
       遍历 0~31 读 PHYIDR1(2)/PHYIDR2(3)，非 0 非 0xFFFF 即命中。
       YT8512C 的地址脚与 LED 复用，原理图读不出来，只能这样实测。
       拿到地址后回填 ETH_PHY_ADDR，并把 ETH_PHY_SCAN 改回 0。 */
    /* 不再过滤，全部打印：MDIO 通不通、通到哪个地址，一眼能看出来。
       判读方法：
         - 所有地址都是 FFFF      → MDIO 完全断开（PA2 被占用 / PHY 没出时钟）
         - 只有某几个地址有值     → 那几个里挑 BSR(1) 最低位有意义的，就是 PHY
         - st 不是 0               → HAL 返回错误/超时，MDC 时序有问题 */
    printf("[ETH] MDIO scan begin\r\n");
    for (uint8_t a = 0U; a < 32U; a++)
    {
      uint32_t r0 = 0U, r1 = 0U, r2 = 0U, r3 = 0U;
      HAL_StatusTypeDef st;

      st = HAL_ETH_ReadPHYRegister(&heth, a, 0U, &r0);   /* BMCR   */
      HAL_ETH_ReadPHYRegister(&heth, a, 1U, &r1);         /* BSR    */
      HAL_ETH_ReadPHYRegister(&heth, a, 2U, &r2);         /* PHYIDR1 */
      HAL_ETH_ReadPHYRegister(&heth, a, 3U, &r3);         /* PHYIDR2 */

      printf("[ETH] a=%2u st=%d BMCR=%04lX BSR=%04lX ID1=%04lX ID2=%04lX\r\n",
             (unsigned)a, (int)st,
             (unsigned long)r0, (unsigned long)r1,
             (unsigned long)r2, (unsigned long)r3);
    }
    printf("[ETH] MDIO scan end\r\n");
#endif /* ETH_PHY_SCAN */

    /* 【M1 必补】CubeMX 6.17 新模板没有生成启动 ETH DMA 的代码！
       HAL_ETH_Init() 只做寄存器配置与 DMA 软复位，不会置位 DMAOMR 的 ST/SR，
       也不会置位 MACCR 的 TE/RE —— 不调这一个函数，MAC 和 DMA 都停着，
       现象是：PHY 链路正常、灯也在闪，但一个包都收不到、ping 永远超时。
       Start_IT 比 Start 多做的事：把 RxDescList.ItMode 置 1，
       这样收完包才会进 ETH_IRQHandler → HAL_ETH_RxCpltCallback → 释放 RxPktSemaphore。 */
    if (HAL_ETH_Start_IT(&heth) != HAL_OK)
    {
      Error_Handler();
    }

/* USER CODE END low_level_init Code 2 for User BSP */

  }
  else
  {
    Error_Handler();
  }
#endif /* LWIP_ARP || LWIP_ETHERNET */

/* USER CODE BEGIN LOW_LEVEL_INIT */

/* USER CODE END LOW_LEVEL_INIT */
}

/**
 * @brief This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *       strange results. You might consider waiting for space in the DMA queue
 *       to become available since the stack doesn't retry to send a packet
 *       dropped because of memory failure (except for the TCP timers).
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  uint32_t i = 0U;
  struct pbuf *q = NULL;
  err_t errval = ERR_OK;
  ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT] = {0};

  memset(Txbuffer, 0 , ETH_TX_DESC_CNT*sizeof(ETH_BufferTypeDef));

  for(q = p; q != NULL; q = q->next)
  {
    if(i >= ETH_TX_DESC_CNT)
      return ERR_IF;

    Txbuffer[i].buffer = q->payload;
    Txbuffer[i].len = q->len;

    if(i>0)
    {
      Txbuffer[i-1].next = &Txbuffer[i];
    }

    if(q->next == NULL)
    {
      Txbuffer[i].next = NULL;
    }

    i++;
  }

  TxConfig.Length = p->tot_len;
  TxConfig.TxBuffer = Txbuffer;
  TxConfig.pData = p;

  pbuf_ref(p);

  do
  {
    if(HAL_ETH_Transmit_IT(&heth, &TxConfig) == HAL_OK)
    {
      errval = ERR_OK;
    }
    else
    {

      if(HAL_ETH_GetError(&heth) & HAL_ETH_ERROR_BUSY)
      {
        /* Wait for descriptors to become available */
        osSemaphoreAcquire(TxPktSemaphore, ETHIF_TX_TIMEOUT);
        HAL_ETH_ReleaseTxPacket(&heth);
        errval = ERR_BUF;
      }
      else
      {
        /* Other error */
        pbuf_free(p);
        errval =  ERR_IF;
      }
    }
  }while(errval == ERR_BUF);

  return errval;
}

/**
 * @brief Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *         NULL on memory error
   */
static struct pbuf * low_level_input(struct netif *netif)
{
  struct pbuf *p = NULL;

  if(RxAllocStatus == RX_ALLOC_OK)
  {
    HAL_ETH_ReadData(&heth, (void **)&p);
  }

  return p;
}

/**
 * @brief This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface. Then the type of the received packet is determined and
 * the appropriate input function is called.
 *
 * @param netif the lwip network interface structure for this ethernetif
 */
void ethernetif_input(void* argument)
{
  struct pbuf *p = NULL;
  struct netif *netif = (struct netif *) argument;

  for( ;; )
  {
    if (osSemaphoreAcquire(RxPktSemaphore, TIME_WAITING_FOR_INPUT) == osOK)
    {
      do
      {
        p = low_level_input( netif );
        if (p != NULL)
        {
          if (netif->input( p, netif) != ERR_OK )
          {
            pbuf_free(p);
          }
        }
      } while(p!=NULL);
    }
  }
}

#if !LWIP_ARP
/**
 * This function has to be completed by user in case of ARP OFF.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if ...
 */
static err_t low_level_output_arp_off(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
  err_t errval;
  errval = ERR_OK;

/* USER CODE BEGIN 5 */

/* USER CODE END 5 */

  return errval;

}
#endif /* LWIP_ARP */

/**
 * @brief Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  /* Initialize interface hostname */
  netif->hostname = "lwip";
#endif /* LWIP_NETIF_HOSTNAME */

  /*
   * Initialize the snmp variables and counters inside the struct netif.
   * The last argument should be replaced with your link speed, in units
   * of bits per second.
   */
  // MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  /* We directly use etharp_output() here to save a function call.
   * You can instead declare your own function an call etharp_output()
   * from it if you have to do some checks before sending (e.g. if link
   * is available...) */

#if LWIP_IPV4
#if LWIP_ARP || LWIP_ETHERNET
#if LWIP_ARP
  netif->output = etharp_output;
#else
  /* The user should write its own code in low_level_output_arp_off function */
  netif->output = low_level_output_arp_off;
#endif /* LWIP_ARP */
#endif /* LWIP_ARP || LWIP_ETHERNET */
#endif /* LWIP_IPV4 */

#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */

  netif->linkoutput = low_level_output;

  /* initialize the hardware */
  low_level_init(netif);

  return ERR_OK;
}

/**
  * @brief  Custom Rx pbuf free callback
  * @param  pbuf: pbuf to be freed
  * @retval None
  */
void pbuf_free_custom(struct pbuf *p)
{
  struct pbuf_custom* custom_pbuf = (struct pbuf_custom*)p;
  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);

  /* If the Rx Buffer Pool was exhausted, signal the ethernetif_input task to
   * call HAL_ETH_GetRxDataBuffer to rebuild the Rx descriptors. */

  if (RxAllocStatus == RX_ALLOC_ERROR)
  {
    RxAllocStatus = RX_ALLOC_OK;
    osSemaphoreRelease(RxPktSemaphore);
  }
}

/* USER CODE BEGIN 6 */

/**
* @brief  Returns the current time in milliseconds
*         when LWIP_TIMERS == 1 and NO_SYS == 1
* @param  None
* @retval Current Time value
*/
u32_t sys_now(void)
{
  return HAL_GetTick();
}

/* USER CODE END 6 */

/* USER CODE BEGIN PHI IO Functions for User BSP */

/* USER CODE END PHI IO Functions for User BSP */

/**
  * @brief  Check the ETH link state then update ETH driver and netif link accordingly.
  * @retval None
  */
void ethernet_link_thread(void* argument)
{

/* USER CODE BEGIN ETH link init */

/* USER CODE END ETH link init */

  for(;;)
  {

/* USER CODE BEGIN ETH link Thread core code for User BSP */

  /* CubeMX 6.17 新模板不再生成 LAN8742 的链路检测代码，这里自己实现。
     只做一件事：读 PHY 的 BSR(0x01) 的 Link 位(0x0004)，同步给 lwIP。        */
  struct netif *netif = (struct netif *)argument;
  static uint8_t link_up = 0U;
  uint32_t regvalue = 0U;

  if (HAL_ETH_ReadPHYRegister(&heth, ETH_PHY_ADDR, PHY_BSR, &regvalue) == HAL_OK)
  {
    if ((regvalue & PHY_LINKED_STATUS) != 0U)          /* Link established */
    {
      if (!link_up)
      {
        netif_set_link_up(netif);
        link_up = 1U;
      }
    }
    else
    {
      if (link_up)
      {
        netif_set_link_down(netif);
        link_up = 0U;
      }
    }
  }
  /* 循环末尾原有的 osDelay(100) 生效，这里不要再 delay，否则变成 200ms 轮询 */
/* USER CODE END ETH link Thread core code for User BSP */

    osDelay(100);
  }
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
/* USER CODE BEGIN HAL ETH RxAllocateCallback */
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
  if (p)
  {
    /* Get the buff from the struct pbuf address. */
    *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
    p->custom_free_function = pbuf_free_custom;
    /* Initialize the struct pbuf.
    * This must be performed whenever a buffer's allocated because it may be
    * changed by lwIP or the app, e.g., pbuf_free decrements ref. */
    pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUF_SIZE);
  }
  else
  {
    RxAllocStatus = RX_ALLOC_ERROR;
    *buff = NULL;
  }
/* USER CODE END HAL ETH RxAllocateCallback */
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
/* USER CODE BEGIN HAL ETH RxLinkCallback */

  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd = (struct pbuf **)pEnd;
  struct pbuf *p = NULL;

  /* Get the struct pbuf from the buff address. */
  p = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));
  p->next = NULL;
  p->tot_len = 0;
  p->len = Length;

  /* Chain the buffer. */
  if (!*ppStart)
  {
    /* The first buffer of the packet. */
    *ppStart = p;
  }
  else
  {
    /* Chain the buffer to the end of the packet. */
    (*ppEnd)->next = p;
  }
  *ppEnd  = p;

  /* Update the total length of all the buffers of the chain. Each pbuf in the chain should have its tot_len
   * set to its own length, plus the length of all the following pbufs in the chain. */
  for (p = *ppStart; p != NULL; p = p->next)
  {
    p->tot_len += Length;
  }

/* USER CODE END HAL ETH RxLinkCallback */
}

void HAL_ETH_TxFreeCallback(uint32_t * buff)
{
/* USER CODE BEGIN HAL ETH TxFreeCallback */

  pbuf_free((struct pbuf *)buff);

/* USER CODE END HAL ETH TxFreeCallback */
}

/* USER CODE BEGIN 8 */

/* ============================================================================
 * CubeMX 6.17.0-RC5 的新 LWIP 模板不会生成 HAL_ETH_MspInit()，
 * 导致 ETH 时钟、RMII 引脚、ETH 中断全部没有初始化（HAL 里只剩 __weak 空实现），
 * 表现是 HAL_ETH_Init() 里 DMA 软复位位永远不清零 → 超时 → Error_Handler 死循环。
 * 这里手工补齐；本段在 USER CODE 区内，重新生成代码不会被覆盖。
 * ========================================================================== */
void HAL_ETH_MspInit(ETH_HandleTypeDef *ethHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  if (ethHandle->Instance != ETH)
  {
    return;
  }

  /* 1) ETH 时钟：F4 没有 __HAL_RCC_ETH_CLK_ENABLE()，MAC/TX/RX 要分别开 */
  __HAL_RCC_ETHMAC_CLK_ENABLE();
  __HAL_RCC_ETHMACTX_CLK_ENABLE();
  __HAL_RCC_ETHMACRX_CLK_ENABLE();

  /* 2) GPIO 端口时钟 */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /* 3) RMII 引脚全部 AF11，速度必须 Very High（RMII 是 50MHz 信号，
        CubeMX 默认的 Low 会造成"能初始化但疯狂丢包"） */
  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull      = GPIO_NOPULL;
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF11_ETH;

  /* PA1 = REF_CLK (PHY 的 25MHz 晶振倍频后送来的 50MHz)
     PA2 = MDIO
     PA7 = CRS_DV                                            */
  GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* PC1 = MDC, PC4 = RXD0, PC5 = RXD1 */
  GPIO_InitStruct.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* PG11 = TX_EN, PG13 = TXD0, PG14 = TXD1
     ← 探索者 V3.5 板载把 TX_EN 接到 PG11（IO 表：PG11 接 YT8512C 的 TXEN 脚），
        CubeMX 默认会挑 PB11，硬件上没连，必须改到这里。                       */
  GPIO_InitStruct.Pin = GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* 4) ETH 中断：优先级必须 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY(5)，
        否则在 ISR 里调 FromISR 类 API 会触发 configASSERT 死机 */
  HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(ETH_IRQn);
}

/* USER CODE END 8 */

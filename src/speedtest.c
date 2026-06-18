#include "speedtest.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "fsl_netc_endpoint.h"
#include <zephyr/net/dsa_core.h>
#include "ats_time.h"

LOG_MODULE_REGISTER(speedtest, CONFIG_ATS_LOG_LEVEL);

#define STACK_SIZE 1024
#define NUM_STEPS 3
#define PACKET_SIZE 1500
#define MAX_TIMESTAMPS ((CONFIG_ATS_SPEEDTEST_BUF_SIZE / 8 / CONFIG_ATS_SPEEDTEST_MAX_INSTANCES / NUM_STEPS) * CONFIG_ATS_SPEEDTEST_MAX_INSTANCES * NUM_STEPS)
#define TX_BD_RING 0

#if TX_BD_RING == 0
#define HW_BD_RING 0
#else
#define HW_BD_RING (TX_BD_RING + 1)
#endif

uint64_t speedtest_start_timestamp = 0;
uint64_t speedtest_send_timestamps[MAX_TIMESTAMPS];

static inline size_t speedtest_store_time_index(uint8_t threadI, uint32_t packetI, uint8_t step)
{
    return (threadI + ((size_t)step * CONFIG_ATS_SPEEDTEST_MAX_INSTANCES) + ((size_t)packetI * CONFIG_ATS_SPEEDTEST_MAX_INSTANCES * NUM_STEPS)) % MAX_TIMESTAMPS;
}

static void speedtest_store_time(uint8_t threadI, uint32_t packetI, uint8_t step, uint64_t mask)
{
    size_t i = speedtest_store_time_index(threadI, packetI, step);
    ats_time_get(speedtest_send_timestamps + i);
    speedtest_send_timestamps[i] = ((speedtest_send_timestamps[i]) & 0x7fffffffffffffffLL) | mask;
}

static void speedtest_delayToNext(uint8_t threadI, uint32_t packetI, uint64_t delay)
{
    uint64_t thisStep = speedtest_send_timestamps[speedtest_store_time_index(threadI, packetI, 2)] & 0x7fffffffffffffffLL;
    uint64_t now;
    ats_time_get(&now);
    while (now - thisStep <= delay)
    {
        k_yield();
        ats_time_get(&now);
    }
}

static K_HEAP_DEFINE(speedtest_buf_heap, PACKET_SIZE * CONFIG_ATS_SPEEDTEST_MAX_INSTANCES * CONFIG_ETH_NXP_IMX_TX_RING_LEN + 512);

static K_MUTEX_DEFINE(speedtest_hal_mutex);

extern struct netc_eth_data
{
    DEVICE_MMIO_NAMED_RAM(port);
    DEVICE_MMIO_NAMED_RAM(pfconfig);
    ep_handle_t handle;
    struct net_if *iface;
    uint8_t mac_addr[6];
    /* TX */
    uint8_t *tx_buff;
    volatile bool tx_done;
    atomic_t tx_packetCount;
    /* RX */
    struct k_sem rx_sem;
    struct k_thread rx_thread;

    K_KERNEL_STACK_MEMBER(rx_thread_stack, CONFIG_ETH_NXP_IMX_RX_THREAD_STACK_SIZE);
    uint8_t *rx_frame;
#ifdef CONFIG_ETH_NXP_IMX_NETC_MSI_GIC
    unsigned int tx_intid;
    unsigned int rx_intid;
#endif
};

extern struct netc_eth_config
{
    DEVICE_MMIO_NAMED_ROM(port);
    DEVICE_MMIO_NAMED_ROM(pfconfig);
    uint16_t si_idx;
    const struct device *phy_dev;
    netc_hw_mii_mode_t phy_mode;
    volatile bool pseudo_mac;
    void (*generate_mac)(uint8_t *mac_addr);
    void (*bdr_init)(netc_bdr_config_t *bdr_config, netc_rx_bdr_config_t *rx_bdr_config,
                     netc_tx_bdr_config_t *tx_bdr_config);
    const struct pinctrl_dev_config *pincfg;
#ifdef CONFIG_ETH_NXP_IMX_NETC_MSI_GIC
    const struct device *msi_dev;
    uint8_t msi_device_id; /* MSI device ID */
#else
    uint8_t tx_intr_msg_data;
    uint8_t rx_intr_msg_data;
#endif
#ifdef NETC_PTP_TIMESTAMPING_SUPPORT
    const struct device *ptp_clock;
#endif
};

void speedtest_hal(void *if_param, void *thread_num, void *p3)
{
    ARG_UNUSED(p3);
    struct net_if *iface = (struct net_if *)if_param;
    uint8_t threadNum = (uint8_t)(size_t)thread_num;
    LOG_INF("speedtest_hal started");

    uint8_t *buf[CONFIG_ETH_NXP_IMX_TX_RING_LEN];
    uint32_t *i[CONFIG_ETH_NXP_IMX_TX_RING_LEN];
    netc_buffer_struct_t buff[CONFIG_ETH_NXP_IMX_TX_RING_LEN]; // = {.buffer = buf, .length = PACKET_SIZE};
    netc_frame_struct_t frame[CONFIG_ETH_NXP_IMX_TX_RING_LEN]; // = {.buffArray = &buff, .length = 1};

    for (size_t a = 0; a < CONFIG_ETH_NXP_IMX_TX_RING_LEN; a++)
    {
        buf[a] = k_heap_alloc(&speedtest_buf_heap, PACKET_SIZE, K_NO_WAIT);
        __ASSERT(buf[a] != NULL, "could not aquire buffer memory for speedtest instance %d", threadNum);
        i[a] = (uint32_t *)(buf[a] + 14);
        for (size_t x = 0; x < PACKET_SIZE; x++)
        {
            if (x < 12)
            {
                buf[a][x] = x;
            }
            else
            {
                buf[a][x] = threadNum;
            }
        }
        *((uint16_t *)(buf[a] + 12)) = 0x4208;
        *(i[a]) = 0;
        *((uint16_t *)(buf[a] + 26)) = a;

        buff[a].buffer = buf[a];
        buff[a].length = PACKET_SIZE;
        frame[a].buffArray = &(buff[a]);
        frame[a].length = 1;
    }

    const struct device *dev = net_if_get_device(iface);
    struct netc_eth_data *data = dev->data;
#ifdef CONFIG_NET_DSA
    const struct netc_eth_config *cfg = dev->config;
    int dst_port = -1;
    if (cfg->pseudo_mac)
    {
        struct dsa_switch_context *dsa_switch_ctx = dev->data;
        const struct device *conduitDev = net_if_get_device(dsa_switch_ctx->iface_conduit);
        data = conduitDev->data;
        const struct dsa_port_config *port_cfg = dev->config;
        dst_port = port_cfg->port_idx;
    }
#endif
    ep_tx_opt opt = {0};
    int errSum = 0;

    uint32_t pktI = 0;
    while (true)
    {
        speedtest_store_time(threadNum, pktI, 0, 0);
        netc_tx_bd_t txDesc[2] = {0};
        NETC_ClearTxDescriptor(&txDesc[0]);
        NETC_ClearTxDescriptor(&txDesc[1]);
#ifdef CONFIG_NET_DSA
        if (dst_port != -1)
        {
#if TX_BD_RING == 0
            txDesc[0].standard.flags = NETC_SI_TXDESCRIP_RD_FLQ(2) |
                                       NETC_SI_TXDESCRIP_RD_SMSO_MASK |
                                       NETC_SI_TXDESCRIP_RD_PORT(dst_port);
#endif
            /*txDesc[0].standard.isExtended = 1;
            txDesc[1].ext.eFlags = 1; //vlan offload
            txDesc[1].ext.pcp = 3;
            txDesc[1].ext.dei = 0;
            txDesc[1].ext.vid = 158;
            txDesc[1].ext.tpid= 0b00; //standard vlan 0x8100*/
        }
#endif
        *(i[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN]) = pktI;

        speedtest_store_time(threadNum, pktI, 1, 0);
        // *((uint16_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 44)) = 0;
        /*while ((data->handle.txBdRing[TX_BD_RING].producerIndex + 1) % CONFIG_ETH_NXP_IMX_TX_RING_LEN == data->handle.txBdRing[TX_BD_RING].cleanIndex)
        {
            *((uint16_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 44)) = 1;
            EP_ReclaimTxDescCommon(&data->handle, &data->handle.txBdRing[TX_BD_RING], HW_BD_RING, false);
        }*/
        /* *((uint16_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 30)) = data->handle.txBdRing[TX_BD_RING].producerIndex;
         *((uint16_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 32)) = data->handle.txBdRing[TX_BD_RING].cleanIndex;
         *((uint16_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 34)) = data->handle.hw.si->BDR[TX_BD_RING].TBCIR;*/
        k_mutex_lock(&speedtest_hal_mutex, K_FOREVER);
        status_t result = EP_SendFrameCommon(&data->handle, &data->handle.txBdRing[TX_BD_RING], HW_BD_RING, &(frame[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN]),
                                             NULL, &txDesc[0], data->handle.cfg.txCacheMaintain);
        k_mutex_unlock(&speedtest_hal_mutex);
        pktI++;
        /**((uint16_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 36)) = data->handle.txBdRing[TX_BD_RING].producerIndex;
         *((uint16_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 38)) = data->handle.txBdRing[TX_BD_RING].cleanIndex;
         *((uint16_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 40)) = data->handle.hw.si->BDR[TX_BD_RING].TBCIR;*/

        // EP_ReclaimTxDescCommon(&data->handle, &data->handle.txBdRing[TX_BD_RING], HW_BD_RING, false);
        if (result == 0)
        {
            /*while (data->handle.hw.si->BDR[TX_BD_RING].TBCIR != data->handle.txBdRing[TX_BD_RING].producerIndex)
            {
            }*/
            speedtest_store_time(threadNum, pktI - 1, 2, SPEEDTEST_BITMASK_SEND_SUCCESS);
        }
        else
        {
            speedtest_store_time(threadNum, pktI - 1, 2, 0);
            errSum++;
            *((uint32_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 22)) = errSum;
        }
        *((uint32_t *)(buf[pktI % CONFIG_ETH_NXP_IMX_TX_RING_LEN] + 18)) = result;
        // speedtest_delayToNext(threadNum, pktI, 20000);
        k_yield();
        //*i += 1;
    }
}

void speedtest_zephyr(void *if_param, void *thread_num, void *p3)
{
    ARG_UNUSED(p3);
    uint8_t threadNum = (uint8_t)(size_t)thread_num;
    LOG_INF("speedtest_zephyr started");

    uint8_t *bufData = k_heap_alloc(&speedtest_buf_heap, PACKET_SIZE, K_NO_WAIT);
    __ASSERT(bufData != NULL, "could not aquire buffer memory for speedtest instance %d", threadNum);
    uint32_t *i = (uint32_t *)(bufData + 14);

    struct net_pkt *pkt = net_pkt_alloc_on_iface((struct net_if *)if_param, K_FOREVER);
    struct net_buf buf_inst = {
        .ref = 1U,
        .flags = 0U,
        .size = PACKET_SIZE,
        .user_data_size = 0,
        .b = {
            .__buf = bufData,
            .data = bufData,
            .size = PACKET_SIZE,
            .len = 0,
        },
    };
    struct net_buf *buf = &buf_inst;
    net_buf_reset(buf);
    net_pkt_append_buffer(pkt, buf);
    net_buf_reserve(buf, 14);

    net_pkt_set_ll_proto_type(pkt, 0x0842);
    net_pkt_memset(pkt, threadNum, PACKET_SIZE - 14);
    *i = 0;

    int errSum = 0;

    while (true)
    {
        speedtest_store_time(threadNum, *i, 0, 0);
        buf->data = buf->__buf + 14; // reserve space for ethernet headers, reference get_reserve_ll_header_size; can't use net_buf_reserve as it requires the buffer to be empty but we want to overwrite
        buf->len = buf->size - 14;
        net_pkt_ref(pkt);

        speedtest_store_time(threadNum, *i, 1, 0);
        int result = net_send_data(pkt);

        if (result)
        {
            speedtest_store_time(threadNum, *i, 2, SPEEDTEST_BITMASK_SEND_SUCCESS);
            errSum += result;
            *((uint32_t *)(bufData + 22)) = errSum;
        }
        else
        {
            speedtest_store_time(threadNum, *i, 2, SPEEDTEST_BITMASK_SEND_FAIL);
        }
        *((uint32_t *)(bufData + 18)) = result;
        *i += 1;
    }
}

static K_THREAD_STACK_ARRAY_DEFINE(speedtest_thread_stack, CONFIG_ATS_SPEEDTEST_MAX_INSTANCES, STACK_SIZE);
static struct k_thread speedtest_thread_data[CONFIG_ATS_SPEEDTEST_MAX_INSTANCES];
static atomic_t speedtest_threads_count = 0;

k_tid_t speedtest_start(struct net_if *iface, uint8_t prio, bool useZephyrStack)
{
    uint8_t newThreadNum = (uint8_t)atomic_inc(&speedtest_threads_count);

    if (newThreadNum == 0)
    {
        ats_time_init(NULL);
        ats_time_get(&speedtest_start_timestamp);
    }

    __ASSERT(newThreadNum < CONFIG_ATS_SPEEDTEST_MAX_INSTANCES, "Couldn't greate speedtest instance %d as only space for %d instances configured", newThreadNum, CONFIG_ATS_SPEEDTEST_MAX_INSTANCES);

    k_tid_t speedtest_thread_id = k_thread_create(
        &speedtest_thread_data[newThreadNum], &speedtest_thread_stack[newThreadNum][0], STACK_SIZE,
        useZephyrStack ? speedtest_zephyr : speedtest_hal, iface, (void *)(size_t)newThreadNum, NULL,
        prio, 0, K_NO_WAIT);

    return speedtest_thread_id;
}
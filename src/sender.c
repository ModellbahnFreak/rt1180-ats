#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include "ats.h"
#include "sender.h"
#include "fsl_netc_endpoint.h"
#include <zephyr/net/dsa_core.h>

LOG_MODULE_REGISTER(ats_sender, CONFIG_ATS_LOG_LEVEL);

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

// AT_NONCACHEABLE_SECTION_ALIGN(static uint8_t sender_buf[1500], 64);
NET_BUF_SIMPLE_DEFINE_STATIC(sender_buf, 1500);

void sender_speedtest(void *if_param, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    struct net_if *iface = (struct net_if *)if_param;
    LOG_INF("speedtest started");

    uint8_t *buf = net_buf_data_sender_buf;
    for (size_t i = 0; i < 1500; i++)
    {
        buf[i] = 0xff;
    }
    *((uint16_t *)(buf + 12)) = 0x4208;
    *((uint32_t *)(buf + 14)) = 0;

    const struct device *dev = net_if_get_device(iface);
    struct dsa_switch_context *dsa_switch_ctx = dev->data;
    const struct device *conduitDev = net_if_get_device(dsa_switch_ctx->iface_conduit);
    struct netc_eth_data *data = conduitDev->data;
    netc_buffer_struct_t buff = {.buffer = buf, .length = 1500};
    netc_frame_struct_t frame = {.buffArray = &buff, .length = 1};
    ep_tx_opt opt = {0};

    const struct dsa_port_config *port_cfg = dev->config;
    const int dst_port = port_cfg->port_idx;

    while (true)
    {
        netc_tx_bd_t txDesc[2] = {0};
        NETC_ClearTxDescriptor(&txDesc[0]);
        NETC_ClearTxDescriptor(&txDesc[1]);
        txDesc[0].standard.flags = NETC_SI_TXDESCRIP_RD_FLQ(2) |
                                   NETC_SI_TXDESCRIP_RD_SMSO_MASK |
                                   NETC_SI_TXDESCRIP_RD_PORT(dst_port);
        status_t result = EP_SendFrameCommon(&data->handle, &data->handle.txBdRing[0], 0, &frame,
                                             NULL, &txDesc[0], data->handle.cfg.txCacheMaintain);

        // status_t result = EP_SendFrame(&data->handle, 0, &frame, NULL, &opt);
        if (result)
        {
            LOG_ERR("Send error: %d", result);
        }
        *((uint32_t *)(buf + 14)) += 1;
    }
}

void sender(void *sender_parameters, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    struct sender_params *params = (struct sender_params *)sender_parameters;

    LOG_INF("sender started");

    k_tid_t appId = k_current_get();
    struct ats *ats_data = ats_get_by_net_if(params->iface);
    uint32_t maxTokenRate = params->maxPacket * 1000000 / params->minIntervalUs;
    if (!params->regular)
    {
        maxTokenRate = params->maxPacket * 1000000 / ((params->minIntervalUs + params->maxIntervalUs) / 2);
    }
    uint8_t schedId = ats_register_stream(ats_data, -1, maxTokenRate, params->maxPacket * 3 / 2);
    uint32_t counter = 0;

    if (params->regular)
    {
        // ######Regular packet allocation
        // struct net_pkt *pkt = net_pkt_alloc_with_buffer(params->iface, params->maxPacket, NET_AF_UNSPEC, 0, K_FOREVER);

        // ###### Alternative: Use static (contiguous) buffer
        struct net_pkt *pkt = net_pkt_alloc_on_iface(params->iface, K_FOREVER);
        struct net_buf buf_inst = {
            .ref = 1U,
            .flags = 0U,
            .size = sender_buf.size,
            .user_data_size = 0,
            .b = sender_buf,
        };
        struct net_buf *buf = &buf_inst;
        net_buf_reset(buf);
        net_pkt_append_buffer(pkt, buf);
        net_buf_reserve(buf, 14);

        net_pkt_set_ll_proto_type(pkt, 0x0842);
        net_pkt_memset(pkt, 5, params->maxPacket);
        *((uint32_t *)(net_buf_data_sender_buf + 14)) = 0;
        while (true)
        {
            buf->data = buf->__buf + 14; // reserve space for ethernet headers, reference get_reserve_ll_header_size; can't use net_buf_reserve as it requires the buffer to be empty but we want to overwrite
            buf->len = params->maxPacket;
            net_pkt_ref(pkt);

            LOG_INF("App %d trying to send %d bytes, then waiting %d us", (int)appId, net_pkt_get_len(pkt), params->maxIntervalUs);
            // ats_send(ats_data, schedId, pkt);
            // k_sleep(K_USEC(params->maxIntervalUs));
            net_send_data(pkt);
            *((uint32_t *)(net_buf_data_sender_buf + 14)) += 1;
        }
    }
    else
    {
        while (true)
        {
            uint16_t pkgSize = sys_rand16_get() % params->maxPacket;
            uint32_t sleepTime = (sys_rand32_get() % (params->maxIntervalUs - params->minIntervalUs)) + params->minIntervalUs;
            struct net_pkt *pkt = net_pkt_alloc_with_buffer(params->iface, pkgSize, NET_AF_UNSPEC, 0, K_FOREVER);
            net_pkt_write_be32(pkt, counter++);
            net_pkt_memset(pkt, 2, params->maxPacket);
            LOG_INF("App %d trying to send %d bytes, then waiting %d us", (int)appId, net_pkt_get_len(pkt), sleepTime);
            net_pkt_set_ll_proto_type(pkt, 0x22f0);
            ats_send(ats_data, schedId, pkt);
            k_sleep(K_USEC(sleepTime));
        }
    }
}
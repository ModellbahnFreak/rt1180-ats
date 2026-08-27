#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include "ats_iface.h"
#include "ats.h"
#include "sender.h"

LOG_MODULE_REGISTER(ats_sender, CONFIG_ATS_LOG_LEVEL);

#define useStaticBuf
// #define sendDirectAts

static __noinit uint8_t sender_buf[1500];

struct net_buf_pool *tx_bufs;
struct net_buf_pool *rx_bufs;

static struct net_pkt *allocate_and_prepare_pkt(struct net_if *iface, uint16_t size)
{
#ifdef useStaticBuf
    struct net_pkt *pkt = net_pkt_alloc_on_iface(iface, K_FOREVER);
    struct net_buf buf_inst = {
        .ref = 1U,
        .flags = 0U,
        .size = size,
        .user_data_size = 0,
        .b = {
            .__buf = sender_buf,
            .data = sender_buf,
            .size = size,
            .len = 0,
        },
    };
    struct net_buf *buf = &buf_inst;
    net_buf_reset(buf);
    net_pkt_append_buffer(pkt, buf);

    net_buf_reserve(buf, 14);
    net_pkt_cursor_init(pkt);
#else
    struct net_pkt *pkt = net_pkt_alloc_with_buffer(iface, size - 14, NET_AF_UNSPEC, 0, K_FOREVER);
#endif

    net_pkt_write_le32(pkt, 0);
    net_pkt_memset(pkt, 0xff, size - 14 - 4);

    net_pkt_set_ll_proto_type(pkt, 0x0842);

#ifdef sendDirectAts
    ((struct net_eth_hdr *)sender_buf)->type = net_htons(net_pkt_ll_proto_type(pkt));
    uint8_t broadcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    net_linkaddr_set(net_pkt_lladdr_dst(pkt),
                     broadcast,
                     sizeof(broadcast));
#endif

    return pkt;
}

void sender(void *sender_parameters, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    struct sender_params *params = (struct sender_params *)sender_parameters;

    LOG_INF("sender started");

    uint32_t maxTokenRate = params->maxPacket * 1000000 / params->maxIntervalUs;
    if (!params->regular)
    {
        maxTokenRate = params->maxPacket * 1000000 / ((params->minIntervalUs + params->maxIntervalUs) / 2);
    }
    uint32_t counter = 0;

    k_tid_t appId = k_current_get();
#ifdef sendDirectAts
    struct ats *ats_data = ats_get_by_net_if(params->iface);
    uint8_t schedId = ats_register_stream(ats_data, -1, maxTokenRate, params->maxPacket * 3 / 2);
#else
    uint8_t schedId = ats_iface_register_stream(params->iface, maxTokenRate, params->maxPacket * 3 / 2);
#endif

    if (params->regular)
    {

        uint8_t i = 0;
        struct net_pkt *pkts[CONFIG_ATS_QUEUE_LENGTH + 1];
        for (i = 0; i < CONFIG_ATS_QUEUE_LENGTH + 1; i++)
        {
            pkts[i] = allocate_and_prepare_pkt(params->iface, params->maxPacket);
            net_pkt_set_ats_stream(pkts[i], schedId);
        }

        i = 0;
        struct net_pkt *pkt = pkts[i];
        while (true)
        {

            LOG_INF("App %d trying to send pkt %d with %d bytes, then waiting %d us", (int)appId, i, net_pkt_get_len(pkt), params->minIntervalUs);

#ifdef sendDirectAts
            int res = ats_send(ats_data, schedId, pkt);
#else
            net_pkt_set_iface(pkt, params->iface);
            net_pkt_ref(pkt);
            pkt->buffer->data = pkt->buffer->__buf + 14; // reserve space for ethernet headers, reference get_reserve_ll_header_size; can't use net_buf_reserve as it requires the buffer to be empty but we want to overwrite
            pkt->buffer->len = pkt->buffer->size - 14;

            int res = net_send_data(pkt);
#endif
            if (!res)
            {
                i = (i + 1) % (CONFIG_ATS_QUEUE_LENGTH + 1);
                pkt = pkts[i];
            }

            k_sleep(K_USEC(params->minIntervalUs));
            *((uint32_t *)(net_pkt_data(pkt) + 14)) = ++counter;
        }
    }
    else
    {
        while (true)
        {
            uint16_t pkgSize = /*sys_rand16_get() %*/ params->maxPacket;
            uint32_t sleepTime = (sys_rand32_get() % (params->maxIntervalUs - params->minIntervalUs)) + params->minIntervalUs;
            struct net_pkt *pkt = net_pkt_alloc_with_buffer(params->iface, pkgSize, NET_AF_UNSPEC, 0, K_FOREVER);
            net_pkt_write_be32(pkt, counter++);
            net_pkt_memset(pkt, 2, params->maxPacket);
            LOG_INF("App %d trying to send %d bytes, then waiting %d us", (int)appId, net_pkt_get_len(pkt), sleepTime);

#ifdef sendDirectAts
            uint8_t broadcast[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            net_linkaddr_set(net_pkt_lladdr_dst(pkt),
                             broadcast,
                             sizeof(broadcast));
            ats_send(ats_data, schedId, pkt);
#else
            net_pkt_set_ll_proto_type(pkt, 0x22f0);
            net_pkt_set_ats_stream(pkt, schedId);
            net_send_data(pkt);
#endif

            k_sleep(K_USEC(sleepTime));
        }
    }
}
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include "ats.h"
#include "sender.h"

LOG_MODULE_REGISTER(ats_sender, CONFIG_ATS_LOG_LEVEL);

void sender(void *sender_parameters, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    struct sender_params *params = (struct sender_params *)sender_parameters;

    LOG_INF("sender started");

    k_tid_t appId = k_current_get();
    struct ats *ats_data = ats_get_by_net_if(params->iface);
    uint8_t schedId = ats_register_stream(ats_data, -1, params->maxPacket * 1000000 / params->minIntervalUs, params->maxPacket * 3 / 2);

    if (params->regular)
    {
        while (true)
        {
            struct net_pkt *pkt = net_pkt_alloc_with_buffer(params->iface, params->maxPacket, NET_AF_UNSPEC, 0, K_FOREVER);
            net_pkt_set_ll_proto_type(pkt, 0x0842);
            net_pkt_memset(pkt, 5, params->maxPacket);
            LOG_INF("App %d trying to send %d bytes, then waiting %d us", (int)appId, net_pkt_get_len(pkt), params->maxIntervalUs);
            ats_send(ats_data, schedId, pkt);
            k_sleep(K_USEC(params->maxIntervalUs));
        }
    }
    else
    {
        while (true)
        {
            uint16_t pkgSize = sys_rand16_get() % params->maxPacket;
            uint32_t sleepTime = (sys_rand32_get() % (params->maxIntervalUs - params->minIntervalUs)) + params->minIntervalUs;
            struct net_pkt *pkt = net_pkt_alloc_with_buffer(params->iface, pkgSize, NET_AF_UNSPEC, 0, K_FOREVER);
            net_pkt_memset(pkt, 2, params->maxPacket);
            LOG_INF("App %d trying to send %d bytes, then waiting %d us", (int)appId, net_pkt_get_len(pkt), sleepTime);
            net_pkt_set_ll_proto_type(pkt, 0x22f0);
            ats_send(ats_data, schedId, pkt);
            k_sleep(K_USEC(sleepTime));
        }
    }
}
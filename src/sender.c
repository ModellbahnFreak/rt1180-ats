#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include "ats.h"
#include "sender.h"

LOG_MODULE_REGISTER(ats_sender, CONFIG_ATS_LOG_LEVEL);

AT_NONCACHEABLE_SECTION_ALIGN(static uint8_t aligned_sender_buf[1], 64);
NET_BUF_SIMPLE_DEFINE_STATIC(sender_buf, 1400);

struct net_buf_pool *tx_bufs;
struct net_buf_pool *rx_bufs;

void sender(void *sender_parameters, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    struct sender_params *params = (struct sender_params *)sender_parameters;

    LOG_INF("sender started");

    k_tid_t appId = k_current_get();
    struct ats *ats_data = ats_get_by_net_if(params->iface);
    uint32_t maxTokenRate = params->maxPacket * 1000000 / params->maxIntervalUs;
    if (!params->regular)
    {
        maxTokenRate = params->maxPacket * 1000000 / ((params->minIntervalUs + params->maxIntervalUs) / 2);
    }
    uint8_t schedId = ats_register_stream(ats_data, -1, maxTokenRate, params->maxPacket * 3 / 2);
    uint32_t counter = 0;

    if (params->regular)
    {
        // #define useStaticBuf

#ifdef useStaticBuf
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
        net_pkt_alloc_buffer(pkt, 100 - 14, 0, K_NO_WAIT); // makes pkt non contiguous
        net_buf_reserve(buf, 14);

        net_pkt_set_ll_proto_type(pkt, 0x0842);
        net_pkt_memset(pkt, 5, params->maxPacket);
        *((uint32_t *)(net_buf_data_sender_buf + 14)) = 0;
#else
        int i = 0;
        net_pkt_get_info(NULL, NULL, &rx_bufs, &tx_bufs);
#endif

        while (true)
        {
            int64_t preSend[8] = {k_uptime_ticks(), 0, 0, 0, 0, 0, 0, 0};
#ifdef useStaticBuf
            buf->data = buf->__buf + 14; // reserve space for ethernet headers, reference get_reserve_ll_header_size; can't use net_buf_reserve as it requires the buffer to be empty but we want to overwrite
            buf->len = buf->size - 14;
            net_pkt_ref(pkt);
#else
            LOG_DBG("Space in rx pool: %d, space in tx pool: %d", rx_bufs->uninit_count, tx_bufs->uninit_count);
            // ######Regular packet allocation
            struct net_pkt *pkt = net_pkt_alloc_with_buffer(params->iface, params->maxPacket, NET_AF_UNSPEC, 0, K_FOREVER);
            // todo: find out, why packet allocation takes so long
            preSend[1] = k_uptime_ticks();
            net_pkt_set_ll_proto_type(pkt, 0x0842);
            preSend[2] = k_uptime_ticks();
            net_pkt_write_le32(pkt, i++);
            preSend[3] = k_uptime_ticks();
            net_pkt_memset(pkt, 5, params->maxPacket);
            preSend[4] = k_uptime_ticks();
            net_pkt_set_priority(pkt, i % 8);
#endif

            LOG_INF("App %d trying to send %d bytes, then waiting %d us", (int)appId, net_pkt_get_len(pkt), params->minIntervalUs);
            preSend[5] = k_uptime_ticks();
            ats_send(ats_data, schedId, pkt);
            preSend[6] = k_uptime_ticks();
            k_sleep(K_USEC(params->minIntervalUs));
            preSend[7] = k_uptime_ticks();
            // net_send_data(pkt);

            int64_t waitTime = (preSend[7] - preSend[0]) * 1000000LL / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
            if (waitTime > 1200000)
            {
                LOG_ERR("Wait of sender was longer than 1.2s (%" PRId64 "us)", waitTime);
            }
            LOG_INF("Send timings: total: %" PRId64 " us; allocate: %" PRId64 " us, setType: %" PRId64 " us, write id: %" PRId64 " us; memset: %" PRId64 " us; prio: %" PRId64 " us; ats: %" PRId64 " us; sleep: %" PRId64 " us", waitTime,
                    (preSend[1] - preSend[0]) * 1000000LL / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
                    (preSend[2] - preSend[1]) * 1000000LL / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
                    (preSend[3] - preSend[2]) * 1000000LL / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
                    (preSend[4] - preSend[3]) * 1000000LL / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
                    (preSend[5] - preSend[4]) * 1000000LL / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
                    (preSend[6] - preSend[5]) * 1000000LL / CONFIG_SYS_CLOCK_TICKS_PER_SEC,
                    (preSend[7] - preSend[6]) * 1000000LL / CONFIG_SYS_CLOCK_TICKS_PER_SEC);
#ifdef useStaticBuf
            *((uint32_t *)(net_buf_data_sender_buf + 14)) += 1;
#endif
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
            net_pkt_set_ll_proto_type(pkt, 0x22f0);
            ats_send(ats_data, schedId, pkt);
            k_sleep(K_USEC(sleepTime));
        }
    }
}
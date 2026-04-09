#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include "ats.h"
#include "sender.h"

LOG_MODULE_DECLARE(ats, CONFIG_ATS_LOG_LEVEL);

void sender(struct sender_params *params)
{
    k_tid_t appId = k_current_get();
    struct ats *ats_data = ats_get_by_net_if(params->iface);
    uint8_t schedId = ats_register_stream(ats_data, -1, params->maxPacket * 1000000 / params->minIntervalUs, params->maxPacket * 3 / 2);
    char pkg[params->maxPacket];

    if (params->regular)
    {
        while (true)
        {
            LOG_INF("App %d trying to send %d bytes, then waiting %d us", (int)appId, params->maxPacket, params->maxIntervalUs);
            ats_send(ats_data, schedId, pkg, params->maxPacket);
            k_sleep(K_USEC(params->maxIntervalUs));
        }
    }
    else
    {
        while (true)
        {
            uint16_t pkgSize = sys_rand16_get() % params->maxPacket;
            uint32_t sleepTime = sys_rand32_get() % (params->maxIntervalUs - params->minIntervalUs) + params->minIntervalUs;
            LOG_INF("App %d trying to send %d bytes, then waiting %d us", (int)appId, pkgSize, sleepTime);
            ats_send(ats_data, schedId, pkg, pkgSize);
            k_sleep(K_USEC(sleepTime));
        }
    }
}
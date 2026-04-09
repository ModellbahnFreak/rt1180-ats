#ifndef RT1180_ATS_SENDER_H__
#define RT1180_ATS_SENDER_H__

#include <zephyr/net/net_if.h>

struct sender_params {
    uint16_t maxPacket;
    bool regular;
    uint32_t maxIntervalUs;
    uint32_t minIntervalUs;
    struct net_if* iface;
};

void sender(struct sender_params *params);

#endif
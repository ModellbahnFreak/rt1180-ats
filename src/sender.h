#ifndef RT1180_ATS_SENDER_H__
#define RT1180_ATS_SENDER_H__

#ifdef CONFIG_ATS_SOFTWARE_DRIVER

#include <zephyr/net/net_if.h>

struct sender_params
{
    uint16_t maxPacket;
    bool regular;
    uint32_t maxIntervalUs;
    uint32_t minIntervalUs;
    struct net_if *iface;
};

void sender(void *sender_parameters, void *p2, void *p3);

#else // CONFIG_ATS_SOFTWARE_DRIVER

struct sender_params
{
};
#define sender(...) \
    {               \
    }

#endif // CONFIG_ATS_SOFTWARE_DRIVER

#endif
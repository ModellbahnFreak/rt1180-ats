#ifndef RT1180_ATS_ATS_IFACE_H__
#define RT1180_ATS_ATS_IFACE_H__

#include <zephyr/kernel.h>

bool ats_iface_is_ats(struct net_if *ats_if);
struct ats *ats_iface_get_data(struct net_if *iface);

uint8_t ats_iface_register_stream_to_group(struct net_if *ats_if, int groupId, uint32_t tokensPerSecond, uint32_t burstSize);
uint8_t ats_iface_register_stream(struct net_if *ats_if, uint32_t tokensPerSecond, uint32_t burstSize);

uint8_t ats_iface_stream_get_group(struct net_if *ats_if, uint8_t streamId);

#endif
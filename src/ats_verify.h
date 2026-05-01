#ifndef RT1180_ATS_ATS_VERIFY_H__
#define RT1180_ATS_ATS_VERIFY_H__

#include <zephyr/kernel.h>

void ats_verify_packet_sent(void *ats, void *group, uint16_t packetSize);

#endif
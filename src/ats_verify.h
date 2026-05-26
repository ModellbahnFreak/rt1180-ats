#ifndef RT1180_ATS_ATS_VERIFY_H__
#define RT1180_ATS_ATS_VERIFY_H__

#include <zephyr/kernel.h>

void ats_verify_packet_sent(void *streamId, uint16_t packetSize, uint64_t elegibilityTime);

void ats_verify_set_stream_parameters(void *streamId, uint32_t tokensPerSecond, uint32_t burstSize);

#endif
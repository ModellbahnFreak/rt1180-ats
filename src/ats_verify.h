#ifndef RT1180_ATS_ATS_VERIFY_H__
#define RT1180_ATS_ATS_VERIFY_H__

#include <zephyr/kernel.h>
#include <zephyr/net/net_pkt.h>


void ats_verify_packet_enqueued(void *streamId, struct net_pkt *pkt, uint64_t arrivalTime);

void ats_verify_packet_sent(void *streamId, struct net_pkt *pkt, uint64_t elegibilityTime);

void ats_verify_set_stream_parameters(void *streamId, void* ats_data, uint32_t tokensPerSecond, uint32_t burstSize);

#endif
#ifndef RT1180_ATS_ATS_VERIFY_H__
#define RT1180_ATS_ATS_VERIFY_H__

#include <zephyr/kernel.h>

struct ats_verify_sent_event
{
    void *streamId;
    uint64_t timestamp;
    uint64_t elegibilityTime;
    uint16_t packetSize;
};

struct ats_verify_stream_t
{
    void *streamId;
    int32_t bucket;
    uint64_t lastUpdate;
    uint32_t tokensPerSecond;
    uint32_t burstSize;
};

struct ats_verify_t
{
    struct ats_verify_sent_event events[128];
    uint8_t producerIdx;
    uint8_t consumerIdx;
    struct ats_verify_stream_t streams[CONFIG_ATS_SCHEDULER_INSTANCES];
    uint64_t lastElegibilityTime;
};

void ats_verify_packet_sent(struct ats_verify_t *verifier, void *streamId, uint16_t packetSize, uint64_t elegibilityTime);

void ats_verify_set_stream_parameters(struct ats_verify_t *verifier, void *streamId, uint32_t tokensPerSecond, uint32_t burstSize);

void ats_verify_start_processing(struct ats_verify_t *verifier);

#endif
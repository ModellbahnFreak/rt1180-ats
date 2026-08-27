#include "ats_verify.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include "ats_time.h"
#include <zephyr/logging/log.h>
#include "constants.h"

LOG_MODULE_REGISTER(ats_verify, CONFIG_ATS_VERIFY_LOG_LEVEL);

#define NUM_STREAMS CONFIG_ATS_SCHEDULER_INSTANCES

struct ats_verify_sent_event // 24byte
{
    uint64_t timestamp;
    uint64_t elegibilityTime;
    void *streamId;
    uint16_t packetSize;
};

struct ats_verify_stream_t // 32byte
{
    uint64_t lastUpdate;
    void *streamId;
    struct ats_verify_iface_t *ats_iface;
    int32_t bucket;
    uint32_t tokensPerSecond;
    uint32_t burstSize;
};

struct ats_verify_iface_t // 16byte
{
    uint64_t lastElegibilityTime;
    void *ats_data;
};

#define CONFIG_ATS_VERIFY_NUM_EVENTS 2500

struct ats_verify_t
{
    struct ats_verify_sent_event events[CONFIG_ATS_VERIFY_NUM_EVENTS];
    struct ats_verify_stream_t streams[CONFIG_ATS_SCHEDULER_INSTANCES];
    struct ats_verify_iface_t ifaces[CONFIG_ATS_STRUCT_ITERABLE_SIZE];
    uint16_t producerIdx;
    uint16_t consumerIdx;
};

static bool finish = false;
struct ats_verify_t ats_verify_instance;

void ats_verify_packet_sent(void *streamId, struct net_pkt *pkt, uint64_t elegibilityTime)
{
    uint16_t i;
    unsigned int key = irq_lock();
#ifndef CONFIG_ATS_VERIFY_NO_PROCESSING
    if (((ats_verify_instance.producerIdx + 1) % CONFIG_ATS_VERIFY_NUM_EVENTS) == ats_verify_instance.consumerIdx)
    {
        irq_unlock(key);
        LOG_WRN("verify queue full, dropping event");
        return;
    }
#endif
    i = ats_verify_instance.producerIdx;
    ats_verify_instance.producerIdx = (ats_verify_instance.producerIdx + 1) % CONFIG_ATS_VERIFY_NUM_EVENTS;
    irq_unlock(key);
    struct ats_verify_sent_event *write = &ats_verify_instance.events[i];
    write->streamId = streamId;
    write->packetSize = net_pkt_get_len(pkt);
    write->elegibilityTime = elegibilityTime;
    ats_time_get(&(write->timestamp));
    if (write->packetSize >= 40)
    {
        *((uint64_t *)(pkt->buffer->__buf + 28)) = elegibilityTime;
        *((uint64_t *)(pkt->buffer->__buf + 36)) = write->timestamp;
    }
}

void ats_verify_set_stream_parameters(void *streamId, void *ats_data, uint32_t tokensPerSecond, uint32_t burstSize)
{
    struct ats_verify_stream_t *stream = NULL;
    bool isNew = false;
    for (size_t i = 0; i < NUM_STREAMS; i++)
    {
        if (ats_verify_instance.streams[i].streamId == streamId)
        {
            stream = &ats_verify_instance.streams[i];
            break;
        }
        else if (ats_verify_instance.streams[i].streamId == NULL)
        {
            stream = &ats_verify_instance.streams[i];
            isNew = true;
            break;
        }
    }
    __ASSERT(stream != NULL, "no free stream in verifier found to set params");

    for (size_t i = 0; i < CONFIG_ATS_STRUCT_ITERABLE_SIZE; i++)
    {
        if (ats_verify_instance.ifaces[i].ats_data == ats_data)
        {
            stream->ats_iface = &(ats_verify_instance.ifaces[i]);
            break;
        }
        else if (ats_verify_instance.ifaces[i].ats_data == NULL)
        {
            stream->ats_iface = &(ats_verify_instance.ifaces[i]);
            break;
        }
    }
    __ASSERT(stream->ats_iface != NULL, "no free iface in verifier found to set params");

    stream->streamId = streamId;
    stream->ats_iface->ats_data = ats_data;
    stream->tokensPerSecond = tokensPerSecond;
    stream->burstSize = burstSize;
    stream->bucket = burstSize;
}

void ats_verify_thread_entry(void *param1, void *param2, void *param3)
{
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(param3);

    LOG_DBG("ATS-verify started");

    while (!finish)
    {
        while (true /*read all events, canceled with break*/)
        {
            uint16_t i;
            unsigned int key = irq_lock();
            if (ats_verify_instance.consumerIdx == ats_verify_instance.producerIdx)
            {
                irq_unlock(key);
                break;
            }
            i = ats_verify_instance.consumerIdx;
            ats_verify_instance.consumerIdx = (ats_verify_instance.consumerIdx + 1) % CONFIG_ATS_VERIFY_NUM_EVENTS;
            irq_unlock(key);

            struct ats_verify_sent_event *evt = &ats_verify_instance.events[i];
            struct ats_verify_stream_t *stream = NULL;
            for (size_t i = 0; i < NUM_STREAMS; i++)
            {
                if (ats_verify_instance.streams[i].streamId == evt->streamId)
                {
                    stream = &ats_verify_instance.streams[i];
                    break;
                }
                else if (ats_verify_instance.streams[i].streamId == NULL)
                {
                    break;
                }
            }
            __ASSERT(stream != NULL, "stream %p for saved timing data not found in verifier", evt->streamId);

            if (evt->elegibilityTime < stream->ats_iface->lastElegibilityTime)
            {
                LOG_WRN("A packet with smaller elegibility time %" PRIu64 " was sent after another packet with %" PRIu64 "", evt->elegibilityTime, stream->ats_iface->lastElegibilityTime);
            }

            uint64_t jitter = evt->timestamp - evt->elegibilityTime;

            uint64_t timeSinceLast = evt->timestamp - stream->lastUpdate;
            uint32_t rate = (uint64_t)(evt->packetSize) * TIME_NS / timeSinceLast;

            int64_t tokensSinceLast = timeSinceLast * stream->tokensPerSecond / TIME_NS;
            stream->bucket = MIN((stream->bucket + tokensSinceLast), stream->burstSize);
            int32_t newBucket = stream->bucket - (int32_t)evt->packetSize;

            LOG_INF("stream %p sent %d bytes (%d B/s); elegible at %" PRIu64 ", sent at %" PRIu64 ", jitter %" PRIu64 ", bucket: %" PRId32 "=>%" PRId32 ", since last: %" PRIu64 "", evt->streamId, evt->packetSize, rate, evt->elegibilityTime, evt->timestamp, jitter, stream->bucket, newBucket, timeSinceLast);

            if (newBucket < 0)
            {
                LOG_ERR("stream %p sent %d bytes when bucket only held %" PRId32 " bytes. ATS CONSTRAINTS VIOLATED", evt->streamId, evt->packetSize, stream->bucket);
                stream->bucket = 0;
            }
            else
            {
                stream->bucket = newBucket;
            }

            stream->lastUpdate = evt->timestamp;
            stream->ats_iface->lastElegibilityTime = evt->elegibilityTime;

            k_yield();
        }
        k_sleep(K_MSEC(100));
    }
}

#ifndef CONFIG_ATS_VERIFY_NO_PROCESSING
K_THREAD_DEFINE(sats_verify_thread_id, 1024,
                ats_verify_thread_entry, NULL, NULL, NULL,
                CONFIG_NUM_PREEMPT_PRIORITIES - 1, 0, 0);
#endif

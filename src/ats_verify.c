#include "ats_verify.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ats_verify, CONFIG_ATS_VERIFY_LOG_LEVEL);

#define NUM_STREAMS CONFIG_ATS_SCHEDULER_INSTANCES
#define TIME_NS 1000000000LL

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
    struct k_spinlock lock;
    struct device *timer;
    struct ats_verify_sent_event events[128];
    uint8_t producerIdx;
    uint8_t consumerIdx;
    struct ats_verify_stream_t streams[NUM_STREAMS];
    uint64_t lastElegibilityTime;
};

struct ats_verify_t ats_verify = {.timer = DEVICE_DT_GET(DT_ALIAS(time))};
static bool finish = false;

void ats_verify_packet_sent(void *streamId, uint16_t packetSize, uint64_t elegibilityTime)
{
    uint8_t i;
    K_SPINLOCK(&ats_verify.lock)
    {
        if (((ats_verify.producerIdx + 1) & 0x7f) == ats_verify.consumerIdx)
        {
            LOG_ERR("verify queue full, dropping event");
            return;
        }
        i = ats_verify.producerIdx;
        ats_verify.producerIdx = (ats_verify.producerIdx + 1) & 0x7f;
    }
    struct ats_verify_sent_event *write = &ats_verify.events[i];
    write->streamId = streamId;
    write->packetSize = packetSize;
    write->elegibilityTime = elegibilityTime;
    counter_get_value_64(ats_verify.timer, &(write->timestamp));
}

void ats_verify_set_stream_parameters(void *streamId, uint32_t tokensPerSecond, uint32_t burstSize)
{
    struct ats_verify_stream_t *stream = NULL;
    bool isNew = false;
    for (size_t i = 0; i < NUM_STREAMS; i++)
    {
        if (ats_verify.streams[i].streamId == streamId)
        {
            stream = &ats_verify.streams[i];
            break;
        }
        else if (ats_verify.streams[i].streamId == NULL)
        {
            stream = &ats_verify.streams[i];
            isNew = true;
            break;
        }
    }
    __ASSERT(stream != NULL, "no free stream in verifier found to set params");

    stream->tokensPerSecond = tokensPerSecond;
    stream->burstSize = burstSize;
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
            uint8_t i;
            K_SPINLOCK(&ats_verify.lock)
            {
                if (ats_verify.consumerIdx == ats_verify.producerIdx)
                {
                    break;
                    K_SPINLOCK_BREAK;
                }
                i = ats_verify.consumerIdx;
                ats_verify.consumerIdx = (ats_verify.consumerIdx + 1) & 0x7f;
            }
            struct ats_verify_sent_event *evt = &ats_verify.events[i];
            struct ats_verify_stream_t *stream = NULL;
            bool isNew = false;
            for (size_t i = 0; i < NUM_STREAMS; i++)
            {
                if (ats_verify.streams[i].streamId == evt->streamId)
                {
                    stream = &ats_verify.streams[i];
                    break;
                }
                else if (ats_verify.streams[i].streamId == NULL)
                {
                    stream = &ats_verify.streams[i];
                    isNew = true;
                    break;
                }
            }
            __ASSERT(stream != NULL, "no free stream in verifier found");

            // assumes only one send interface / pcp class!
            if (evt->elegibilityTime < ats_verify.lastElegibilityTime)
            {
                LOG_WRN("A packet with smaller elegibility time %d was sent after another packet with %d", evt->elegibilityTime, ats_verify.lastElegibilityTime);
            }

            uint64_t jitter = evt->timestamp - evt->elegibilityTime;

            if (!isNew)
            {
                uint64_t timeSinceLast = evt->timestamp - stream->lastUpdate;
                uint32_t rate = (uint64_t)(evt->packetSize) * TIME_NS / timeSinceLast;

                LOG_INF("stream %p sent %d bytes (%d B/s); elegible at %d, sent at %d, jitter %d", evt->streamId, evt->packetSize, rate, evt->elegibilityTime, evt->timestamp, jitter);
            }
            else
            {
                LOG_INF("stream %p sent %d bytes; elegible at %d, sent at %d, jitter %d", evt->streamId, evt->packetSize, evt->elegibilityTime, evt->timestamp, jitter);
            }

                        stream->lastUpdate = evt->timestamp;
            ats_verify.lastElegibilityTime = evt->elegibilityTime;

            k_yield();
        }
        k_sleep(K_MSEC(100));
    }
}

/*K_THREAD_DEFINE(sats_verify_thread_id, 1024,
                ats_verify_thread_entry, NULL, NULL, NULL,
                CONFIG_NUM_PREEMPT_PRIORITIES - 1, 0, 0);*/
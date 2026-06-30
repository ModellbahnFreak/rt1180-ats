#include "ats_verify.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include "ats_time.h"
#include <zephyr/logging/log.h>
#include "constants.h"

LOG_MODULE_REGISTER(ats_verify, CONFIG_ATS_VERIFY_LOG_LEVEL);

#define NUM_STREAMS CONFIG_ATS_SCHEDULER_INSTANCES

static bool finish = false;

void ats_verify_packet_sent(struct ats_verify_t *verifier, void *streamId, uint16_t packetSize, uint64_t elegibilityTime)
{
    uint8_t i;
    unsigned int key = irq_lock();
    if (((verifier->producerIdx + 1) & 0x7f) == verifier->consumerIdx)
    {
        LOG_ERR("verify queue full, dropping event");
        return;
    }
    i = verifier->producerIdx;
    verifier->producerIdx = (verifier->producerIdx + 1) & 0x7f;
    irq_unlock(key);
    struct ats_verify_sent_event *write = &verifier->events[i];
    write->streamId = streamId;
    write->packetSize = packetSize;
    write->elegibilityTime = elegibilityTime;
    ats_time_get(&(write->timestamp));
}

void ats_verify_set_stream_parameters(struct ats_verify_t *verifier, void *streamId, uint32_t tokensPerSecond, uint32_t burstSize)
{
    struct ats_verify_stream_t *stream = NULL;
    bool isNew = false;
    for (size_t i = 0; i < NUM_STREAMS; i++)
    {
        if (verifier->streams[i].streamId == streamId)
        {
            stream = &verifier->streams[i];
            break;
        }
        else if (verifier->streams[i].streamId == NULL)
        {
            stream = &verifier->streams[i];
            isNew = true;
            break;
        }
    }
    __ASSERT(stream != NULL, "no free stream in verifier found to set params");

    stream->tokensPerSecond = tokensPerSecond;
    stream->burstSize = burstSize;
}

void ats_verify_thread_entry(void *verifierPtr, void *param2, void *param3)
{
    ARG_UNUSED(param2);
    ARG_UNUSED(param3);

    struct ats_verify_t *verifier = verifierPtr;

    LOG_DBG("ATS-verify started");

    while (!finish)
    {
        while (true /*read all events, canceled with break*/)
        {
            uint8_t i;
            unsigned int key = irq_lock();
            if (verifier->consumerIdx == verifier->producerIdx)
            {
                break;
                K_SPINLOCK_BREAK;
            }
            i = verifier->consumerIdx;
            verifier->consumerIdx = (verifier->consumerIdx + 1) & 0x7f;
            irq_unlock(key);

            struct ats_verify_sent_event *evt = &verifier->events[i];
            struct ats_verify_stream_t *stream = NULL;
            bool isNew = false;
            for (size_t i = 0; i < NUM_STREAMS; i++)
            {
                if (verifier->streams[i].streamId == evt->streamId)
                {
                    stream = &verifier->streams[i];
                    break;
                }
                else if (verifier->streams[i].streamId == NULL)
                {
                    stream = &verifier->streams[i];
                    isNew = true;
                    break;
                }
            }
            __ASSERT(stream != NULL, "no free stream in verifier found");

            // assumes only one send interface / pcp class!
            if (evt->elegibilityTime < verifier->lastElegibilityTime)
            {
                LOG_WRN("A packet with smaller elegibility time %d was sent after another packet with %d", evt->elegibilityTime, verifier->lastElegibilityTime);
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
            verifier->lastElegibilityTime = evt->elegibilityTime;

            k_yield();
        }
        k_sleep(K_MSEC(100));
    }
}

static K_THREAD_STACK_ARRAY_DEFINE(processing_thread_stack, CONFIG_ATS_STRUCT_ITERABLE_SIZE, 1024);
static struct k_thread processing_thread_data[CONFIG_ATS_STRUCT_ITERABLE_SIZE];
static atomic_t processing_thread_count;

void ats_verify_start_processing(struct ats_verify_t *verifier)
{
#ifndef CONFIG_ATS_VERIFY_NO_PROCESSING
    atomic_val_t i = atomic_inc(&processing_thread_count);

    __ASSERT(i < CONFIG_ATS_STRUCT_ITERABLE_SIZE, "No free ats verifier thread stack. Tried to start verifier %d when maximum count is %d", i, CONFIG_ATS_STRUCT_ITERABLE_SIZE);
    k_tid_t processing_thread_id = k_thread_create(
        &processing_thread_data[i], &processing_thread_stack[i][0], 1024,
        ats_verify_thread_entry, verifier, NULL, NULL,
        CONFIG_NUM_PREEMPT_PRIORITIES - 1, 0, K_NO_WAIT);
#endif
}

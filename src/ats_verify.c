#include "ats_verify.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ats_verify, CONFIG_ATS_VERIFY_LOG_LEVEL);

struct ats_verify_sent_event
{
    void *ats;
    void *group;
    uint64_t timestamp;
    uint16_t packetSize;
};

struct ats_verify_t
{
    struct k_spinlock lock;
    struct device *timer;
    struct ats_verify_sent_event events[128];
    uint8_t producerIdx;
    uint8_t consumerIdx;
};

struct ats_verify_t ats_verify = {.timer = DEVICE_DT_GET(DT_ALIAS(time))};
static bool finish = false;

void ats_verify_packet_sent(void *ats, void *group, uint16_t packetSize)
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
    write->ats = ats;
    write->group = group;
    write->packetSize = packetSize;
    counter_get_value_64(ats_verify.timer, &(write->timestamp));
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
                }
                i = ats_verify.consumerIdx;
                ats_verify.consumerIdx = (ats_verify.consumerIdx + 1) & 0x7f;
            }
            // todo verify ats
        }
    }
}

K_THREAD_DEFINE(sats_verify_thread_id, 1024,
                ats_verify_thread_entry, NULL, NULL, NULL,
                CONFIG_NUM_PREEMPT_PRIORITIES - 1, 0, 0);
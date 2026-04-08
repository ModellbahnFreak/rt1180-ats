#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include "ats.h"

LOG_MODULE_DECLARE(ats, CONFIG_ATS_LOG_LEVEL);

// Actual instances of ats struct in iterable struct section
STRUCT_SECTION_ITERABLE(ats, ats_instance_0);

struct ats *ats_init(struct net_if *iface)
{
    LOG_INF("Initializing ATS struct for iface %s", net_if_get_config(iface)->name);
    LOG_DBG("allocating %d schedulers, %d groups and %d queue slots/group", CONFIG_ATS_SCHEDULER_INSTANCES, CONFIG_ATS_GROUP_INSTANCES, CONFIG_ATS_QUEUE_LENGTH);

    struct ats *newInst = NULL;

    STRUCT_SECTION_FOREACH(ats, instance)
    {
        if (instance->iface == NULL)
        {
            newInst = instance;
            break;
        }
    }
    newInst->iface = iface;

    __ASSERT(newInst != NULL, "No free ats instance found in ats_init");

    return newInst;
}

int ats_register_stream(struct ats *ats_data, uint8_t groupId, uint32_t tokensPerSecond, uint32_t burstSize)
{
    // uint8_t scheduler =
}

int ats_send(char *buf, int len)
{
    k_tid_t appId = k_current_get();

    return 0;
}
#ifndef RT1180_ATS_ATS_H__
#define RT1180_ATS_ATS_H__

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/net/net_if.h>

struct ats_scheduler
{
    const int64_t bucketEmptyTime[CONFIG_ATS_SCHEDULER_INSTANCES];
    const uint32_t tokensPerSecond[CONFIG_ATS_SCHEDULER_INSTANCES]; // committed information rate
    const uint32_t burstSize[CONFIG_ATS_SCHEDULER_INSTANCES];       // comitted burst size
    const uint8_t schedulerGroup[CONFIG_ATS_SCHEDULER_INSTANCES];
    uint8_t nextFreeScheduler; // First scheduler that has not been registered to a stream yet
};

struct ats_groups
{
    const void *queue[CONFIG_ATS_GROUP_INSTANCES][CONFIG_ATS_QUEUE_LENGTH];             // pointer to packet queue
    const uint8_t queue_sched_ids[CONFIG_ATS_GROUP_INSTANCES][CONFIG_ATS_QUEUE_LENGTH]; // scheduler ids of packets in queue
    int queue_read_pos[CONFIG_ATS_GROUP_INSTANCES];
    int queue_write_pos[CONFIG_ATS_GROUP_INSTANCES];
};

struct ats
{
    struct ats_scheduler scheduler;
    struct ats_groups groups;
    struct net_if *iface;
};

struct ats *ats_init(struct net_if *iface);

int ats_register_stream(struct ats *ats_data, uint8_t groupId, uint32_t tokensPerSecond, uint32_t burstSize);

int ats_send(char *buf, int len);

#endif
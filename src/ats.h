#ifndef RT1180_ATS_ATS_H__
#define RT1180_ATS_ATS_H__

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/net/net_if.h>

struct ats_scheduler
{
    int64_t bucketEmptyTime[CONFIG_ATS_SCHEDULER_INSTANCES];
    uint32_t tokensPerSecond[CONFIG_ATS_SCHEDULER_INSTANCES]; // committed information rate
    uint32_t burstSize[CONFIG_ATS_SCHEDULER_INSTANCES];       // comitted burst size
    uint8_t schedulerGroup[CONFIG_ATS_SCHEDULER_INSTANCES];
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

/**
 * @brief Initialize new ATS scheduler structure for a given net_if
 *
 * Tries to initialize the first free ATS structure in memory with the given net_if as upstream.
 * Note: Initialization can't be undone.
 *
 * Errors if no free ATS structure remains
 *
 * @param iface The upstream l2 ethernet interface to send scheduled packets via
 */
struct ats *ats_init(struct net_if *iface);

/**
 * @brief find the ats structure for the given interface
 */
struct ats *ats_get_by_net_if(struct net_if *iface);

/**
 * @brief Allocate the first free ATS scheduler with the given parameters
 *
 * Errors if no free scheduler remains
 * 
 * @param groupId the id of the group to attach the scheduler to. Negative to use the first empty group.
 */
uint8_t ats_register_stream(struct ats *ats_data, int groupId, uint32_t tokensPerSecond, uint32_t burstSize);

/**
 * @brief Send buffer using configured ATS scheduling
 */
int ats_send(struct ats *ats_data, uint8_t schedId, const char *buf, int len);

#endif
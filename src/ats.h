#ifndef RT1180_ATS_ATS_H__
#define RT1180_ATS_ATS_H__

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include "ats_time.h"

struct ats_scheduler
{
    int64_t bucketEmptyTimeTicks;
    uint32_t tokensPerSecond; // committed information rate
    uint32_t burstSize;       // comitted burst size
    int64_t emptyToFullTimeTicks;
    uint8_t schedulerGroup;
};

struct ats_enqued_packet
{
    struct net_pkt *packet;
    int64_t elegibilityTime;
};

struct ats_group
{
    struct k_msgq queue;
    char queueBuffer[CONFIG_ATS_QUEUE_LENGTH * sizeof(struct ats_enqued_packet)]; // (byte) buffer for packet queue
    atomic_t queueLen;
    int64_t groupElegibilityTimeTicks;
    ats_time_t queueHeadElegibilityTime;
    struct k_mutex elegibilityTimeMutex;
    struct ats *parent;
};

struct ats
{
    struct ats_scheduler scheduler[CONFIG_ATS_SCHEDULER_INSTANCES];
    struct ats_group group[CONFIG_ATS_GROUP_INSTANCES];
    struct net_if *iface;
    const struct device *ifaceDevice;
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
 * @brief find the first ats structure for the given interface
 *
 * IMPORTANT: Does not include ats_iface instances.
 */
struct ats *ats_get_by_net_if(struct net_if *iface);

/**
 * @brief Returns the group id for the specified stream
 *
 * @return The id of the group in [0, CONFIG_ATS_GROUP_INSTANCES-1] when scheduler with given id is initialized,
 *          CONFIG_ATS_GROUP_INSTANCES when the scheduler with the gifen id isn't initialized yet.
 */
uint8_t ats_get_group_id(struct ats *ats_data, uint8_t schedId);

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
int ats_send(struct ats *ats_data, uint8_t schedId, struct net_pkt *pkt);

#endif
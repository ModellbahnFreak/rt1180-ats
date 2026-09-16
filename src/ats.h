#ifndef RT1180_ATS_ATS_H__
#define RT1180_ATS_ATS_H__

#include <zephyr/sys/iterable_sections.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include "ats_time.h"

#ifndef CONFIG_ATS_SOFTWARE_DRIVER
#define CONFIG_ATS_QUEUE_LENGTH 0
#define CONFIG_ATS_SCHEDULER_INSTANCES 0
#define CONFIG_ATS_GROUP_INSTANCES 0
#endif

/**
 * ATS scheduler instance (IEEE802.1Qcr 8.6.5.6 a-e)
 * One per stream/flow
 */
struct ats_scheduler
{
    /** State of the schedulers token bucket (via time bucket was last empty) */
    int64_t bucketEmptyTimeTicks;
    /**
     * CommittedInformationRate in Bytes per Second
     *
     * See IEEE802.1Qr 8.6.11.3.6
     */
    uint32_t informationRate;
    /**
     * CommittedBurstSize in Byes
     *
     * See IEEE802.1Qr 8.6.11.3.5
     */
    uint32_t burstSize;
    /**
     * @internal
     * Time for bucket to recover from empty to full in ats_time ticks.
     * Calculated on registering a stream to optimize eligibility time calculation
     * emptyToFullTimeTicks=burstSize/informationRate
     */
    int64_t emptyToFullTimeTicks;
    /**
     * Scheduler group this scheduler instance belongs to
     *
     * See IEEE802.1Qr 8.6.5.6 b)
     */
    uint8_t schedulerGroup;
};

/**
 * Packet and its eligibility time
 * Used for queueing within \ref struct ats_group
 */
struct ats_enqued_packet
{
    struct net_pkt *packet;
    /** Eligibility time in ats_time ticks */
    int64_t elegibilityTime;
};

/**
 * ATS group instance (IEEE802.1Qcr 8.6.5.6 f-h)
 */
struct ats_group
{
    /**
     * FIFO Queue for storing \ref struct ats_enqued_packet until their eligibility
     * Can hold up to CONFIG_ATS_QUEUE_LENGTH packets
     */
    struct k_msgq queue;
    /** @internal (byte) data buffer for packet queue */
    char queueBuffer[CONFIG_ATS_QUEUE_LENGTH * sizeof(struct ats_enqued_packet)];
    /** Current number of enqueued packets */
    atomic_t queueLen;
    /**
     * Latest eligibility time of all schedulers of this group.
     * Enforces FIFO ordering within the group
     *
     * See IEEE802.1Qcr 8.6.11.3.10
     */
    int64_t groupElegibilityTimeTicks;
    /** Eligibility time of current head of queue. (valid=false if queue empty) */
    ats_time_t queueHeadElegibilityTime;
    /** Mutex to make access to groupElegibilityTimeTicks thread safe */
    struct k_mutex elegibilityTimeMutex;
    /**
     * ATS instance this group belongs to
     * Set on initialization \ref struct ats; must not be changed
     */
    struct ats *parent;
};

/**
 * ATS instance
 * One per interface and priority level (actual priority value irrelevant for ATS not part of this struct; set in net_pkt!)
 */
struct ats
{
    /**
     * ATS scheduler instances for this ATS instance
     * Up to CONFIG_ATS_SCHEDULER_INSTANCES
     */
    struct ats_scheduler scheduler[CONFIG_ATS_SCHEDULER_INSTANCES];
    /**
     * ATS group instances for this ATS instance
     * Up to CONFIG_ATS_GROUP_INSTANCES
     */
    struct ats_group group[CONFIG_ATS_GROUP_INSTANCES];
    /** Parent ethernet \ref struct net_if to send packets via */
    struct net_if *iface;
    /** @internal Device used for packet transmission (for DSA interfaces the conduit port device) */
    const struct device *ifaceDevice;
};

#ifdef CONFIG_ATS_SOFTWARE_DRIVER

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
 * @brief find the first ats structure for the given (parent/upstream) interface
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
uint8_t ats_register_scheduler(struct ats *ats_data, int groupId, uint32_t informationRate, uint32_t burstSize);

/**
 * @brief Send buffer using configured ATS scheduling
 */
int ats_send(struct ats *ats_data, uint8_t schedId, struct net_pkt *pkt);

#else // CONFIG_ATS_SOFTWARE_DRIVER

#define ats_init(...) ((struct ats *)NULL);
#define ats_get_by_net_if(...) ((struct ats *)NULL);
#define ats_get_group_id(...) 0
#define ats_register_scheduler(...) 0
#define ats_send(...) 0

#endif // CONFIG_ATS_SOFTWARE_DRIVER

#endif

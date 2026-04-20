#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include "ats.h"

#define TIME_NS 1000000000LL
#define TIME_INVALID -1LL
#define ATS_QUEUE_ARR_LENGTH (CONFIG_ATS_QUEUE_LENGTH + 1)
#define LOG_IF_ERR(err, ...)  \
    if ((err) != 0)           \
    {                         \
        LOG_ERR(__VA_ARGS__); \
    }

LOG_MODULE_REGISTER(ats, CONFIG_ATS_LOG_LEVEL);

// Actual instances of ats struct in iterable struct section
STRUCT_SECTION_ITERABLE(ats, ats_instance_0);

void ats_timer_expiry(struct k_timer *timer);

struct ats *ats_init(struct net_if *iface)
{
    LOG_INF("Initializing ATS struct for iface %s", net_if_get_config(iface)->name);
    LOG_DBG("allocating %d schedulers, %d groups and %d queue slots/group", CONFIG_ATS_SCHEDULER_INSTANCES, CONFIG_ATS_GROUP_INSTANCES, CONFIG_ATS_QUEUE_LENGTH);

    struct ats *newInst = ats_get_by_net_if(iface);
    if (newInst != NULL)
    {
        LOG_DBG("a structure was already allocated for the iface");
        return newInst;
    }

    STRUCT_SECTION_FOREACH(ats, instance)
    {
        if (instance->iface == NULL)
        {
            newInst = instance;
            break;
        }
    }
    newInst->iface = iface;

    __ASSERT(newInst != NULL, "no free ats instance found in ats_init");

    for (int i = 0; i < CONFIG_ATS_SCHEDULER_INSTANCES; i++)
    {
        newInst->scheduler[i].schedulerGroup = CONFIG_ATS_GROUP_INSTANCES;
    }

    for (int i = 0; i < CONFIG_ATS_GROUP_INSTANCES; i++)
    {
        newInst->group[i].parent = newInst;
        struct k_timer *timer = &(newInst->group[i].sendTimer);
        k_timer_init(timer, ats_timer_expiry, NULL);
        k_timer_user_data_set(timer, &(newInst->group[i]));

        k_msgq_init(&(newInst->group[i].queue), newInst->group[i].queueBuffer, sizeof(struct ats_enqued_packet), CONFIG_ATS_QUEUE_LENGTH);
        k_mutex_init(&(newInst->group[i].elegibilityTimeMutex));
        LOG_DBG("For ATS %p: Group %d is %p", newInst, i, &(newInst->group[i]));
    }

    return newInst;
}

struct ats *ats_get_by_net_if(struct net_if *iface)
{
    STRUCT_SECTION_FOREACH(ats, instance)
    {
        if (instance->iface == iface)
        {
            return instance;
        }
    }
    return NULL;
}

uint8_t ats_register_stream(struct ats *ats_data, int groupId, uint32_t tokensPerSecond, uint32_t burstSize)
{
    __ASSERT(ats_data != NULL, "no ats structure passed");
    __ASSERT(groupId < CONFIG_ATS_GROUP_INSTANCES, "invalid ATS group id %d, out of range of %d available groups", groupId, CONFIG_ATS_GROUP_INSTANCES);
    __ASSERT(tokensPerSecond > 0, "committed information rate (CIR, tokensPreSecond) must be greater than 0!");
    __ASSERT(burstSize > 0, "committed burst size (CBS, burstSize) must be greater than 0!");
    int schedulerId = -1;
    for (int i = 0; i < CONFIG_ATS_SCHEDULER_INSTANCES; i++)
    {
        if (ats_data->scheduler[i].tokensPerSecond == 0)
        {
            schedulerId = i;
            break;
        }
    }
    __ASSERT(schedulerId >= 0, "no free ATS scheduler found for iface %s", net_if_get_config(ats_data->iface)->name);

    LOG_INF("Registered ATS stream %d for iface %s", schedulerId, net_if_get_config(ats_data->iface)->name);
    LOG_DBG("using groupId %d, %d tokens/sec, %d bytes burst size", groupId, tokensPerSecond, burstSize);
    if (groupId < 0)
    {
        for (int i = 0; i < CONFIG_ATS_SCHEDULER_INSTANCES; i++)
        {
            if (ats_data->scheduler[schedulerId].schedulerGroup == CONFIG_ATS_GROUP_INSTANCES)
            {
                groupId = i;
                LOG_DBG("found free group %d for scheduler %d", groupId, schedulerId);
                break;
            }
        }
        __ASSERT(groupId >= 0, "no free group without scheduler found to assign to instance %d", schedulerId);
    }
    ats_data->scheduler[schedulerId].bucketEmptyTime = 0;
    ats_data->scheduler[schedulerId].tokensPerSecond = tokensPerSecond;
    ats_data->scheduler[schedulerId].burstSize = burstSize;
    ats_data->scheduler[schedulerId].schedulerGroup = groupId;
    ats_data->scheduler[schedulerId].emptyToFullTimeNs = burstSize * TIME_NS / tokensPerSecond;

    return schedulerId;
}

int ats_send_iface(struct ats_group *group, struct net_pkt *pkt)
{
    // todo: assert correctness here
    LOG_DBG("Group %p sending packet with %d bytes", group, net_pkt_get_len(pkt));
    net_pkt_set_iface(pkt, group->parent->iface);
    return net_send_data(pkt);
}

int64_t ats_calc_elegibility_time(struct ats *ats_data, uint8_t schedId, size_t pktSize, int64_t arrivalNs)
{
    struct ats_scheduler *sched = &(ats_data->scheduler[schedId]);
    struct ats_group *group = &(ats_data->group[sched->schedulerGroup]);
    int64_t pktRecoveryTimeNs = ((uint64_t)pktSize) * TIME_NS / sched->tokensPerSecond;

    k_mutex_lock(&(group->elegibilityTimeMutex), K_FOREVER);
    int64_t schedulerElegebilityTimeNs = sched->bucketEmptyTime + pktRecoveryTimeNs;
    int64_t elegebilityTimeNs = max(max(arrivalNs, schedulerElegebilityTimeNs), group->groupElegibilityTime); // todo: handle time rollover?!
    group->groupElegibilityTime = elegebilityTimeNs;

    int64_t bucketFullTime = sched->bucketEmptyTime + sched->emptyToFullTimeNs;
    if (elegebilityTimeNs < bucketFullTime)
    {
        // bucket wasn't full yet => remove pktSize tokens from bucket by moving bucketEmptyTime
        sched->bucketEmptyTime = schedulerElegebilityTimeNs;
    }
    else
    {
        // bucket was full => subtrack pktSize tokens from bucket and set, bucketEmptyTime accurdingly
        sched->bucketEmptyTime = elegebilityTimeNs - sched->emptyToFullTimeNs + pktRecoveryTimeNs;
    }
    k_mutex_unlock(&(group->elegibilityTimeMutex));

    return elegebilityTimeNs;
}

/**
 * @brief Sends all elegible packets at head of the groups queue.
 *
 * WARNING: This function assumes that the current head of the queue is elegible. Must be ensures by caller (timer or check)
 */
int ats_process_queue(struct ats_group *group, int64_t now)
{
    int ret = 0;
    int64_t timeUntilElegible = 0;
    while (timeUntilElegible <= 0)
    {
        LOG_DBG("Dequeue packet from group %p", group);
        struct ats_enqued_packet pkt;
        if (k_msgq_get(&(group->queue), &pkt, K_NO_WAIT) != 0)
        {
            // no packet in queue => nothing to do
            LOG_DBG("Queue of group %p was empty. Nothing to do", group);
            return 0;
        }
#ifdef CONFIG_ATS_ASSERT_ON_SEND
        __ASSERT(pkt.elegibilityTime >= now, "head of queue was not elegible yet when calling ats_process_queue. elegibilityTime %d, now: %d", pkt.elegibilityTime, now);
#endif

        ret = ats_send_iface(group, pkt.packet);
        LOG_IF_ERR(ret, "error while sending packets from queue of ATS group %p: %d", group, ret);

        atomic_val_t oldLen = atomic_dec(&(group->queueLen));
        if (oldLen <= 1)
        {
            // queue empty => no need to calc elegibility time or stat timer
            LOG_DBG("group %p, new len: 0, queue now empty", group);
            return ret;
        }
        if (k_msgq_peek(&(group->queue), &pkt) != 0)
        {
            // no packet in queue => nothing to do
            LOG_DBG("group %p, new len: 0, queue now empty. This should not be reachable", group);
            return ret;
        }

        // packet in queue => need to send or restart timer
        int64_t nextElegible = pkt.elegibilityTime;
        timeUntilElegible = nextElegible - now;

        LOG_DBG("group %p, new len %d, next elegibility time %d", group, oldLen - 1, nextElegible);
    }

    // queue not empty but next packet not elegible yet
    k_timer_start(&(group->sendTimer), K_NSEC(timeUntilElegible) /*todo: timer with absolute time?!*/, K_NO_WAIT);
    LOG_DBG("group %p, next elegibility time %d, timer set for %d ns", group, (int)timeUntilElegible);
    return ret;
}

void ats_timer_expiry(struct k_timer *timer)
{
    struct ats_group *group = k_timer_user_data_get(timer);
    int64_t now = k_uptime_ticks() * TIME_NS / CONFIG_SYS_CLOCK_TICKS_PER_SEC; // todo see ats_send
    ats_process_queue(group, now);
}

int ats_send(struct ats *ats_data, uint8_t schedId, struct net_pkt *pkt)
{
    int64_t arrivalNs = k_uptime_ticks() * TIME_NS / CONFIG_SYS_CLOCK_TICKS_PER_SEC; // todo check if enough granularity or use counters/timerts (general purpose timer or ptp_clock)
    size_t pktSize = net_pkt_get_len(pkt);                                           // todo: packet size doesn't include mac header?
#ifdef CONFIG_ATS_ASSERT_ON_SEND
    __ASSERT(schedId < CONFIG_ATS_SCHEDULER_INSTANCES, "invalid scheduler id %d out of range (%d)", schedId, CONFIG_ATS_SCHEDULER_INSTANCES);
    __ASSERT(ats_data->scheduler[schedId].tokensPerSecond > 0, "ATS instance %d not registered/initialized", schedId);
    __ASSERT(pktSize <= ats_data->scheduler[schedId].burstSize, "packet size %d exeeds burst size %d of ATS %d", pktSize, ats_data->scheduler[schedId].burstSize, schedId);
#endif

    uint8_t groupId = ats_data->scheduler[schedId].schedulerGroup;
    struct k_msgq *queue = &(ats_data->group[groupId].queue);

    struct ats_enqued_packet packet =
        {
            .packet = pkt,
            .elegibilityTime = ats_calc_elegibility_time(ats_data, schedId, pktSize, arrivalNs),
        };

    if (k_msgq_put(queue, &packet, K_NO_WAIT) != 0)
    {
        // packet queue full, can't process packet (even if it were immediately elegible)
        LOG_WRN("group %d packet queue is full (%d packets). Dropping packet from scheduler %d", groupId, CONFIG_ATS_QUEUE_LENGTH, schedId);
        return -1; // todo: proper error codes
    }

    atomic_val_t oldLen = atomic_inc(&(ats_data->group[groupId].queueLen));
    if (oldLen > 0)
    {
        // queue was not empty => timer already running
        LOG_DBG("group %d, enqueued packet, new len: %d, timer running", groupId, oldLen + 1);
        return 0;
    }

    LOG_DBG("group %d, enqueued paket, new len: %d, next elegibility time %d, empty before", groupId, oldLen + 1, packet.elegibilityTime);

    if (packet.elegibilityTime <= arrivalNs)
    {
        // packet is immediatley elegible => send
        return ats_process_queue(&(ats_data->group[groupId]), arrivalNs);
    }
    else
    {
        // packet isn't elegible yet and no tier is running (queue was empty) => start timer
        int64_t timeUntilElegible = packet.elegibilityTime - arrivalNs;
        k_timer_start(&(ats_data->group[groupId].sendTimer), K_NSEC(timeUntilElegible) /*todo: timer with absolute time?!*/, K_NO_WAIT);
        LOG_DBG("group %p, timer set for %d ns, no timer running before", groupId, packet.elegibilityTime, (int)timeUntilElegible);
        return 0;
    }
}
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include "ats.h"

#define TIME_NS 1000000000
#define ATS_QUEUE_ARR_LENGTH (CONFIG_ATS_QUEUE_LENGTH + 1)

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

/**
 * @brief Sends all elegible packets at head of the groups queue
 *
 * CAUTION: Only call from timer ISR as if the timer preempts another execution of this function,
 * it changes the queueConsumerIndex resulting in outdated values in the other execution and thus undefined behaviour
 */
int ats_process_queue(struct ats_group *group, int64_t now)
{
    while (group->queueConsumerIndex != group->queueProducerIndex)
    {
        int64_t timeUntilElegible = group->queueElegibilityTimes[group->queueConsumerIndex] - now;
        if (timeUntilElegible <= 0)
        {
            struct net_pkt *pkt = group->queue[group->queueConsumerIndex];
            int newRet = ats_send_iface(group, pkt);
            if (newRet != 0)
            {
                LOG_ERR("error while sending packets from queue of ATS group %p: %d", group, newRet);
                return newRet;
            }
        }
        else
        {
            // queue not empty but next packet not elegible yet
            k_timer_start(&(group->sendTimer), K_NSEC(timeUntilElegible) /*todo: timer with absolute time?!*/, K_NO_WAIT);
            LOG_DBG("Group %p queue not empty but head not elegible yet. Reprocessing in %d ns", group, (int)timeUntilElegible);
            return 0;
        }
        group->queueConsumerIndex = (group->queueConsumerIndex + 1) % ATS_QUEUE_ARR_LENGTH;
    }
    return 0;
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
    size_t pktSize = net_pkt_get_len(pkt);
#ifdef CONFIG_ATS_ASSERT_ON_SEND
    __ASSERT(schedId < CONFIG_ATS_SCHEDULER_INSTANCES, "invalid scheduler id %d out of range (%d)", schedId, CONFIG_ATS_SCHEDULER_INSTANCES);
    __ASSERT(ats_data->scheduler[schedId].tokensPerSecond > 0, "ATS instance %d not registered/initialized", schedId);
    __ASSERT(pktSize <= ats_data->scheduler[schedId].burstSize, "packet size %d exeeds burst size %d of ATS %d", pktSize, ats_data->scheduler[schedId].burstSize, schedId);
#endif

    // start of non preemtrible block (only works on single core processors)
    k_tid_t currThread = k_current_get();
    int oldPrio = k_thread_priority_get(currThread);
    k_thread_priority_set(currThread, min(-1, oldPrio));

    uint8_t groupId = ats_data->scheduler[schedId].schedulerGroup;

    if ((ats_data->group[groupId].queueProducerIndex - ats_data->group[groupId].queueConsumerIndex + ATS_QUEUE_ARR_LENGTH) % ATS_QUEUE_ARR_LENGTH >= CONFIG_ATS_QUEUE_LENGTH)
    {
        // packet queue full, can't process packet (even if it were immediately elegible)
        LOG_WRN("queue of ATS group %d is full (%d packets). Dropping packet from scheduler %d", groupId, CONFIG_ATS_QUEUE_LENGTH, schedId);
        return -1; // todo: proper error codes
    }

    int64_t pktRecoveryTimeNs = pktSize * TIME_NS / ats_data->scheduler[schedId].tokensPerSecond;
    int64_t schedulerElegebilityTimeNs = ats_data->scheduler[schedId].bucketEmptyTime + pktRecoveryTimeNs;
    int64_t elegebilityTimeNs = max(max(arrivalNs, schedulerElegebilityTimeNs), ats_data->group[groupId].groupElegibilityTime); // todo: handle time rollover?!
    ats_data->group[groupId].groupElegibilityTime = elegebilityTimeNs;

    int64_t bucketFullTime = ats_data->scheduler[schedId].bucketEmptyTime + ats_data->scheduler[schedId].emptyToFullTimeNs;
    if (elegebilityTimeNs < bucketFullTime)
    {
        // bucket wasn't full yet => remove pktSize tokens from bucket by moving bucketEmptyTime
        ats_data->scheduler[schedId].bucketEmptyTime = schedulerElegebilityTimeNs;
    }
    else
    {
        // bucket was full => subtrack pktSize tokens from bucket and set bucketEmptyTime accurdingly
        ats_data->scheduler[schedId].bucketEmptyTime = elegebilityTimeNs - ats_data->scheduler[schedId].emptyToFullTimeNs + pktRecoveryTimeNs;
    }

    int ret = 0;
    if (elegebilityTimeNs <= arrivalNs)
    {
        // packet immediately elegible => no need to enqueue; can only happen if queue has no packets that also are elegible
        ret = ats_send_iface(&(ats_data->group[groupId]), pkt);
#if CONFIG_ATS_LOG_LEVEL >= LOG_LEVEL_ERR
        if (ret != 0)
        {
            LOG_ERR("error sending packet from ATS group %d, scheduler %d immediately: %d", groupId, schedId, ret);
        }
#endif
    }
    else
    {
        // packet elegible in future => queue
        uint8_t queueIndex = ats_data->group[groupId].queueProducerIndex;
        ats_data->group[groupId].queue[queueIndex] = pkt; // todo: handle net_pkt ref count/pass ref (see https://docs.zephyrproject.org/latest/connectivity/networking/api/net_pkt.html#concurrency-and-thread-safety)
        ats_data->group[groupId].queueElegibilityTimes[queueIndex] = elegebilityTimeNs;
        ats_data->group[groupId].queueProducerIndex = (queueIndex + 1) % ATS_QUEUE_ARR_LENGTH;
        if (ats_data->group[groupId].queueConsumerIndex == queueIndex)
        {
            // queue was empty (before this pkt) => no timer is running
            // thread safe as here we can only be preempted by the timer which would also (re)start the timer because this pkt is already written to the queue
            // AND we can't be preempted by the timer if it's not running (as we are in the critical section)
            k_timer_start(&(ats_data->group[groupId].sendTimer), K_NSEC(elegebilityTimeNs - arrivalNs) /*todo: timer with absolute time?!*/, K_NO_WAIT);
        }
    }

    k_thread_priority_set(currThread, oldPrio);

    return ret;
}
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include "ats.h"

#define TIME_NS 1000000000

LOG_MODULE_REGISTER(ats, CONFIG_ATS_LOG_LEVEL);

// Actual instances of ats struct in iterable struct section
STRUCT_SECTION_ITERABLE(ats, ats_instance_0);

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

int ats_process_queue(struct ats *ats_data, uint8_t groupId, int64_t now)
{
    // todo: check if thread safe?
    // todo: make index access atomic? => what if preempted after reading consumer index BY TIMER?
    size_t consumerIndex = ats_data->groups[groupId].queueConsumerIndex;
    while (consumerIndex != ats_data->groups[groupId].queueProducerIndex)
    {
        if (ats_data->groups[groupId].queueElegibilityTimes[consumerIndex] <= now)
        {
            struct net_pkt *pkt = ats_data->groups[groupId].queue[consumerIndex];
            net_pkt_set_iface(pkt, ats_data->iface);
            int newRet = net_send_data(pkt);
            if (newRet != 0)
            {
                return newRet;
            }
        } else {
            break;
        }
    }
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
    if (ats_data->groups[groupId].queueProducerIndex - ats_data->groups[groupId].queueConsumerIndex >= CONFIG_ATS_QUEUE_LENGTH)
    {
        // packet queue full, can't process packet (even if it were immediately elegible)
        // todo: ckeck if head of queue elegible to clear spot in queue
        return -1; // todo: proper error codes
    }

    int64_t pktRecoveryTimeNs = pktSize * TIME_NS / ats_data->scheduler[schedId].tokensPerSecond;
    int64_t schedulerElegebilityTimeNs = ats_data->scheduler[schedId].bucketEmptyTime + pktRecoveryTimeNs;
    int64_t elegebilityTimeNs = max(max(arrivalNs, schedulerElegebilityTimeNs), ats_data->groups[groupId].groupElegibilityTime);
    ats_data->groups[groupId].groupElegibilityTime = elegebilityTimeNs;

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

    if (schedulerElegebilityTimeNs <= arrivalNs)
    {
        // packet immediately elegible => no need to enqueue
        net_pkt_set_iface(pkt, ats_data->iface);
        return net_send_data(pkt);
    }
    else
    {
        // packet elegible in future => queue
        uint8_t queueIndex = ats_data->groups[groupId].queueProducerIndex++;
        ats_data->groups[groupId].queue[queueIndex] = pkt; // todo: handle net_pkt ref count/pass ref (see https://docs.zephyrproject.org/latest/connectivity/networking/api/net_pkt.html#concurrency-and-thread-safety)
        ats_data->groups[groupId].queueElegibilityTimes[queueIndex] = elegebilityTimeNs;
    }

    k_thread_priority_set(currThread, oldPrio);

    return 0;
}
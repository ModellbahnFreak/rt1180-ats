#include <zephyr/kernel.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/dsa_core.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include "ats.h"
#include "constants.h"
#ifdef CONFIG_ATS_VERIFY_ENABLED
#include "ats_verify.h"
#endif

#define ATS_QUEUE_ARR_LENGTH (CONFIG_ATS_QUEUE_LENGTH + 1)
#define LOG_IF_ERR(err, ...)  \
    if ((err) != 0)           \
    {                         \
        LOG_ERR(__VA_ARGS__); \
    }

#define RETURN_PROCESS_NOW 189 // magic return value for ats_update_counter_alarm to signal to caller to process packet now and no timer was set

LOG_MODULE_REGISTER(ats, CONFIG_ATS_LOG_LEVEL);

// Actual instances of ats struct in iterable struct section
STRUCT_SECTION_ITERABLE_ARRAY(ats, ats_instances, CONFIG_ATS_STRUCT_ITERABLE_SIZE);

void ats_counter_alarm_cb();

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

    if (ats_time_init(ats_counter_alarm_cb))
    {
        LOG_ERR("counter/timer initialization failed!");
        return NULL;
    }

    STRUCT_SECTION_FOREACH(ats, instance)
    {
        if (instance->iface == NULL)
        {
            newInst = instance;
            break;
        }
    }
    __ASSERT(newInst != NULL, "no free ats instance found in ats_init");

    __ASSERT(net_if_l2(iface) == &NET_L2_GET_NAME(ETHERNET), "ATS only works on ethernet interfaces. Given interface wasn't ethernet");

#ifdef CONFIG_NET_DSA
    struct ethernet_context *eth_ctx = net_if_l2_data(iface);
    __ASSERT(eth_ctx->dsa_port != DSA_CONDUIT_PORT, "Given interface was a DSA conduit port. Cannot send via that, use a user/switch port");
    if (eth_ctx->dsa_port == DSA_USER_PORT)
    {
        const struct device *dev = net_if_get_device(iface);
        struct dsa_switch_context *dsa_switch_ctx = dev->data;
        newInst->ifaceDevice = net_if_get_device(dsa_switch_ctx->iface_conduit);
    }
    else
    {
        __ASSERT(eth_ctx->dsa_port == NON_DSA_PORT, "Given interface is of unknown type %d. Not a dsa switch port nor a non-dsa port", eth_ctx->dsa_port);
        newInst->ifaceDevice = net_if_get_device(iface);
    }
#else
    newInst->ifaceDevice = net_if_get_device(iface);
#endif

    newInst->iface = iface;

    for (int i = 0; i < CONFIG_ATS_SCHEDULER_INSTANCES; i++)
    {
        newInst->scheduler[i].schedulerGroup = CONFIG_ATS_GROUP_INSTANCES;
    }

    for (int i = 0; i < CONFIG_ATS_GROUP_INSTANCES; i++)
    {
        newInst->group[i].parent = newInst;
        newInst->group[i].queueHeadElegibilityTime.valid = false;

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

uint8_t ats_get_group_id(struct ats *ats_data, uint8_t schedId)
{
    __ASSERT(schedId >= 0 && schedId < CONFIG_ATS_SCHEDULER_INSTANCES, "Invalid scheduler id %d to get the group id for", schedId);
    return ats_data->scheduler[schedId].schedulerGroup;
}

uint8_t ats_register_scheduler(struct ats *ats_data, int groupId, uint32_t informationRate, uint32_t burstSize)
{
    __ASSERT(ats_data != NULL, "no ats structure passed");
    __ASSERT(groupId < CONFIG_ATS_GROUP_INSTANCES, "invalid ATS group id %d, out of range of %d available groups", groupId, CONFIG_ATS_GROUP_INSTANCES);
    __ASSERT(informationRate > 0, "committed information rate (CIR, informationRate) must be greater than 0!");
    __ASSERT(burstSize > 0, "committed burst size (CBS, burstSize) must be greater than 0!");
    int schedulerId = -1;
    for (int i = 0; i < CONFIG_ATS_SCHEDULER_INSTANCES; i++)
    {
        if (ats_data->scheduler[i].informationRate == 0)
        {
            schedulerId = i;
            break;
        }
    }
    __ASSERT(schedulerId >= 0, "no free ATS scheduler found for iface %s", net_if_get_config(ats_data->iface)->name);

    LOG_INF("Registered ATS stream %d for iface %s", schedulerId, net_if_get_config(ats_data->iface)->name);
    LOG_DBG("using groupId %d, %d tokens/sec, %d bytes burst size", groupId, informationRate, burstSize);
    if (groupId < 0)
    {
        groupId = -1;
        for (int i = 0; i < CONFIG_ATS_SCHEDULER_INSTANCES; i++)
        {
            if (ats_data->scheduler[i].schedulerGroup != CONFIG_ATS_GROUP_INSTANCES)
            {
                groupId = max(ats_data->scheduler[i].schedulerGroup, groupId);
            }
        }
        groupId++;
        __ASSERT(groupId >= 0 && groupId < CONFIG_ATS_GROUP_INSTANCES, "no free group without scheduler found to assign to instance %d", schedulerId);
    }

#ifdef CONFIG_ATS_VERIFY_ENABLED
    ats_verify_set_stream_parameters(&(ats_data->group[groupId]), ats_data, informationRate, burstSize);
#endif

    ats_data->scheduler[schedulerId].bucketEmptyTimeTicks = 0;
    ats_data->scheduler[schedulerId].informationRate = informationRate;
    ats_data->scheduler[schedulerId].burstSize = burstSize;
    ats_data->scheduler[schedulerId].schedulerGroup = groupId;
    ats_data->scheduler[schedulerId].emptyToFullTimeTicks = ((uint64_t)burstSize * (uint64_t)TICKS_PER_SECOND) / informationRate;

    return schedulerId;
}

int ats_send_iface(struct ats_group *group, struct net_pkt *pkt
#ifdef CONFIG_ATS_VERIFY_ENABLED
                   ,
                   uint64_t elegibilityTime
#endif
)
{
    LOG_DBG("Group %p sending packet with %d bytes", group, net_pkt_get_len(pkt));
    net_pkt_set_iface(pkt, group->parent->iface);
    net_pkt_cursor_init(pkt);
#ifdef CONFIG_ATS_VERIFY_ENABLED
    ats_verify_packet_sent(group, pkt, elegibilityTime);
#endif
    const struct ethernet_api *api = group->parent->ifaceDevice->api;
    int res = api->send(group->parent->ifaceDevice, pkt);
    net_pkt_unref(pkt);
    return res;
}

/**
 * @brief Sends all elegible packets at head of the groups queue.
 * @returns >= 0 number of packets left in queue (if > 0 queueHeadElegibilityTime will be next elegibility time)
 */
int ats_process_queue(struct ats_group *group, uint64_t now)
{
    int ret = 0;
    atomic_val_t oldLen = -1;
    now = TICK_NORMALIZE(now);
    while (group->queueHeadElegibilityTime.valid && TICK_LE(TICK_NORMALIZE(group->queueHeadElegibilityTime.tick), now))
    {
        LOG_DBG("Dequeue packet from group %p", group);
        struct ats_enqued_packet pkt;
        if (k_msgq_get(&(group->queue), &pkt, K_NO_WAIT) != 0)
        {
            // no packet in queue => nothing to do
            LOG_DBG("Queue of group %p was empty. Nothing to do", group);
            group->queueHeadElegibilityTime.valid = false;
            return 0;
        }

        ret = ats_send_iface(group, pkt.packet
#ifdef CONFIG_ATS_VERIFY_ENABLED
                             ,
                             pkt.elegibilityTime
#endif
        );
        LOG_IF_ERR(ret, "error while sending packets from queue of ATS group %p: %d", group, ret);

        oldLen = atomic_dec(&(group->queueLen));
        if (oldLen <= 1)
        {
            // queue empty => no need to calc elegibility time or stat timer
            LOG_DBG("group %p, new len: 0, queue now empty", group);
            group->queueHeadElegibilityTime.valid = false;
            return 0;
        }
        if (k_msgq_peek(&(group->queue), &pkt) != 0)
        {
            // no packet in queue => nothing to do
            LOG_DBG("group %p, new len: 0, queue now empty. This should not be reachable", group);
            group->queueHeadElegibilityTime.valid = false;
            return 0;
        }

        // packet in queue => need to send or restart timer
        group->queueHeadElegibilityTime.tick = pkt.elegibilityTime;
        group->queueHeadElegibilityTime.valid = true;
        LOG_DBG("group %p, new len %d, next elegibility time %" PRId64, group, (int)(oldLen - 1), group->queueHeadElegibilityTime.tick);
    }

    // queue not empty but next packet not elegible yet
    return oldLen != -1 ? oldLen - 1 : (group->queueHeadElegibilityTime.valid ? 1 : 0);
}

ats_time_t ats_calc_elegibility_time(struct ats *ats_data, uint8_t schedId, size_t pktSize, uint64_t arrivalTick, uint64_t *bucketEmptyTime)
{
    struct ats_scheduler *sched = &(ats_data->scheduler[schedId]);
    struct ats_group *group = &(ats_data->group[sched->schedulerGroup]);
    uint64_t pktRecoveryTimeTicks = ((uint64_t)pktSize) * TICKS_PER_SECOND / sched->informationRate;

    uint64_t schedulerElegebilityTimeTicks = sched->bucketEmptyTimeTicks + pktRecoveryTimeTicks;
    ats_time_t elegebilityTime = {
        .valid = false,
        .tick = MAX(arrivalTick, MAX(schedulerElegebilityTimeTicks, group->groupElegibilityTimeTicks)),
    };
    if (TICK_ILLEGAL_INTERVAL(arrivalTick, elegebilityTime.tick))
    {
        return elegebilityTime;
    }
    group->groupElegibilityTimeTicks = elegebilityTime.tick;

    uint64_t bucketFullTime = sched->bucketEmptyTimeTicks + sched->emptyToFullTimeTicks;
    if (elegebilityTime.tick < bucketFullTime)
    {
        // bucket wasn't full yet => remove pktSize tokens from bucket by moving bucketEmptyTime
        *bucketEmptyTime = schedulerElegebilityTimeTicks;
    }
    else
    {
        // bucket was full => subtrack pktSize tokens from bucket and set, bucketEmptyTime accurdingly
        *bucketEmptyTime = elegebilityTime.tick - sched->emptyToFullTimeTicks + pktRecoveryTimeTicks;
    }

    elegebilityTime.valid = true;
    return elegebilityTime;
}

int ats_update_counter_alarm_tmp(uint64_t nextElegibilityTime, bool timerExpired, uint64_t *outUsedNow)
{
    uint64_t counterVal;
    if (outUsedNow == NULL)
    {
        outUsedNow = &counterVal;
    }
    ats_time_get(outUsedNow);
    *outUsedNow = TICK_NORMALIZE(*outUsedNow);
    nextElegibilityTime = TICK_NORMALIZE(nextElegibilityTime);
    ats_time_t alarm_time;
    ats_time_get_alarm(&alarm_time);
    LOG_DBG("now: %" PRIu64 ", elegible: %" PRIu64 ", alarm valid: %d, alarm time: %" PRIu64, *outUsedNow, nextElegibilityTime, alarm_time.valid, alarm_time.tick);

    if (TICK_LE(nextElegibilityTime, *outUsedNow))
    {
        LOG_DBG("timer update: packet already elegible, not updating timer");
        return RETURN_PROCESS_NOW;
    }
    if (timerExpired || !alarm_time.valid)
    {
        // if elegibility time is before now + epsilon (closer than time needed to update OR already past)
        if (TICK_LE(nextElegibilityTime, *outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS))
        {
            // counter alarm time is past and elegibility time is closer than epsilon => update to now+epsilon
            LOG_DBG("timer update: update to now+epsilon (%" PRId64 "); reason: timer expired and next elegibility time (%" PRId64 ") closer than epsilon; N'-E=%" PRId64,
                    TICK_NORMALIZE(*outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS),
                    nextElegibilityTime,
                    TICK_REL_TIME(nextElegibilityTime, TICK_NORMALIZE(*outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS)));
            return ats_time_set_alarm(*outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS);
        }
        else
        {
            // counter alarm time is past and enough time to elegibility time to update => update to elegibility time
            LOG_DBG("timer update: update to next elegibility (%" PRId64 "); reason: timer expired and next elegibility not too close; E-N=%" PRId64,
                    nextElegibilityTime,
                    TICK_REL_TIME(*outUsedNow, nextElegibilityTime));
            return ats_time_set_alarm(nextElegibilityTime);
        }
    }
    else
    {
        // if timer does not expire within the time needed to update it (alarm_time outside epsilon from now)
        if (TICK_GT(alarm_time.tick, *outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS))
        {
            // if elegibility time is before now + epsilon (closer than time needed to update OR already past)
            if (TICK_LE(nextElegibilityTime, *outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS))
            {
                // elegibility time already past or too close to update AND counter alarm not within epsilon => update alarm time to now+epsilon
                LOG_DBG("timer update: update to now+epsilon (%" PRId64 "); reason: next elegibility time (%" PRId64 ") closer than epsilon; N'-E=%" PRId64,
                        TICK_NORMALIZE(*outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS),
                        nextElegibilityTime,
                        TICK_REL_TIME(nextElegibilityTime, TICK_NORMALIZE(*outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS)));
                return ats_time_set_alarm(*outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS);
            }
            else
            {
                // if elegibility time is closer than current alarm time
                if (TICK_LE(nextElegibilityTime, alarm_time.tick))
                {
                    // => new elegibility time is closer than current alarm time but far enough to be able to update the alarm time => update alarm to elegibility time
                    LOG_DBG("timer update: update to next elegibility (%" PRId64 "); reason: next elegibility time closer than counter (%" PRId64 ") but not too close; C-E=%" PRId64,
                            nextElegibilityTime,
                            alarm_time.tick,
                            TICK_REL_TIME(nextElegibilityTime, alarm_time.tick));
                    return ats_time_set_alarm(nextElegibilityTime);
                }
                else
                {
                    // => new elegibility time is farther than next alarm time => no action required
                    LOG_DBG("timer update: no action; reason: timer not expired, next elegibility time (%" PRId64 ") greater than counter (%" PRId64 "); E-C=%" PRId64,
                            nextElegibilityTime,
                            alarm_time.tick,
                            TICK_REL_TIME(alarm_time.tick, nextElegibilityTime));
                    return 0;
                }
            }
        }
        else
        {
            // no action (except maybe if E < N; TODO)
            LOG_DBG("timer update: no action; reason: timer not expired but expiring within <epsilon from now; N'-C=%" PRId64 "",
                    TICK_REL_TIME(alarm_time.tick, *outUsedNow + CONFIG_ATS_TIMER_EPSILON_TICKS));
            return 0;
        }
    }
    return -1; // unreachable
}

int ats_update_counter_alarm(uint64_t nextElegibilityTime, bool timerExpired, uint64_t *outUsedNow)
{
    uint64_t timeAfter;
    int res = ats_update_counter_alarm_tmp(nextElegibilityTime, timerExpired, outUsedNow);
    ats_time_get(&timeAfter);
    LOG_DBG("time needed for update: %" PRIu64, TICK_REL_TIME(*outUsedNow, timeAfter));
    return res;
}

void ats_counter_alarm_cb(uint64_t ticks)
{
    LOG_DBG("counter alarm at %" PRId64, ticks);
    ats_time_t nextElegibilityTime = {.valid = false};
    do
    {
        nextElegibilityTime.valid = false;
        STRUCT_SECTION_FOREACH(ats, ats_data)
        {
            for (uint8_t i = 0; i < CONFIG_ATS_GROUP_INSTANCES; i++)
            {
                if (ats_process_queue(&(ats_data->group[i]), ticks) > 0)
                {
                    if (nextElegibilityTime.valid)
                    {
                        nextElegibilityTime.tick = TICK_MIN(nextElegibilityTime.tick, TICK_NORMALIZE(ats_data->group[i].queueHeadElegibilityTime.tick));
                    }
                    else
                    {
                        nextElegibilityTime.tick = TICK_NORMALIZE(ats_data->group[i].queueHeadElegibilityTime.tick);
                        nextElegibilityTime.valid = true;
                    }
                }
            }
        }
    } while (nextElegibilityTime.valid && ats_update_counter_alarm(nextElegibilityTime.tick, true, &ticks) == RETURN_PROCESS_NOW);
    if (!nextElegibilityTime.valid)
    {
        ats_time_cancel_alarm();
    }
}

int ats_send(struct ats *ats_data, uint8_t schedId, struct net_pkt *pkt)
{
    uint64_t now;
    ats_time_get(&now);
    size_t pktSize = net_pkt_get_len(pkt);
#ifdef CONFIG_ATS_ASSERT_ON_SEND
    __ASSERT(net_pkt_lladdr_dst(pkt)->len > 0, "packet to be sent via ATS isn't ethernet processed yet! Use upper layers of network stack.");
    __ASSERT(schedId < CONFIG_ATS_SCHEDULER_INSTANCES, "invalid scheduler id %d out of range (%d)", schedId, CONFIG_ATS_SCHEDULER_INSTANCES);
    __ASSERT(ats_data->scheduler[schedId].informationRate > 0, "ATS instance %d not registered/initialized", schedId);
    __ASSERT(pktSize <= ats_data->scheduler[schedId].burstSize, "packet size %d exeeds burst size %d of ATS %d", pktSize, ats_data->scheduler[schedId].burstSize, schedId);
#endif

    uint8_t groupId = ats_data->scheduler[schedId].schedulerGroup;
    struct k_msgq *queue = &(ats_data->group[groupId].queue);
    uint64_t newBucketEmptyTime;

    // k_mutex_lock(&(ats_data->group[groupId].elegibilityTimeMutex), K_FOREVER);
    ats_time_t elegibilityTime = ats_calc_elegibility_time(ats_data, schedId, pktSize, now, &newBucketEmptyTime);
    if (!elegibilityTime.valid)
    {
        // k_mutex_unlock(&(ats_data->group[groupId].elegibilityTimeMutex));
        LOG_WRN("group %d packet elegibility time (%" PRId64 ") more than %" PRId64 " ticks after now (%" PRId64 ") => longer than absolut maximum residence. Dropping packet from scheduler %d", groupId, elegibilityTime.tick, TICK_MAX_INTERVAL, now, schedId);
        return -1; // todo: proper error codes
    }
    struct ats_enqued_packet packet =
        {
            .packet = pkt,
            .elegibilityTime = TICK_NORMALIZE(elegibilityTime.tick),
    };

    if (k_msgq_put(queue, &packet, K_NO_WAIT) != 0)
    {
        // packet queue full, can't process packet (even if it were immediately elegible)
        // k_mutex_unlock(&(ats_data->group[groupId].elegibilityTimeMutex));
        LOG_INF("group %d packet queue is full (%d packets). Dropping packet from scheduler %d", groupId, CONFIG_ATS_QUEUE_LENGTH, schedId);
        return -1; // todo: proper error codes
    }
    ats_data->scheduler[schedId].bucketEmptyTimeTicks = newBucketEmptyTime;
    ats_data->group[groupId].groupElegibilityTimeTicks = elegibilityTime.tick;
    // k_mutex_unlock(&(ats_data->group[groupId].elegibilityTimeMutex));

#ifdef CONFIG_ATS_VERIFY_ENABLED
    ats_verify_packet_enqueued(&(ats_data->group[groupId]), pkt, now);
#endif

    net_pkt_ref(pkt);

    atomic_val_t oldLen = atomic_inc(&(ats_data->group[groupId].queueLen));
    if (oldLen > 0)
    {
        // queue was not empty => timer already running
        LOG_DBG("group %d, enqueued packet, new len: %d, timer running", groupId, (int)(oldLen + 1));
        return 0;
    }

    LOG_DBG("group %d, enqueued paket, new len: %d, next elegibility time %" PRId64 ", empty before", groupId, (int)(oldLen + 1), packet.elegibilityTime);

    ats_data->group[groupId].queueHeadElegibilityTime.tick = packet.elegibilityTime;
    ats_data->group[groupId].queueHeadElegibilityTime.valid = true;

    // packet isn't elegible yet and queue was empty => check if timer need updating
    unsigned int lockI = irq_lock();
    int updateRes = ats_update_counter_alarm(packet.elegibilityTime, false, &now);
    irq_unlock(lockI);
    if (updateRes == RETURN_PROCESS_NOW)
    {
        uint8_t numRemaining = ats_process_queue(&(ats_data->group[groupId]), now);
        if (numRemaining > 0)
        {
            lockI = irq_lock();
            updateRes = ats_update_counter_alarm(ats_data->group[groupId].queueHeadElegibilityTime.tick, false, &now);
            irq_unlock(lockI);
        }
    }
    // LOG_DBG("group %p, timer set for %" PRId64 " ns, no timer running before", groupId, timeUntilElegible);
    return 0;
}

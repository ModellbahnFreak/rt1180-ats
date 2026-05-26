#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/drivers/counter.h>
#include "ats.h"
#ifdef CONFIG_ATS_VERIFY_ENABLED
#include "ats_verify.h"
#endif

#define ATS_COUNTER DT_ALIAS(time)

#define TIME_NS 1000000000LL
#define TICKS_PER_SECOND (DT_PROP(ATS_COUNTER, gptfreq))
/**
 * The time overflow/modulo in ticks (internal time unit)
 * Equal to casting to uint32 (as thats te overflow of the timer)
 * NOTE: Customization not possible because there would be a mismatch between counter overflow and time overflow
 */
#define TICK_MODULO 0xffffffffLL
/**
 * The number of ns it takes the 32bit counter to overflow.
 * (Absolut) Maximum residence duration will be half of this interval as durations longer than that can't be reliably compared.
 *
 * CAUTION: This value is only to be used for asserts (not for actual time calculation) as it might not devide cleanly!
 */
#define TIME_MODULO (TICK_MODULO * TIME_NS / TICKS_PER_SECOND)
#define CONFIG_ATS_TIMER_EPSILON_TICKS ((uint64_t)CONFIG_ATS_TIMER_EPSILON_NS * (uint64_t)TICKS_PER_SECOND / TIME_NS)

#define TIME_INVALID -1LL
#define ATS_QUEUE_ARR_LENGTH (CONFIG_ATS_QUEUE_LENGTH + 1)
#define LOG_IF_ERR(err, ...)  \
    if ((err) != 0)           \
    {                         \
        LOG_ERR(__VA_ARGS__); \
    }
#define TICK_LE(a, b) ((((b) - (a) + TICK_MODULO) % TICK_MODULO < (TICK_MODULO / 2)) ? true : false)
#define TICK_GT(a, b) ((((b) - (a) + TICK_MODULO) % TICK_MODULO > (TICK_MODULO / 2)) ? true : false)
#define TICK_MAX(a, b) ((((b) - (a) + TICK_MODULO) % TICK_MODULO < (TICK_MODULO / 2)) ? (b) : (a))
#define TICK_REL_TIME(a, b) (((b) - (a) + TICK_MODULO) % TICK_MODULO)

#define RETURN_PROCESS_NOW 189 // magic return value for ats_update_counter_alarm to signal to caller to process packet now and no timer was set

LOG_MODULE_REGISTER(ats, CONFIG_ATS_LOG_LEVEL);

// Actual instances of ats struct in iterable struct section
STRUCT_SECTION_ITERABLE(ats, ats_instance_0);

const static struct device *ats_counter_dev = DEVICE_DT_GET(ATS_COUNTER);
static uint64_t ats_counter_alarm_time;
static uint64_t ats_counter_time_msb;

void ats_timer_expiry(struct k_timer *timer);

void ats_counter_top_cb(const struct device *dev, void *user_data);
void ats_counter_alarm_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks, void *user_data);

struct ats *ats_init(struct net_if *iface)
{
    LOG_INF("Initializing ATS struct for iface %s", net_if_get_config(iface)->name);
    LOG_DBG("allocating %d schedulers, %d groups and %d queue slots/group", CONFIG_ATS_SCHEDULER_INSTANCES, CONFIG_ATS_GROUP_INSTANCES, CONFIG_ATS_QUEUE_LENGTH);
    // todo: remove
    LOG_DBG("Counter frequency from DT: " PRId64 " ; from api: " PRId64, TICKS_PER_SECOND, counter_get_frequency(ats_counter_dev));

    struct ats *newInst = ats_get_by_net_if(iface);
    if (newInst != NULL)
    {
        LOG_DBG("a structure was already allocated for the iface");
        return newInst;
    }

    if (!device_is_ready(ats_counter_dev))
    {
        LOG_ERR("gpt device not ready");
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
    newInst->iface = iface;

    __ASSERT(newInst != NULL, "no free ats instance found in ats_init");

    for (int i = 0; i < CONFIG_ATS_SCHEDULER_INSTANCES; i++)
    {
        newInst->scheduler[i].schedulerGroup = CONFIG_ATS_GROUP_INSTANCES;
    }

    for (int i = 0; i < CONFIG_ATS_GROUP_INSTANCES; i++)
    {
        newInst->group[i].parent = newInst;

        k_msgq_init(&(newInst->group[i].queue), newInst->group[i].queueBuffer, sizeof(struct ats_enqued_packet), CONFIG_ATS_QUEUE_LENGTH);
        k_mutex_init(&(newInst->group[i].elegibilityTimeMutex));
        LOG_DBG("For ATS %p: Group %d is %p", newInst, i, &(newInst->group[i]));
    }

    struct counter_top_cfg topCfg = {
        .ticks = counter_get_top_value(ats_counter_dev),
        .callback = ats_counter_top_cb};
    counter_set_top_value(ats_counter_dev, &topCfg);

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
        for (int i = 0; i < CONFIG_ATS_GROUP_INSTANCES; i++)
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

#ifdef CONFIG_ATS_VERIFY_ENABLED
    ats_verify_set_stream_parameters(&(ats_data->group[groupId]), tokensPerSecond, burstSize);
#endif

    ats_data->scheduler[schedulerId].bucketEmptyTimeTicks = 0;
    ats_data->scheduler[schedulerId].tokensPerSecond = tokensPerSecond;
    ats_data->scheduler[schedulerId].burstSize = burstSize;
    ats_data->scheduler[schedulerId].schedulerGroup = groupId;
    ats_data->scheduler[schedulerId].emptyToFullTimeTicks = ((uint64_t)burstSize * (uint64_t)TICKS_PER_SECOND) / tokensPerSecond;

    return schedulerId;
}

int ats_send_iface(struct ats_group *group, struct net_pkt *pkt
#ifdef CONFIG_ATS_VERIFY_ENABLED
                   ,
                   uint64_t elegibilityTime
#endif
)
{
    // todo: assert correctness here
    LOG_DBG("Group %p sending packet with %d bytes", group, net_pkt_get_len(pkt));
    net_pkt_set_iface(pkt, group->parent->iface);
#ifdef CONFIG_ATS_VERIFY_ENABLED
    ats_verify_packet_sent(group, net_pkt_get_len(pkt), elegibilityTime);
#endif
    return net_send_data(pkt);
}

/**
 * @brief Sends all elegible packets at head of the groups queue.
 *
 * WARNING: This function assumes that the current head of the queue is elegible. Must be ensures by caller (timer or check)
 */
int ats_process_queue(struct ats_group *group, uint64_t now)
{
    int ret = 0;
    while (TICK_LE(group->queueHeadElegibilityTime, now))
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
        __ASSERT(pkt.elegibilityTime <= now, "head of queue was not elegible yet when calling ats_process_queue. elegibilityTime %" PRId64 ", now: %" PRId64 "", pkt.elegibilityTime, now);
#endif

        ret = ats_send_iface(group, pkt.packet
#ifdef CONFIG_ATS_VERIFY_ENABLED
                             ,
                             pkt.elegibilityTime
#endif
        );
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
        group->queueHeadElegibilityTime = pkt.elegibilityTime;
        LOG_DBG("group %p, new len %d, next elegibility time %" PRId64, group, (int)(oldLen - 1), group->queueHeadElegibilityTime);
    }

    // queue not empty but next packet not elegible yet
    return ret;
}

uint64_t ats_calc_elegibility_time(struct ats *ats_data, uint8_t schedId, size_t pktSize, uint64_t arrivalTick)
{
    struct ats_scheduler *sched = &(ats_data->scheduler[schedId]);
    struct ats_group *group = &(ats_data->group[sched->schedulerGroup]);
    uint64_t pktRecoveryTimeTicks = ((uint64_t)pktSize) * TICKS_PER_SECOND / sched->tokensPerSecond;

    k_mutex_lock(&(group->elegibilityTimeMutex), K_FOREVER);
    uint64_t schedulerElegebilityTimeTicks = sched->bucketEmptyTimeTicks + pktRecoveryTimeTicks;
#ifdef CONFIG_ATS_ASSERT_ON_SEND
    __ASSERT((schedulerElegebilityTimeTicks - arrivalTick) < TICK_MODULO / 2, "elegibility time more than modulo time after now => longer than absolut maximum residence");
#endif
    uint64_t elegebilityTimeTicks = max(arrivalTick, max(schedulerElegebilityTimeTicks, group->groupElegibilityTimeTicks));
    group->groupElegibilityTimeTicks = elegebilityTimeTicks;

    uint64_t bucketFullTime = sched->bucketEmptyTimeTicks + sched->emptyToFullTimeTicks;
    if (elegebilityTimeTicks < bucketFullTime)
    {
        // bucket wasn't full yet => remove pktSize tokens from bucket by moving bucketEmptyTime
        sched->bucketEmptyTimeTicks = schedulerElegebilityTimeTicks;
    }
    else
    {
        // bucket was full => subtrack pktSize tokens from bucket and set, bucketEmptyTime accurdingly
        sched->bucketEmptyTimeTicks = elegebilityTimeTicks - sched->emptyToFullTimeTicks + pktRecoveryTimeTicks;
    }
    k_mutex_unlock(&(group->elegibilityTimeMutex));

    return elegebilityTimeTicks % TICK_MODULO;
}

int ats_update_counter_alarm(uint64_t nextElegibilityTime, bool timerExpired, uint32_t *outUsedNow)
{
    uint32_t counterVal;
    int res = counter_get_value(ats_counter_dev, &counterVal);
    if (res)
    {
        LOG_ERR("Error reading current time: %d", res);
        return -1;
    }
    if (outUsedNow != NULL)
    {
        (*outUsedNow) = counterVal;
    }
    if (TICK_LE(nextElegibilityTime, counterVal))
    {
        return RETURN_PROCESS_NOW;
    }
    if (timerExpired)
    {
        // if elegibility time is before now + epsilon (closer than time needed to update OR already past)
        if (TICK_LE(nextElegibilityTime, counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS))
        {
            // counter alarm time is past and elegibility time is closer than epsilon => update to now+epsilon
            LOG_DBG("timer update: update to now+epsilon (" PRId64 "); reason: timer expired and next elegibility time (" PRId64 ") closer than epsilon; N'-E" PRId64, (counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS) % TICK_MODULO, nextElegibilityTime, TICK_REL_TIME(nextElegibilityTime, (counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS) % TICK_MODULO));
            ats_counter_alarm_time = (counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS) % TICK_MODULO;
        }
        else
        {
            // counter alarm time is past and enough time to elegibility time tto update => update to elegibility time
            LOG_DBG("timer update: update to next elegibility (" PRId64 "); reason: timer expired and next elegibility not too close; delta: " PRId64, nextElegibilityTime, TICK_REL_TIME(ats_counter_alarm_time, nextElegibilityTime));
            ats_counter_alarm_time = nextElegibilityTime;
        }
    }
    else
    {
        // if timer does not expire within the time needed to update it (ats_counter_alarm_time outside epsilon from now)
        if (TICK_GT(ats_counter_alarm_time, counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS))
        {
            // if elegibility time is before now + epsilon (closer than time needed to update OR already past)
            if (TICK_LE(nextElegibilityTime, counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS))
            {
                // elegibility time already past or too close to update AND counter alarm not within epsilon => update alarm time to now+epsilon
                LOG_DBG("timer update: update to now+epsilon (" PRId64 "); reason: next elegibility time (" PRId64 ") closer than epsilon; N'-E" PRId64, (counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS) % TICK_MODULO, nextElegibilityTime, TICK_REL_TIME(nextElegibilityTime, (counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS) % TICK_MODULO));
                ats_counter_alarm_time = (counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS) % TICK_MODULO;
            }
            else
            {
                // if elegibility time is closer than current alarm time
                if (TICK_LE(nextElegibilityTime, ats_counter_alarm_time))
                {
                    // => new elegibility time is closer than current alarm time but far enough to be able to update the alarm time => update alarm to elegibility time
                    LOG_DBG("timer update: update to next elegibility (" PRId64 "); reason: next elegibility time closer than counter (" PRId64 ") but not too close; C-E=" PRId64, nextElegibilityTime, ats_counter_alarm_time, TICK_REL_TIME(nextElegibilityTime, ats_counter_alarm_time));
                    ats_counter_alarm_time = nextElegibilityTime;
                }
                else
                {
                    // => new elegibility time is farther than next alarm time => no action required
                    LOG_DBG("timer update: no action; reason: timer not expired, next elegibility time (" PRId64 ") greater than counter (" PRId64 "); E-C" PRId64, nextElegibilityTime, ats_counter_alarm_time, TICK_REL_TIME(ats_counter_alarm_time, nextElegibilityTime));
                    return 0;
                }
            }
        }
        else
        {
            // no action (except maybe if E < N; TODO)
            LOG_DBG("timer update: no action; reason: timer not expired but expiring within <epsilon from now (" PRId64 ")", TICK_REL_TIME(ats_counter_alarm_time, counterVal + CONFIG_ATS_TIMER_EPSILON_TICKS));
            return 0;
        }
    }
    struct counter_alarm_cfg alarm = {
        .callback = ats_counter_alarm_cb,
        .flags = COUNTER_ALARM_CFG_ABSOLUTE,
        .ticks = ats_counter_alarm_time};

    counter_cancel_channel_alarm(ats_counter_dev, 0);
    return counter_set_channel_alarm(ats_counter_dev, 0, &alarm);
}

void ats_counter_top_cb(const struct device *dev, void *user_data)
{
    ats_counter_time_msb++;
}

void ats_counter_alarm_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks, void *user_data)
{
    uint64_t nextElegibilityTime = TIME_INVALID;
    do
    {
        STRUCT_SECTION_FOREACH(ats, ats_data)
        {
            for (uint8_t i = 0; i < CONFIG_ATS_GROUP_INSTANCES; i++)
            {
                if (ats_data->group[i].queueHeadElegibilityTime <= ticks)
                {
                    ats_process_queue(&(ats_data->group[i]), ticks); // todo??
                }
            }
        }
    } while (nextElegibilityTime != TIME_INVALID && ats_update_counter_alarm(nextElegibilityTime, true, &ticks) == RETURN_PROCESS_NOW);
}

int ats_send(struct ats *ats_data, uint8_t schedId, struct net_pkt *pkt)
{
    uint32_t counterVal = 0;
    uint64_t counteMsb = ats_counter_time_msb; // todo: not thread safe
    int res = counter_get_value(ats_counter_dev, &counterVal);
    if (res)
    {
        LOG_ERR("Error reading current time: %d", res);
        return -1;
    }
    size_t pktSize = net_pkt_get_len(pkt); // todo: packet size doesn't include mac header?
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
            .elegibilityTime = ats_calc_elegibility_time(ats_data, schedId, pktSize, (counteMsb << 32) | counterVal),
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
        LOG_DBG("group %d, enqueued packet, new len: %d, timer running", groupId, (int)(oldLen + 1));
        return 0;
    }

    LOG_DBG("group %d, enqueued paket, new len: %d, next elegibility time %" PRId64 ", empty before", groupId, (int)(oldLen + 1), packet.elegibilityTime);

    ats_data->group[groupId].queueHeadElegibilityTime = packet.elegibilityTime;

    // packet isn't elegible yet and queue was empty => check if timer need updating
    k_sched_lock();
    int updateRes = ats_update_counter_alarm(packet.elegibilityTime, false, &counterVal);
    k_sched_unlock();
    if (updateRes == RETURN_PROCESS_NOW)
    {
        ats_process_queue(&(ats_data->group[groupId]), counterVal);
    }
    // LOG_DBG("group %p, timer set for %" PRId64 " ns, no timer running before", groupId, timeUntilElegible);
    return 0;
}

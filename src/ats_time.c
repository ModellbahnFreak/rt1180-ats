#include "ats_time.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ats_time, CONFIG_ATS_LOG_LEVEL);

#if DT_NODE_EXISTS(ATS_COUNTER)
const static struct device *ats_time_dev = DEVICE_DT_GET(ATS_COUNTER);
#endif
static ats_time_t ats_time_alarm_time = {.valid = false};
static ats_time_alarm_callback_t ats_time_callback = NULL;

#if DT_NODE_HAS_COMPAT(ATS_COUNTER, nxp_imx_gpt)

#include <zephyr/drivers/counter.h>

static uint64_t ats_time_msb;

void ats_time_counter_top_cb(const struct device *dev, void *user_data)
{
    ats_time_msb++;
}

static void ats_time_counter_alarm_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks, void *user_data)
{
    uint64_t counterMsb = ats_time_msb;
    ats_time_alarm_time.valid = false;
    if (ats_time_callback != NULL)
    {
        ats_time_callback(ticks | (counterMsb << 32));
    }
}

int ats_time_init(ats_time_alarm_callback_t callback)
{
    if (!device_is_ready(ats_time_dev))
    {
        return -1;
    }

    if (ats_time_callback != NULL)
    {
        ats_time_callback = callback;
    }

    LOG_DBG("Using gpt as time source with frequency %" PRId32 " Hz as", TICKS_PER_SECOND);

    struct counter_top_cfg topCfg = {
        .ticks = counter_get_top_value(ats_time_dev),
        .callback = ats_time_counter_top_cb};
    if (counter_set_top_value(ats_time_dev, &topCfg))
    {
        return -2;
    }
    return counter_start(ats_time_dev);
}

void ats_time_get(uint64_t *now)
{
    uint64_t counterMsb = ats_time_msb; // todo: not thread safe
    uint32_t counterValue;
    int res = counter_get_value(ats_time_dev, &counterValue);
    if (res)
    {
        LOG_ERR("Error reading current time: %d", res);
        return;
    }
    *(now) = counterValue | (counterMsb << 32);
}

int ats_time_set_alarm(uint64_t time)
{
    time = TICK_NORMALIZE(time);
    struct counter_alarm_cfg alarm = {
        .callback = ats_time_counter_alarm_cb,
        .flags = COUNTER_ALARM_CFG_ABSOLUTE,
        .ticks = time};

    counter_cancel_channel_alarm(ats_time_dev, 0);
    ats_time_alarm_time.valid = true;
    ats_time_alarm_time.tick = time;
    return counter_set_channel_alarm(ats_time_dev, 0, &alarm);
}

int ats_time_cancel_alarm()
{
    counter_cancel_channel_alarm(ats_time_dev, 0);
    ats_time_alarm_time.valid = true;
}

#elif DT_NODE_HAS_COMPAT(ATS_COUNTER, nxp_netc_ptp_clock)
#include "ptp_clock.h"
#include <zephyr/drivers/counter.h>

const static struct device *ats_time_gpt_dev = DEVICE_DT_GET(ATS_TIME_GPT);

// static void ats_time_ptp_alarm_cb(const struct device *dev)
static void ats_time_counter_alarm_cb(const struct device *dev, uint8_t chan_id, uint32_t ticks, void *user_data)
{
    ats_time_alarm_time.valid = false;
    if (ats_time_callback != NULL)
    {
        uint64_t now;
        ats_time_get(&now);
        ats_time_callback(now);
    }
}

int ats_time_init(ats_time_alarm_callback_t callback)
{
    if (!device_is_ready(ats_time_dev))
    {
        LOG_ERR("ptp clock device not ready");
        return -1;
    }
    if (!device_is_ready(ats_time_gpt_dev))
    {
        LOG_ERR("gpt device not ready");
        return -1;
    }

    LOG_DBG("Using ptp as time source and gpt as interrupt timer");

    if (ats_time_callback != NULL)
    {
        ats_time_callback = callback;
    }

    return counter_start(ats_time_gpt_dev);
}

void ats_time_get(uint64_t *now)
{
    ptp_clock_get_ns(ats_time_dev, now);
}

int ats_time_set_alarm(uint64_t time)
{
    uint64_t now;
    ats_time_get(&now);
    time = (time - now) * 24 / 100;
    struct counter_alarm_cfg alarm = {
        .callback = ats_time_counter_alarm_cb,
        .flags = 0,
        .ticks = time};

    counter_cancel_channel_alarm(ats_time_gpt_dev, 0);
    ats_time_alarm_time.valid = true;
    ats_time_alarm_time.tick = time;
    return counter_set_channel_alarm(ats_time_gpt_dev, 0, &alarm);
}

int ats_time_cancel_alarm()
{
    counter_cancel_channel_alarm(ats_time_gpt_dev, 0);
    ats_time_alarm_time.valid = true;
}

#elif !DT_NODE_EXISTS(ATS_COUNTER)

static void ats_time_timer_cb(struct k_timer *timer)
{
    ats_time_alarm_time.valid = false;
    if (ats_time_callback != NULL)
    {
        ats_time_callback((uint64_t)k_uptime_ticks());
    }
}

K_TIMER_DEFINE(ats_time_timer, ats_time_timer_cb, NULL);

int ats_time_init(ats_time_alarm_callback_t callback)
{
    LOG_WRN("Using zephyr uptime as time source (NOT GRANULAR ENOUGH!) with frequency %" PRId32 " Hz as", TICKS_PER_SECOND);

    if (ats_time_callback != NULL)
    {
        ats_time_callback = callback;
    }
    return 0;
}

void ats_time_get(uint64_t *now)
{
    *now = (uint64_t)k_uptime_ticks();
}

int ats_time_set_alarm(uint64_t time)
{
    time = TICK_NORMALIZE(time);
    ats_time_alarm_time.valid = true;
    ats_time_alarm_time.tick = time;

    k_timer_start(&ats_time_timer, K_TICKS(time - (uint64_t)k_uptime_ticks()), K_FOREVER);
    return 0;
}

int ats_time_cancel_alarm()
{
    k_timer_stop(&ats_time_timer);
    ats_time_alarm_time.valid = true;
}

#endif

void ats_time_get_alarm(ats_time_t *now)
{
    *(now) = ats_time_alarm_time;
}
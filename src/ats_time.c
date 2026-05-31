#include "ats_time.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ats_time, CONFIG_ATS_LOG_LEVEL);

const static struct device *ats_time_dev = DEVICE_DT_GET(ATS_COUNTER);
static ats_time_t ats_time_alarm_time = {.valid = false};
static ats_time_alarm_callback_t ats_time_callback = NULL;

#ifdef DT_NODE_HAS_COMPAT(ATS_COUNTER, nxp_imx_gpt)

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

    ats_time_callback = callback;

    struct counter_top_cfg topCfg = {
        .ticks = counter_get_top_value(ats_time_dev),
        .callback = ats_time_counter_top_cb};
    counter_set_top_value(ats_time_dev, &topCfg);
    counter_start(ats_time_dev);
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
}

#elif DT_NODE_HAS_COMPAT(ATS_COUNTER, nxp_netc_ptp_clock)



#endif

void ats_time_get_alarm(ats_time_t *now)
{
    *(now) = ats_time_alarm_time;
}
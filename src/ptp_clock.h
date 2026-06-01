#ifndef RT1180_ATS_ATS_H__
#define RT1180_ATS_PTP_CLOCK_H__

#include <zephyr/kernel.h>

typedef void (*netc_ptp_clock_alarm_cb_t)(const struct device *dev);

struct netc_ptp_clock_alarm_config
{
    uint8_t alarmIndex;
    netc_ptp_clock_alarm_cb_t callback;
};

void ptp_clock_get_ns(const struct device *dev, uint64_t *nanosecond);
void ptp_clock_config_alarm(const struct device *dev, struct netc_ptp_clock_alarm_config *config);
void ptp_clock_start_alarm(const struct device *dev, uint8_t alarmIndex, uint64_t nanosecond);
void ptp_clock_stop_alarm(const struct device *dev, uint8_t alarmIndex);

#endif
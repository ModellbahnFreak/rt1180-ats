#include "ptp_clock.h"
#include <fsl_netc_timer.h>

#define DT_DRV_COMPAT nxp_netc_ptp_clock

struct ptp_clock_nxp_netc_data
{
	NETC_ENETC_Type *base;
	netc_timer_handle_t handle;
	struct k_mutex ptp_mutex;
	netc_timer_config_t ptp_config;
};

static netc_ptp_clock_alarm_cb_t netc_ptp_clock_curr_cb = NULL;

#define PTP_CLOCK_NXP_NETC_INIT_EXTENSION(n) \
	{                                        \
		.dev = DEVICE_DT_INST_GET(n),        \
		.irqNum = DT_INST_IRQN(n),           \
		.priority = 0,                       \
	},

static struct irqLookup
{
	struct device *dev;
	unsigned int irqNum;
	unsigned int priority;
};
struct irqLookup irqTable[] = {
	/*DT_INST_FOREACH_STATUS_OKAY(PTP_CLOCK_NXP_NETC_INIT_EXTENSION)*/};

void netc_ptp_clock_isr(const void *param)
{
	if (netc_ptp_clock_curr_cb != NULL)
	{
		struct device *dev = param;
		netc_ptp_clock_curr_cb(dev);
	}
}

void ptp_clock_get_ns(const struct device *dev, uint64_t *nanosecond)
{
	struct ptp_clock_nxp_netc_data *data = dev->data;
	NETC_TimerGetCurrentTime(&data->handle, nanosecond);
}

netc_timer_alarm_index_t ptp_clock_get_alarm_index(uint8_t index)
{
	switch (index)
	{
	case 1:
		return kNETC_TimerAlarm1;
		break;
	case 2:
		return kNETC_TimerAlarm2;
		break;
	default:
		__ASSERT(false, "netc ptp timer only supoports alarm index 1 and 2, was %d", index);
		break;
	}
	return (netc_timer_alarm_index_t)-1;
}

void ptp_clock_config_alarm(const struct device *dev, struct netc_ptp_clock_alarm_config *config)
{
	unsigned int irqNum;
	unsigned int priority = -1;
	for (size_t i = 0; i < sizeof(irqTable); i++)
	{
		if (dev == irqTable[i].dev)
		{
			irqNum = irqTable[i].irqNum;
			priority = irqTable[i].priority;
			break;
		}
	}
	__ASSERT(priority != (unsigned int)-1, "Could not find irq number for given ptp clock");
	irq_connect_dynamic(irqNum,
						priority,
						netc_ptp_clock_isr, dev, 0);
	irq_enable(irqNum);

	struct ptp_clock_nxp_netc_data *data = dev->data;
	__ASSERT(config != NULL, "No configuration for ptp timer alarm provided");
	netc_timer_alarm_index_t alarmIndex = ptp_clock_get_alarm_index(config->alarmIndex);
	netc_timer_alarm_t alarm = {
		.enableInterrupt = true,
	};
	if (config->callback != NULL)
	{
		netc_ptp_clock_curr_cb = config->callback;
	}

	NETC_TimerConfigureAlarm(&data->handle, alarmIndex, &alarm);
}

void ptp_clock_start_alarm(const struct device *dev, uint8_t alarmIndex, uint64_t nanosecond)
{
	struct ptp_clock_nxp_netc_data *data = dev->data;
	netc_timer_alarm_index_t alarmIndexEnum = ptp_clock_get_alarm_index(alarmIndex);

	NETC_TimerStartAlarm(&data->handle, alarmIndexEnum, nanosecond);
}

void ptp_clock_stop_alarm(const struct device *dev, uint8_t alarmIndex)
{
	struct ptp_clock_nxp_netc_data *data = dev->data;
	netc_timer_alarm_index_t alarmIndexEnum = ptp_clock_get_alarm_index(alarmIndex);

	NETC_TimerStopAlarm(&data->handle, alarmIndexEnum);
}
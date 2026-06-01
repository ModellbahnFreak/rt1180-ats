#ifndef RT1180_ATS_ATS_TIME_H__
#define RT1180_ATS_ATS_TIME_H__

#include <zephyr/kernel.h>

#define ATS_COUNTER DT_ALIAS(time)
#define TIME_NS 1000000000LL

#if DT_NODE_HAS_COMPAT(ATS_COUNTER, nxp_imx_gpt)
#define TIME_REQUIRES_MODULO
#define TICKS_PER_SECOND (uint32_t)(DT_PROP(ATS_COUNTER, gptfreq))

#elif DT_NODE_HAS_COMPAT(ATS_COUNTER, nxp_netc_ptp_clock)
#define TICKS_PER_SECOND TIME_NS

#elif !DT_NODE_EXISTS(ATS_COUNTER)
#warning "Using zephy integrated timer. This is not recommended due to accuracy. Specify a hardware timer"
#define TICKS_PER_SECOND CONFIG_SYS_CLOCK_TICKS_PER_SEC

#else
#error "Unknown counter/timer type for ATS. Requiring NXPs gpt or ptp timer"

#endif

#define CONFIG_ATS_TIMER_EPSILON_TICKS ((uint64_t)CONFIG_ATS_TIMER_EPSILON_NS * (uint64_t)TICKS_PER_SECOND / TIME_NS)

#ifdef TIME_REQUIRES_MODULO
/**
 * The time overflow/modulo in ticks (internal time unit)
 * Equal to casting to uint32 (as thats te overflow of the timer)
 * NOTE: Customization not possible because there would be a mismatch between counter overflow and time overflow
 */
#define TICK_MODULO (1LL << 32)
#define TICK_MAX_INTERVAL (TICK_MODULO / 2)

/**
 * The number of ns it takes the 32bit counter to overflow.
 * (Absolut) Maximum residence duration will be half of this interval as durations longer than that can't be reliably compared.
 *
 * CAUTION: This value is only to be used for asserts (not for actual time calculation) as it might not devide cleanly!
 */
#define TIME_MODULO (TICK_MODULO * TIME_NS / TICKS_PER_SECOND)

#define TICK_NORMALIZE(a) ((a) % TICK_MODULO)
#define TICK_ILLEGAL_INTERVAL(a, b) ((b) - (a) >= TICK_MAX_INTERVAL ? true : false)
#define TICK_LE(a, b) ((((b) - (a) + TICK_MODULO) % TICK_MODULO < TICK_MAX_INTERVAL) ? true : false)
#define TICK_GT(a, b) ((((b) - (a) + TICK_MODULO) % TICK_MODULO > TICK_MAX_INTERVAL) ? true : false)
#define TICK_MAX(a, b) ((((b) - (a) + TICK_MODULO) % TICK_MODULO < TICK_MAX_INTERVAL) ? (b) : (a))
#define TICK_MIN(a, b) ((((b) - (a) + TICK_MODULO) % TICK_MODULO < TICK_MAX_INTERVAL) ? (a) : (b))
#define TICK_REL_TIME(a, b) (((b) - (a) + TICK_MODULO) % TICK_MODULO)

#else // TIME_REQUIRES_MODULO

#define TICK_MAX_INTERVAL (1 << 63)

#define TICK_NORMALIZE(a) ((uint64_t)(a))
#define TICK_ILLEGAL_INTERVAL(a, b) false
#define TICK_LE(a, b) (((b) - (a)) < TICK_MAX_INTERVAL ? true : false)
#define TICK_GT(a, b) (((b) - (a)) > TICK_MAX_INTERVAL ? true : false)
#define TICK_MAX(a, b) ((((b) - (a)) < TICK_MAX_INTERVAL) ? (b) : (a))
#define TICK_MIN(a, b) ((((b) - (a)) < TICK_MAX_INTERVAL) ? (a) : (b))
#define TICK_REL_TIME(a, b) ((b) - (a))

#endif // TIME_REQUIRES_MODULO

struct ats_time_tick
{
    uint64_t tick;
    bool valid;
};

typedef void (*ats_time_alarm_callback_t)(uint64_t now);
typedef struct ats_time_tick ats_time_t;

int ats_time_init(ats_time_alarm_callback_t callback);
void ats_time_get(uint64_t *now);
void ats_time_get_alarm(ats_time_t *now);
int ats_time_set_alarm(uint64_t time);
int ats_time_cancel_alarm();

#endif
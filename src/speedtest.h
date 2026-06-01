#ifndef RT1180_ATS_SPEEDTEST_H__
#define RT1180_ATS_SPEEDTEST_H__

#include <zephyr/net/net_if.h>

#define CONFIG_ATS_SPEEDTEST_MAX_INSTANCES 1
#define CONFIG_ATS_SPEEDTEST_BUF_SIZE 10240

#define SPEEDTEST_BITMASK_SEND_SUCCESS (0b1LL << 63)
#define SPEEDTEST_BITMASK_SEND_FAIL (0b0LL << 63)
extern uint64_t speedtest_start_timestamp;
extern uint64_t speedtest_send_timestamps[];

k_tid_t speedtest_start(struct net_if *iface, uint8_t prio, bool useZephyrStack);

#endif
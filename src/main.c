/*
 * Copyright (c) 2019 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(net_pkt_sock_sample, LOG_LEVEL_DBG);

#include <zephyr/kernel.h>
#include <errno.h>
#include <stdio.h>

#include <zephyr/misc/lorem_ipsum.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dsa_core.h>

#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>

#include "ats.h"
#include "ats_iface.h"
#ifdef CONFIG_ATS_SOFTWARE_DRIVER
#include "sender.h"
#endif

#ifdef CONFIG_NETC_SPEEDTEST_ENABLED
#include "speedtest.h"
#endif

#if defined(CONFIG_NET_TC_THREAD_COOPERATIVE)
#define THREAD_PRIORITY K_PRIO_PREEMPT(0) // K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
#else
#define THREAD_PRIORITY K_PRIO_PREEMPT(8)
#endif

#define LAN_NODE DT_ALIAS(ats)

static struct k_sem quit_lock;

static bool finish;
static K_SEM_DEFINE(iface_up, 0, 1);
static const char *iface_dev_name = DT_NODE_FULL_NAME(LAN_NODE);
const struct device *if_dev_a; // = DEVICE_DT_GET(LAN_NODE);
struct net_if *iface;

static void find_actual_iface()
{
	if_dev_a = device_get_binding(iface_dev_name);

	iface = net_if_lookup_by_dev(if_dev_a);

	if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET))
	{
		LOG_ERR("Interface not an ethernet interface!");
		return;
	}
}

static void iface_up_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *changed_iface) //(uint64_t mgmt_event, struct net_if *changed_iface, void *info, size_t info_length, void *user_data)
{
	if (mgmt_event == NET_EVENT_IF_UP && changed_iface->if_dev->dev == iface->if_dev->dev)
	{
		k_sem_give(&iface_up);
	}
}

// NET_MGMT_REGISTER_EVENT_HANDLER(iface_up_cb, NET_EVENT_IF_UP, iface_up_handler, NULL);
static void wait_for_interface(void)
{
	if (net_if_is_up(iface))
	{
		return;
	}

	struct net_mgmt_event_callback iface_up_cb;
	net_mgmt_init_event_callback(&iface_up_cb, iface_up_handler,
								 NET_EVENT_IF_UP);
	net_mgmt_add_event_callback(&iface_up_cb);

	/* Wait for the interface to come up. */
	k_sem_take(&iface_up, K_FOREVER);

	net_mgmt_del_event_callback(&iface_up_cb);
}

int main(void)
{
	LOG_WRN("Hello world!");

	find_actual_iface();
	if (!ats_iface_is_ats(iface))
	{
		ats_init(iface);
	}

	k_sem_init(&quit_lock, 0, K_SEM_MAX_LIMIT);

	wait_for_interface();

	LOG_INF("Packet socket sample is running");

	// 2g:   250000000
	// 1g:   125000000
	// 999m: 124875000 = 100,7658%
	// 995m: 124375000 = 100,362%
	// 990m: 123750000 = 99,858%
	// 980m: 122500000 = 98,849%
	// 970m: 121250000 = 97,841%
	// 960m: 120000000 = 96,832%
	// 950m: 118750000 = 95,233%
	// 900m: 112500000 = 90,78%
	// 800m: 100000000 = 80,693%
	// 500m:  62500000
	// 400m:  50000000
	// 200m:  25000000
	// 133.3m:16666667
	// 100m:  12500000
	// 1m:      125000
	sender_start(iface, 1500, 0, 100000000, true, THREAD_PRIORITY);
	//sender_start(iface, 1500, 0, 50000000, true, THREAD_PRIORITY);
	//sender_start(iface, 1500, 0, 16666667, true, THREAD_PRIORITY);
	//sender_start(iface, 1500, 0, 16666667, true, THREAD_PRIORITY);
	//sender_start(iface, 1500, 0, 16666667, true, THREAD_PRIORITY);
	//sender_start(iface, 1500, 0, 16666667, true, THREAD_PRIORITY);

	// speedtest_start(ats_iface_is_ats(iface) ? ats_iface_get_data(iface)->iface : iface, CONFIG_NUM_PREEMPT_PRIORITIES - 1, true);
	/*speedtest_start(ats_iface_is_ats(iface) ? ats_iface_get_data(iface)->iface : iface, CONFIG_NUM_PREEMPT_PRIORITIES - 1, true);
	speedtest_start(ats_iface_is_ats(iface) ? ats_iface_get_data(iface)->iface : iface, CONFIG_NUM_PREEMPT_PRIORITIES - 1, true);
	speedtest_start(ats_iface_is_ats(iface) ? ats_iface_get_data(iface)->iface : iface, CONFIG_NUM_PREEMPT_PRIORITIES - 1, true);*/

	k_sem_take(&quit_lock, K_FOREVER);

	LOG_INF("Stopping...");

	return 0;
}

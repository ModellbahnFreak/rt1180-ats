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

#ifdef CONFIG_ATS_SOFTWARE_DRIVER
#include "ats.h"
#include "ats_iface.h"
#include "sender.h"
#endif

#ifdef CONFIG_NETC_SPEEDTEST_ENABLED
#include "speedtest.h"
#endif

#define STACK_SIZE 1024
#if defined(CONFIG_NET_TC_THREAD_COOPERATIVE)
#define THREAD_PRIORITY K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
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

#ifdef CONFIG_ATS_SOFTWARE_DRIVER
K_HEAP_DEFINE(sender_thread_heap, sizeof(struct sender_params) * 5);
static K_THREAD_STACK_ARRAY_DEFINE(sender_thread_stack, 5, STACK_SIZE);
static struct k_thread sender_thread_data[5];
int sender_threads_count = 0;
static struct sender_params sender_thread_params[5];

void start_sender(struct net_if *iface, uint16_t maxPacket, uint32_t minTUs, uint32_t maxTUs, bool regular)
{
	int i = sender_threads_count++;
	struct sender_params *params = &sender_thread_params[i];
	params->iface = iface;
	params->maxPacket = maxPacket;
	params->maxIntervalUs = maxTUs;
	params->minIntervalUs = minTUs;
	params->regular = regular;

	k_tid_t sender_thread_id = k_thread_create(
		&sender_thread_data[i], &sender_thread_stack[i][0], STACK_SIZE,
		sender, params, NULL, NULL,
		THREAD_PRIORITY, 0, K_NO_WAIT);
}
#else
#define start_sender(struct net_if *iface, uint16_t maxPacket, uint32_t minTUs, uint32_t maxTUs, bool regular) \
	{                                                                                                          \
	}
#endif

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

	start_sender(iface, 1500, 100000, 200000, true);
	//   start_sender(iface, 100, 1000000, 2000000, false);

	// speedtest_start(iface, CONFIG_NUM_PREEMPT_PRIORITIES - 1, false);

	k_sem_take(&quit_lock, K_FOREVER);

	LOG_INF("Stopping...");

	return 0;
}

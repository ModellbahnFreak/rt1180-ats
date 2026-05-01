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
#include "sender.h"

#define CONFIG_NET_SAMPLE_SEND_WAIT_TIME 1000
#define CONFIG_NET_SAMPLE_ENABLE_PACKET_DGRAM
#define CONFIG_NET_SAMPLE_DESTINATION_ADDR "01:00:5e:00:00:00"

#define STACK_SIZE 1024
#if defined(CONFIG_NET_TC_THREAD_COOPERATIVE)
#define THREAD_PRIORITY K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1)
#else
#define THREAD_PRIORITY K_PRIO_PREEMPT(8)
#endif
#define RECV_BUFFER_SIZE 1280
#define WAIT_TIME CONFIG_NET_SAMPLE_SEND_WAIT_TIME

#define FLOOD (CONFIG_NET_SAMPLE_SEND_WAIT_TIME ? 0 : 1)

#define LAN_NODE DT_ALIAS(ethpseudo)
int port_idx = 3;

static struct k_sem quit_lock;

struct packet_data
{
	int send_sock;
};

static struct packet_data sock_packet;
static bool finish;
static K_SEM_DEFINE(iface_up, 0, 1);
const struct device *if_dev_a = DEVICE_DT_GET(LAN_NODE);
struct net_if *iface;

static void send_packet(void);

K_THREAD_DEFINE(send_help_thread_id, STACK_SIZE,
				send_packet, NULL, NULL, NULL,
				THREAD_PRIORITY, 0, -1);

// K_THREAD_STACK_DEFINE(sender_thread_stack, STACK_SIZE);

const char lorem_ipsum_a[] = "Interface A" LOREM_IPSUM;

static void quit(void)
{
	k_sem_give(&quit_lock);
}

static int start_socket(int *sock, int if_idx)
{
	struct sockaddr_ll dst = {0};
	int ret;

	*sock = socket(AF_PACKET,
				   IS_ENABLED(CONFIG_NET_SAMPLE_ENABLE_PACKET_DGRAM) ? SOCK_DGRAM : SOCK_RAW,
				   htons(ETH_P_ALL));
	if (*sock < 0)
	{
		LOG_ERR("Failed to create %s socket : %d",
				IS_ENABLED(CONFIG_NET_SAMPLE_ENABLE_PACKET_DGRAM) ? "DGRAM" : "RAW",
				errno);
		return -errno;
	}

	dst.sll_ifindex = if_idx;
	dst.sll_family = AF_PACKET;

	ret = bind(*sock, (const struct sockaddr *)&dst,
			   sizeof(struct sockaddr_ll));
	if (ret < 0)
	{
		LOG_ERR("Failed to bind packet socket : %d", errno);
		return -errno;
	}

	return 0;
}

static void find_actual_iface()
{
	iface = net_if_lookup_by_dev(if_dev_a);

	if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET))
	{
		LOG_ERR("Interface not an ethernet interface!");
		return;
	}

#ifdef CONFIG_NET_DSA
	if ((net_eth_get_hw_capabilities(iface) & ETHERNET_DSA_CONDUIT_PORT))
	{
		struct net_if *user = dsa_user_get_iface(iface, port_idx);
		if (user == NULL)
		{
			LOG_ERR("Port %d of DSA interface not found", port_idx);
			return;
		}
		LOG_INF("User interface %d found.", net_if_get_by_iface(user));

		iface = user;
		return;
	}
#endif
}

static void send_packet(void)
{
	int ret;
	int if_a = net_if_get_by_iface(iface);

	ret = start_socket(&sock_packet.send_sock, if_a);
	if (ret < 0)
	{
		quit();
		return;
	}

	while (ret == 0)
	{
		/* If we have received any data, flush it here in order to
		 * not to leak memory in IP stack.
		 */
		do
		{

			static char recv_buffer[RECV_BUFFER_SIZE];

			ret = recv(sock_packet.send_sock, recv_buffer,
					   sizeof(recv_buffer), MSG_DONTWAIT);
		} while (ret > 0);

		if (ret != 0)
		{
			LOG_WRN("Error receiving %d", ret);
			ret = 0;
		}

		LOG_DBG("Done receiving. Sleeping");
		k_sleep(K_MSEC(1000));
	}
	LOG_DBG("Helper thread exiting");
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
	// struct net_if *iface = net_if_get_default();
	// struct net_if *iface = net_if_lookup_by_dev(if_dev_a);

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

int main(void)
{
	LOG_WRN("Hello world!");

	// struct net_if *iface = net_if_lookup_by_dev(if_dev_a);
	find_actual_iface();
	// todo? auto init for iface
	ats_init(iface);

	k_sem_init(&quit_lock, 0, K_SEM_MAX_LIMIT);

	wait_for_interface();

	LOG_INF("Packet socket sample is running");

	/*int i = sender_threads_count++;
	k_tid_t speedtest_thread_id = k_thread_create(
		&sender_thread_data[i], &sender_thread_stack[i][0], STACK_SIZE,
		sender_speedtest, iface, NULL, NULL,
		THREAD_PRIORITY, 0, K_NO_WAIT);*/

	 start_sender(iface, 1486, 1000000, 2000000, true);
	//  start_sender(iface, 100, 1000000, 2000000, false);

	// k_thread_start(send_help_thread_id);

	k_sem_take(&quit_lock, K_FOREVER);

	LOG_INF("Stopping...");

	finish = true;

	k_thread_join(send_help_thread_id, K_FOREVER);

	if (sock_packet.send_sock >= 0)
	{
		(void)close(sock_packet.send_sock);
	}
	return 0;
}

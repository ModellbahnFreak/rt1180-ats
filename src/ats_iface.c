#define DT_DRV_COMPAT nxp_imx_ats

#include "ats.h"

#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#ifdef CONFIG_NET_DSA
#include <zephyr/net/dsa_core.h>
#endif

LOG_MODULE_REGISTER(ats_iface, CONFIG_ATS_LOG_LEVEL);

struct ats_iface_config
{
    const char *parentDevName;
    const char *grandparentDevName;
    const uint8_t parentDsaPort;
    const uint8_t priority;
};

struct ats_iface_data
{
    struct ats *ats_data;
    struct net_if *atsIface;
};

int ats_iface_tx(const struct device *dev, struct net_pkt *pkt);

bool ats_iface_is_ats(struct net_if *ats_if)
{
    const struct device *dev = net_if_get_device(ats_if);
    const struct ethernet_api *api = dev->api;
    return api != NULL && api->send == ats_iface_tx;
}

struct ats *ats_iface_get_data(struct net_if *iface)
{
    const struct device *dev = net_if_get_device(iface);
    const struct ethernet_api *api = dev->api;
    if (api == NULL || api->send != ats_iface_tx)
    {
        return NULL;
    }
    struct ats_iface_data *data = dev->data;
    if (data == NULL || data->ats_data == NULL || data->ats_data->iface == NULL)
    {
        return NULL;
    }
    return data->ats_data;
}

uint8_t ats_iface_register_stream_to_group(struct net_if *ats_if, int groupId, uint32_t informationRate, uint32_t burstSize)
{
    struct ats *ats_data = ats_iface_get_data(ats_if);
#ifdef CONFIG_ATS_ASSERT_ON_SEND
    __ASSERT(ats_data != NULL, "The interface given to ats_iface_register_stream_to_group was not an ats_iface");
#endif
    return ats_register_scheduler(ats_data, groupId, informationRate, burstSize);
}

uint8_t ats_iface_register_stream(struct net_if *ats_if, uint32_t informationRate, uint32_t burstSize)
{
    return ats_iface_register_stream_to_group(ats_if, -1, informationRate, burstSize);
}

uint8_t ats_iface_stream_get_group(struct net_if *ats_if, uint8_t streamId)
{
    struct ats *ats_data = ats_iface_get_data(ats_if);
#ifdef CONFIG_ATS_ASSERT_ON_SEND
    __ASSERT(ats_data != NULL, "The interface given to ats_iface_stream_get_group was not an ats_iface");
#endif
    return ats_get_group_id(ats_data, streamId);
}

static struct net_mgmt_event_callback parent_iface_changed_cb;
static void ats_parent_iface_changed_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event, struct net_if *changed_iface)
{
    const struct device *changed_dev = net_if_get_device(changed_iface);
    const struct ethernet_api *changed_api = changed_dev->api;
    if (changed_api != NULL && changed_api->send == ats_iface_tx)
    {
        return;
    }

    STRUCT_SECTION_FOREACH(net_if, iface)
    {
        const struct device *dev = net_if_get_device(iface);
        const struct ethernet_api *api = dev->api;
        if (api == NULL || api->send != ats_iface_tx)
        {
            continue;
        }

        struct ats_iface_data *data = dev->data;
        if (data == NULL || data->ats_data == NULL || data->ats_data->iface != changed_iface)
        {
            continue;
        }

        if (mgmt_event == NET_EVENT_IF_UP)
        {
            LOG_DBG("Parent interface %s of %s came up. Updating ats iface state", net_if_get_device(changed_iface)->name, dev->name);
            net_eth_carrier_on(iface);
        }
        else if (mgmt_event == NET_EVENT_IF_DOWN)
        {
            LOG_DBG("Parent interface %s of %s went down. Updating ats iface state", net_if_get_device(changed_iface)->name, dev->name);
            net_eth_carrier_off(iface);
        }
    }
}

static void ats_iface_interface_init(struct net_if *iface)
{
    const struct device *dev = net_if_get_device(iface);
    struct ats_iface_data *data = dev->data;
    data->atsIface = iface;

    struct net_linkaddr *mac = net_if_get_link_addr(data->ats_data->iface);
    net_if_set_link_addr(iface, mac->addr, mac->len, mac->type);
    LOG_INF("ATS %s MAC: %02x:%02x:%02x:%02x:%02x:%02x", dev->name,
            mac->addr[0], mac->addr[1], mac->addr[2], mac->addr[3],
            mac->addr[4], mac->addr[5]);

    ethernet_init(iface);

    // can assume parent device ready, as it is requirement for it to be returned by device_get_binding
    if (net_if_is_up(data->ats_data->iface))
    {
        net_eth_carrier_on(iface);
    }
    else
    {
        net_if_carrier_off(iface);
    }
}

static int ats_iface_init(const struct device *dev)
{
    const struct ats_iface_config *conf = dev->config;
    LOG_DBG("ATS config for %s: Priority: %d, parent: %s, grandparent: %s, dsaPort: %d", dev->name, conf->priority, conf->parentDevName, conf->grandparentDevName, conf->parentDsaPort);
    const struct device *parentDev = device_get_binding(conf->parentDevName);
    if (parentDev == NULL)
    {
        parentDev = device_get_binding(conf->grandparentDevName);
        if (parentDev == NULL)
        {
            LOG_ERR("Could not find referenced parent interface %s (or it wasn't)", conf->parentDevName);
            return 1;
        }
    }
    struct net_if *parentIface = net_if_lookup_by_dev(parentDev);
    if (parentIface == NULL)
    {
        LOG_ERR("No interface found for referenced parent %s. Make sure it is a ethernet/dsa device", conf->parentDevName);
        return 2;
    }

    if (net_if_l2(parentIface) != &NET_L2_GET_NAME(ETHERNET))
    {
        char ifName[100];
        net_if_get_name(parentIface, ifName, 100);
        LOG_ERR("Parent interface %s of ats %s not an ethernet interface!", ifName, dev->name);
        return 3;
    }

#ifdef CONFIG_NET_DSA
    if ((net_eth_get_hw_capabilities(parentIface) & ETHERNET_DSA_CONDUIT_PORT))
    {
        struct net_if *user = dsa_user_get_iface(parentIface, conf->parentDsaPort);
        if (user == NULL)
        {
            char ifName[100];
            net_if_get_name(parentIface, ifName, 100);
            LOG_ERR("Port %d of DSA interface %s not found", conf->parentDsaPort, ifName);
            return 4;
        }
        parentIface = user;
    }
#endif

    char ifName[100];
    net_if_get_name(parentIface, ifName, 100);
    LOG_INF("Ats %s using iface %s as parent ethernet interface", dev->name, ifName);

    struct ats_iface_data *data = dev->data;
    data->ats_data = ats_init(parentIface);

    if (parent_iface_changed_cb.handler == NULL)
    {
        net_mgmt_init_event_callback(&parent_iface_changed_cb, ats_parent_iface_changed_handler,
                                     NET_EVENT_IF_UP | NET_EVENT_IF_DOWN);
        net_mgmt_add_event_callback(&parent_iface_changed_cb);
    }

    return 0;
}

int ats_iface_tx(const struct device *dev, struct net_pkt *pkt)
{
    struct ats_iface_data *data = dev->data;
    if (net_pkt_ats_is_scheduled(pkt))
    {
        return ats_send(data->ats_data, pkt->ats_stream, pkt);
    }
    else
    {
        const struct ethernet_api *api = data->ats_data->ifaceDevice->api;
        net_pkt_set_iface(pkt, data->ats_data->iface);
        return api->send(data->ats_data->ifaceDevice, pkt);
    }
}

enum ethernet_hw_caps ats_iface_get_capabilities(const struct device *dev)
{
    struct ats_iface_data *data = dev->data;
    return net_eth_get_hw_capabilities(data->ats_data->iface);
}

static const struct ethernet_api ats_eth_api = {.iface_api.init = ats_iface_interface_init,
                                                .send = ats_iface_tx,
                                                .get_capabilities = ats_iface_get_capabilities};

#define ATS_IFACE_INSTANCE_DEFINE(n)                                               \
    static const struct ats_iface_config ats_iface##n##_config = {                 \
        .parentDevName = DT_NODE_FULL_NAME(DT_PHANDLE(n, parent)),                 \
        .grandparentDevName = DT_NODE_FULL_NAME(DT_PARENT(DT_PHANDLE(n, parent))), \
        .parentDsaPort = DT_REG_ADDR(DT_PHANDLE(n, parent)),                       \
        .priority = DT_PROP(n, priority)};                                         \
    static struct ats_iface_data ats_iface##n##_data = {};                         \
    ETH_NET_DEVICE_DT_DEFINE(n, ats_iface_init, NULL, &ats_iface##n##_data,        \
                             &ats_iface##n##_config, CONFIG_ATS_IFACE_INIT_PRIO,   \
                             &ats_eth_api, NET_ETH_MTU);

#if CONFIG_ATS_IFACE_INIT_PRIO <= CONFIG_ETH_INIT_PRIORITY || CONFIG_ATS_IFACE_INIT_PRIO >= CONFIG_NET_INIT_PRIO
#error "CONFIG_ATS_IFACE_INIT_PRIO must be bigger than CONFIG_ETH_INIT_PRIORITY and smaller than CONFIG_NET_INIT_PRIO!"
#endif

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1
DT_INST_FOREACH_CHILD_STATUS_OKAY(0, ATS_IFACE_INSTANCE_DEFINE)
#elif DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) > 1
#error "ats_iface.c: Only one instance of the 'ats' parent element with compatible 'nxp,imx-ats' is allowed!"
#endif

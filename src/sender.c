#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

int ats_send(char *buf, int len, uint8_t id)
{
}

void sender()
{
    uint8_t myId = 0;
    uint16_t maxPacket = 1400;
    bool regular = true;
    uint32_t intervalUs = 1000000;
    char[maxPacket] pkg;
    if (regular)
    {
        while (true)
        {
            LOG_INF("App %d trying to send %d bytes, then waiting %d us", myId, maxPacket, intervalUs);
            ats_send(pkg, maxPacket, myId);
            k_sleep(K_USEC(intervalUs))
        }
    }
    else
    {
        while (true)
        {
            uint16_t pkgSize = MAX(sys_rand16_get() & 0x7ff, maxPacket);
            uint32_t sleepTime = MAX(sys_rand32_get() & 0x3fffff, intervalUs);
            LOG_INF("App %d trying to send %d bytes, then waiting %d us", myId, pkgSize, sleepTime);
            ats_send(pkg, pkgSize, myId);
            k_sleep(K_USEC(sleepTime));
        }
    }
}
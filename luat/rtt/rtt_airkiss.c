#include <rtthread.h>
#include <rtdevice.h>

#ifdef RT_WLAN_MANAGE_ENABLE
#include <sys/socket.h>

#include "airkiss.h"

#include <stdlib.h>

typedef struct rtt_airkiss_ctx
{
    airkiss_context_t ac;
    airkiss_result_t result;
    uint32_t lock_chn;

} rtt_airkiss_ctx_t;

static rtt_airkiss_ctx_t* rak_ctx;

static airkiss_callback airkiss_cb;

static void prom_callback(struct rt_wlan_device *device, void *d, int s);

static void airkiss_send_notification(uint8_t random);

static void airkiss_send_notification(uint8_t random)
{
    int sock = -1;
    int udpbufsize = 2;
    struct sockaddr_in UDPBCAddr, UDPBCServerAddr;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        rt_kprintf("notify create socket error!\n");
        goto _exit;
    }

    UDPBCAddr.sin_family = AF_INET;
    UDPBCAddr.sin_port = htons(10000);
    UDPBCAddr.sin_addr.s_addr = htonl(0xffffffff);
    rt_memset(&(UDPBCAddr.sin_zero), 0, sizeof(UDPBCAddr.sin_zero));

    UDPBCServerAddr.sin_family = AF_INET;
    UDPBCServerAddr.sin_port = htons(10000);
    UDPBCServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    rt_memset(&(UDPBCServerAddr.sin_zero), 0, sizeof(UDPBCServerAddr.sin_zero));

    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &udpbufsize, sizeof(int)) != 0)
    {
        rt_kprintf("notify socket setsockopt error\n");
        goto _exit;
    }

    if (bind(sock, (struct sockaddr *)&UDPBCServerAddr, sizeof(UDPBCServerAddr)) != 0)
    {
        rt_kprintf("notify socket bind error\n");
        goto _exit;
    }

    for (int i = 0; i <= 50; i++)
    {
        int ret = sendto(sock, (char *)&random, 1, 0, (struct sockaddr *)&UDPBCAddr, sizeof(UDPBCAddr));
        rt_thread_mdelay(10);
    }

_exit:
    if (sock >= 0)
    {
        closesocket(sock);
    }
}

static void prom_callback(struct rt_wlan_device *device, void *d, int s) {
    int8_t ret;
    if (rak_ctx == RT_NULL)
        return;
    ret = airkiss_recv(&(rak_ctx->ac), d, s);
    if (ret == AIRKISS_STATUS_CHANNEL_LOCKED) {
        rak_ctx->lock_chn = 1;
    }
    else if (ret == AIRKISS_STATUS_COMPLETE) {
        rt_wlan_dev_exit_promisc(device);
        rt_enter_critical();
        int8_t err = airkiss_get_result(&(rak_ctx->ac), &(rak_ctx->result));
        rt_exit_critical();
        if (err == 0) {
            rt_kprintf("Airkiss DONE!! ssid=%s passwd=%s\n", rak_ctx->result.ssid, rak_ctx->result.pwd);
        }
        else {
            rt_kprintf("Airkiss airkiss_get_result err=%d\n", err);
        }
    }
}

void airkiss_set_callback(airkiss_callback cb) {
    airkiss_cb = cb;
}

static void do_airkiss_configwifi(void)
{
    struct rt_wlan_device *dev;
    int n;
    //int round = 120;
    //int re = -1;
    //int chns = 12;
    rtt_airkiss_ctx_t _ctx;
    airkiss_config_t acfg =
        {
            (airkiss_memset_fn)&rt_memset,
            (airkiss_memcpy_fn)&rt_memcpy,
            (airkiss_memcmp_fn)&rt_memcmp,
            (airkiss_printf_fn)&rt_kprintf
        };
    rt_memset(&_ctx, 0, sizeof(rtt_airkiss_ctx_t));
    rak_ctx = &_ctx;

    rt_kprintf("airkiss thread start...\n");

    airkiss_init(&(rak_ctx->ac), &acfg);
    rt_wlan_config_autoreconnect(0);
    if (rt_wlan_is_connected())
        rt_wlan_disconnect();
    dev = (struct rt_wlan_device *)rt_device_find(RT_WLAN_DEVICE_STA_NAME);
    rt_wlan_dev_set_promisc_callback(dev, prom_callback);

    rt_wlan_dev_enter_promisc(dev);

    for (size_t i = 0; i < 120; i++)
    {
        for (size_t j = 0; j < 12; j++)
        {
            rt_thread_mdelay(100);
            if (_ctx.result.ssid_length > 0) {
                i = 9999;
                break;
            }
            if (!_ctx.lock_chn) {
                rt_wlan_dev_set_channel(dev, j);
                airkiss_change_channel(&(_ctx.ac));
            }
        }
    }
    rt_wlan_dev_exit_promisc(dev);
    rt_kprintf("airkiss loop exit\n");

    if (_ctx.result.ssid_length > 0) {
        rt_err_t err = rt_wlan_connect(_ctx.result.ssid, _ctx.result.pwd);
        if (err == RT_EOK) {
            rt_kprintf("airkiss autoconnect success!!\n");
            rt_wlan_config_autoreconnect(1);
            rt_kprintf("airkiss wait wifi ready!!\n");
            for (size_t i = 0; i < 50; i++)
            {
                if (rt_wlan_is_ready()) {
                    break;
                }
                rt_thread_mdelay(100);
            }
            rt_kprintf("airkiss send notification!!\n");
            airkiss_send_notification(_ctx.result.random);
        }
        else {
            rt_kprintf("airkiss auto connect FAIL!!!\n");
        }
    }
    if (airkiss_cb != NULL) {
        rt_kprintf("calling airkiss_cb\n");
        airkiss_cb(_ctx.result.ssid_length ? 0 : 1, _ctx.result.ssid, _ctx.result.pwd);
    }
    rak_ctx = RT_NULL;
    rt_kprintf("airkiss thread exit\n");
}

static void airkiss_thread(void *p)
{
    do_airkiss_configwifi();
}

int airkiss_start(void)
{
    rt_thread_t tid;
    int ret = -1;

    tid = rt_thread_create("airkiss",
                           airkiss_thread,
                           0,
                           2048,
                           22,
                           20);

    if (tid)
    {
        ret = rt_thread_startup(tid);
    }

    return ret;
}

#endif

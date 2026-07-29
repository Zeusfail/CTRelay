/*
*   nwm_relay.c - a software nwm::UDS server grafted into Rosalina.
*
*   When the k11 patch redirects nwm::UDS to "nwmrelay" (opt-in), a game's local
*   wireless calls land here instead of the real NWM sysmodule. We answer them
*   ourselves and bridge the actual traffic to a VPS over UDP, so a real 3DS can
*   join an emulator-hosted session across the internet.
*
*   Reply formats mirror azahar's nwm_uds.cpp. Single session at a time.
*/
#include <3ds.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "csvc.h"
#include "nwm_relay.h"
#include "minisoc.h"
#include "MyThread.h"
#include "menu.h"        // CORE_SYSTEM
#include "vps_config.h"  // VPS_IP_* and VPS_PORT (copy vps_config.h.example)

#define SERVICEOP_REGISTER_RELAY_PID  ((ServiceOp)4)

// Rosalina's "forced Wi-Fi" flag (sysconfig menu). We set it so the pre-termination
// cleanup in main.c releases the ac session we opened.
extern bool isConnectionForced;

// NetworkStatus values (nwm_uds.h).
#define NWM_NOT_CONNECTED   3
#define NWM_HOST            6
#define NWM_CLIENT          9

static Handle g_connEvent = 0;                    // connection_status_event handed to the game
static volatile u8 g_status = NWM_NOT_CONNECTED;  // also read by the network thread
static bool g_pidRegistered = false;

volatile int g_netState   = 0;   // network-thread state (see nwmRelayNetMain)
volatile u32 g_netTxCount = 0;
volatile u32 g_netRxCount = 0;

// Static-buffer destinations for incoming commands (BeginHostingNetwork /
// SetApplicationData deliver their data via PopStaticBuffer). Without a valid
// destination the kernel faults while copying. One buffer per id so we can read
// back the NetworkInfo passed to BeginHostingNetwork (usually id 0).
#define SBUF_SZ 0x600   // >= the largest SendTo payload (0x5C6) and sizeof(NetworkInfo)
static u8 g_sbuf[6][SBUF_SZ] __attribute__((aligned(8)));
static u8 g_maxNodes = 16;

// TX ring: game SendTo (service thread) -> network thread -> VPS. Single producer.
#define TX_RING 32
#define TX_MAX  0x5C6
typedef struct {
    u16 len;
    u16 dest;
    u8  channel;
    u8  flags;
    u8  data[TX_MAX];
} TxFrame;
static TxFrame g_tx[TX_RING];
static volatile u32 g_txHead = 0;
static volatile u32 g_txTail = 0;

// RX ring: host data from the shim (network thread) -> game PullPacket (service thread).
#define RX_RING 32
#define RX_MAX  0x5C6
typedef struct {
    u16 len;
    u16 srcNode;
    u8  channel;
    u8  pad;
    u8  data[RX_MAX];
} RxFrame;
static RxFrame g_rx[RX_RING];
static volatile u32 g_rxHead = 0;
static volatile u32 g_rxTail = 0;
volatile u32 g_rxCount = 0;
static u8 g_pullBuf[RX_MAX] __attribute__((aligned(8)));

// Latest beacon relayed by the shim (NWMB). Written by the network thread, read by
// RecvBeacon; the length is published last so a torn read yields "no network".
#define BEACON_MAX 0x200
static u8  g_beacon[BEACON_MAX];
static u8  g_beaconMac[6] = {0};
static volatile u8  g_beaconChannel = 11;
static volatile u16 g_beaconLen = 0;
static volatile bool g_wantNet = false;   // RecvBeacon (a scan) wakes the network thread

// Client association state (the 3DS joining a host through the shim).
static volatile bool g_wantJoin   = false;
static volatile bool g_joined     = false;
static volatile u16  g_clientNode = 0;
static volatile bool g_sendReset  = false; // network thread should send NWMX to the shim
volatile bool g_netStop    = false;        // set by pre-termination: close soc and idle
volatile bool g_netStopped = false;

static void setupStaticBuffers(void)
{
    u32 *sb = getThreadStaticBuffers();
    for(u32 id = 0; id < 6; id++)
    {
        sb[id * 2]     = IPC_Desc_StaticBuffer(SBUF_SZ, id);
        sb[id * 2 + 1] = (u32)g_sbuf[id];
    }
}

static void replySimple(u32 *c, u16 cmd, Result result)
{
    c[0] = IPC_MakeHeader(cmd, 1, 0);
    c[1] = (u32)result;
}

static void replyWithEvent(u32 *c, u16 cmd)
{
    if(g_connEvent == 0)
        svcCreateEvent(&g_connEvent, RESET_ONESHOT);

    c[0] = IPC_MakeHeader(cmd, 1, 2);
    c[1] = 0;
    c[2] = IPC_Desc_SharedHandles(1);
    c[3] = (u32)g_connEvent;
}

// Reset the connection state at session start/end. Rosalina keeps running between
// games, so without this the stale state forces a reboot before the next join. Also
// tells the shim to drop its old association (NWMX).
static void relayResetSession(void)
{
    g_status     = NWM_NOT_CONNECTED;
    g_joined     = false;
    g_wantJoin   = false;
    g_clientNode = 0;
    g_rxTail     = g_rxHead;
    g_txTail     = g_txHead;
    g_beaconLen  = 0;
    g_sendReset  = true;
}

void NwmRelay__HandleCommands(void *ctx)
{
    (void)ctx;
    u32 *c = getThreadCommandBuffer();
    u16 cmd = (u16)(c[0] >> 16);
    u32 normal    = (c[0] >> 6) & 0x3F;
    u32 translate = c[0] & 0x3F;

    if(!g_pidRegistered)
    {
        svcControlService(SERVICEOP_REGISTER_RELAY_PID);   // exempt us from the redirect
        g_pidRegistered = true;
    }

    setupStaticBuffers();   // takes effect on the next receive

    switch(cmd)
    {
    case 0x001B: // InitializeWithVersion
    {
        // 12 normal words + one shared-memory handle we don't use.
        if(translate >= 2)
        {
            Handle sharedmem = (Handle)c[normal + 2];
            if(sharedmem)
                svcCloseHandle(sharedmem);
        }
        relayResetSession();
        replyWithEvent(c, 0x001B);
        break;
    }

    case 0x0012: // Bind
        replyWithEvent(c, 0x0012);
        break;

    case 0x000B: // GetConnectionStatus -> ConnectionStatus (12 words at c[2..13])
    {
        c[0] = IPC_MakeHeader(0x000B, 13, 0);
        c[1] = 0;
        for(int i = 0; i < 12; i++)
            c[2 + i] = 0;
        c[2] = g_status;
        if(g_status == NWM_HOST)
        {
            c[3]  = 1;                                    // status_change_reason
            c[4]  = 1 | (1 << 16);                        // node_id=1, changed_nodes=1
            c[5]  = 1;                                    // nodes[0]=1
            c[13] = 1 | (g_maxNodes << 8) | (1 << 16);    // total=1, max, bitmask=1
        }
        else if(g_status == NWM_CLIENT)
        {
            u16 me = g_clientNode ? g_clientNode : 2;     // host is node 1, we are `me`
            u32 bitmask = 1u | (1u << (me - 1));
            c[3]  = 1;
            c[4]  = me | (bitmask << 16);
            c[5]  = 1 | ((u32)me << 16);                  // nodes[0]=1, nodes[1]=me
            c[13] = 2 | (g_maxNodes << 8) | (bitmask << 16);
        }
        break;
    }

    case 0x000F: // RecvBeaconBroadcastData -> the relayed beacon, or "no network"
    {
        g_wantNet = true;   // a scan: bring up the network thread

        u32 bufIdx = 1 + normal + translate;   // mapped output buffer = last translate pair
        if(translate >= 2)
        {
            u32 desc = c[bufIdx - 2];
            u32 addr = c[bufIdx - 1];
            u32 outSize = desc >> 4;
            u16 blen = g_beaconLen;
            u8 *b = (u8 *)addr;

            if(addr && blen > 0 && (u32)(12 + 28 + blen) <= outSize)
            {
                // BeaconDataReplyHeader (12) + BeaconEntryHeader (0x1C) + beacon.
                *(u32 *)(b + 0) = outSize;
                *(u32 *)(b + 4) = 12 + 28 + blen;
                *(u32 *)(b + 8) = 1;                      // total_entries
                u8 *e = b + 12;
                u32 entryTotal = 28 + blen;
                *(u32 *)(e + 0)  = entryTotal;
                e[4] = 0;
                e[5] = g_beaconChannel;
                e[6] = 0; e[7] = 0;
                memcpy(e + 8, g_beaconMac, 6);
                memset(e + 14, 0, 6);
                *(u32 *)(e + 20) = entryTotal;
                *(u32 *)(e + 24) = 28;
                memcpy(b + 40, g_beacon, blen);
            }
            else if(addr)
            {
                *(u32 *)(b + 0) = outSize;
                *(u32 *)(b + 4) = 12;
                *(u32 *)(b + 8) = 0;                      // no network yet
            }
            c[0] = IPC_MakeHeader(0x000F, 1, 2);
            c[1] = 0;
            c[2] = desc;
            c[3] = addr;
        }
        else
        {
            replySimple(c, 0x000F, 0);
        }
        break;
    }

    case 0x001D: // BeginHostingNetwork
    {
        u8 m = g_sbuf[0][0x1D];   // NetworkInfo.max_nodes
        if(m >= 2 && m <= 32)
            g_maxNodes = m;
        g_status = NWM_HOST;
        if(g_connEvent)
            svcSignalEvent(g_connEvent);
        replySimple(c, 0x001D, 0);
        break;
    }

    case 0x001E: // ConnectToNetwork -> ask the shim to associate us with the host
    {
        g_rxTail = g_rxHead;   // don't let the previous session's data leak in
        g_txTail = g_txHead;
        g_wantNet  = true;
        g_wantJoin = true;
        g_joined   = false;
        // ConnectToNetwork blocks on real hardware: wait for the association (NWMC)
        // or time out (~8 s) before replying.
        for(int i = 0; i < 400 && !g_joined; i++)
            svcSleepThread(20 * 1000 * 1000LL);
        if(g_joined)
        {
            g_status = NWM_CLIENT;
            if(g_connEvent)
                svcSignalEvent(g_connEvent);
            replySimple(c, 0x001E, 0);
        }
        else
        {
            g_wantJoin = false;
            replySimple(c, 0x001E, 0xC8A07C01);   // connection failed
        }
        break;
    }

    case 0x0017: // SendTo (header 0x00170182: 6 normal + one static buffer)
    {
        if(g_status != NWM_HOST && g_status != NWM_CLIENT)
            g_status = NWM_HOST;   // a game that sends believes it is connected

        // c[2]=dest_node_id c[3]=data_channel c[5]=data_size c[6]=flags;
        // the static buffer pointer is rewritten by the kernel into c[2+normal].
        if(normal >= 6 && translate >= 2)
        {
            u16 dest    = (u16)(c[2] & 0xFFFF);
            u8  channel = (u8)(c[3] & 0xFF);
            u32 dsize   = c[5];
            u8  flags   = (u8)(c[6] & 0xFF);
            const u8 *data = (const u8 *)c[2 + normal];

            if(data && dsize > 0)
            {
                if(dsize > TX_MAX)
                    dsize = TX_MAX;
                u32 h = g_txHead;
                if(h - g_txTail < TX_RING)
                {
                    TxFrame *f = &g_tx[h % TX_RING];
                    f->len     = (u16)dsize;
                    f->dest    = dest;
                    f->channel = channel;
                    f->flags   = flags;
                    memcpy(f->data, data, dsize);
                    __dmb();
                    g_txHead = h + 1;
                }
            }
        }
        replySimple(c, 0x0017, 0);
        break;
    }

    case 0x0014: // PullPacket -> one host packet (or empty), in the expected format
    {
        u32 maxSize = c[3];   // max_out_buff_size
        u16 dataSize = 0, srcNode = 0;
        if(g_rxTail != g_rxHead)
        {
            RxFrame *f = &g_rx[g_rxTail % RX_RING];
            u32 len = f->len;
            if(len > maxSize)  len = maxSize;
            if(len > RX_MAX)   len = RX_MAX;
            memcpy(g_pullBuf, f->data, len);
            dataSize = (u16)len;
            srcNode  = f->srcNode;
            __dmb();
            g_rxTail++;
        }
        c[0] = IPC_MakeHeader(0x0014, 3, 2);
        c[1] = 0;
        c[2] = dataSize;
        c[3] = srcNode;
        c[4] = IPC_Desc_StaticBuffer(dataSize, 0);
        c[5] = (u32)g_pullBuf;
        break;
    }

    case 0x0003: // Shutdown
    case 0x0008: // DestroyNetwork
        relayResetSession();
        if(g_connEvent)
        {
            svcCloseHandle(g_connEvent);
            g_connEvent = 0;
        }
        replySimple(c, cmd, 0);
        break;

    default:
        // SetProbeResponseParam (0x21), UpdateNetworkAttribute (0x07),
        // SetApplicationData (0x10), Unbind (0x13)... a benign success is enough.
        replySimple(c, cmd, 0);
        break;
    }
}

// ---------------------------------------------------------------------------
//  Network thread: bridges the relay to the VPS over UDP.
// ---------------------------------------------------------------------------
static MyThread s_netThread;
static u8 CTR_ALIGN(8) s_netStack[0x2000];
static int s_sock = -1;
static u32 g_hostIp = 0;
static u64 g_pingTick = 0;   // system tick at the last heartbeat, for the RTT estimate
volatile u32 g_rttUs = 0;

// Bring up infrastructure Wi-Fi, like SysConfigMenu_ForceWifiConnection. Entering
// "local play" drops the internet connection, after which soc:U refuses to create a
// socket (ENETDOWN). Since nwm::UDS is stubbed the radio is actually free, so we can
// reconnect it in infrastructure mode.
static bool relayForceWifi(void)
{
    if(R_FAILED(acInit()))
        return false;

    u32 wifiStatus = 0, acStatus = 0;
    if(R_SUCCEEDED(ACU_GetWifiStatus(&wifiStatus)) && wifiStatus > 0 &&
       R_SUCCEEDED(ACU_GetStatus(&acStatus)) && acStatus != 1)
    {
        acExit();   // already connected (held by the game/system)
        return true;
    }

    for(u32 slot = 0; slot < 3; slot++)
    {
        acuConfig config = {0};
        ACU_CreateDefaultConfig(&config);
        ACU_SetNetworkArea(&config, 2);
        ACU_SetAllowApType(&config, 1 << slot);
        ACU_SetRequestEulaVersion(&config);

        Handle connectEvent = 0;
        svcCreateEvent(&connectEvent, RESET_ONESHOT);

        bool ok = false;
        char ssid[0x20 + 1] = {0};
        if(R_SUCCEEDED(ACU_ConnectAsync(&config, connectEvent)))
        {
            if(R_SUCCEEDED(svcWaitSynchronization(connectEvent, 20ULL * 1000 * 1000 * 1000)) &&
               R_SUCCEEDED(ACU_GetSSID(ssid)))
                ok = true;
        }
        svcCloseHandle(connectEvent);

        if(ok)
        {
            isConnectionForced = true;   // Rosalina closes ac on exit
            return true;
        }
    }

    acExit();
    return false;
}

static void nwmRelayNetMain(void)
{
restart:
    // Fresh state for a (new) session. We jump back here when the app terminates
    // (g_netStop) after closing soc, so the socket never outlives the session - which
    // is what crashes the SOCKET module on game exit otherwise.
    g_netStop = false;
    g_netStopped = false;
    g_wantNet = false;

    // Wait for a session (host, client, or a scan): soc:U is only reliable while a
    // game is running, not at Rosalina boot.
    while(g_status != NWM_HOST && g_status != NWM_CLIENT && !g_wantNet)
    {
        if(g_netStop) { g_netStopped = true; goto restart; }
        g_netState = 10;
        svcSleepThread(200 * 1000 * 1000LL);
    }

    g_netState = 13;
    if(!relayForceWifi()) { g_netState = -3; return; }
    svcSleepThread(1000ULL * 1000 * 1000);   // let the link settle

    g_netState = 11;
    Result initRet = miniSocInit();
    for(u32 t = 25; R_FAILED(initRet) && --t > 0; )
    {
        svcSleepThread(200 * 1000 * 1000LL);
        initRet = miniSocInit();
    }
    if(R_FAILED(initRet)) { g_netState = -1; return; }

    g_netState = 12;
    int sock = socSocket(AF_INET, SOCK_DGRAM, 0);
    for(u32 tries = 25; sock == -1 && --tries > 0; )
    {
        svcSleepThread(100 * 1000 * 1000LL);
        sock = socSocket(AF_INET, SOCK_DGRAM, 0);
    }
    if(sock < 0) { g_netState = -2; miniSocExit(); return; }
    s_sock = sock;

    g_hostIp = socGethostid();
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(0);
    local.sin_addr.s_addr = g_hostIp;
    socBind(s_sock, (struct sockaddr *)&local, sizeof(local));

    g_netState = 1;

    struct sockaddr_in vps;
    memset(&vps, 0, sizeof(vps));
    vps.sin_family      = AF_INET;
    vps.sin_port        = htons(VPS_PORT);
    vps.sin_addr.s_addr = htonl((VPS_IP_A << 24) | (VPS_IP_B << 16) | (VPS_IP_C << 8) | VPS_IP_D);

    char rx[1600];
    u8   pkt[12 + TX_MAX];
    u32  tick = 0;
    for(;;)
    {
        // App terminating: close soc before Rosalina tears down the network (acExit in
        // handlePreTermNotification), or our socRecv/Send on a dead network crash soc.
        if(g_netStop)
        {
            socClose(s_sock);
            miniSocExit();
            s_sock = -1;
            g_beaconLen = 0;
            g_netStopped = true;
            goto restart;
        }

        // Drain the game's SendTo queue -> NWMD frames.
        while(g_txTail != g_txHead)
        {
            TxFrame *f = &g_tx[g_txTail % TX_RING];
            u32 len = f->len;
            if(len > TX_MAX) len = TX_MAX;
            pkt[0]='N'; pkt[1]='W'; pkt[2]='M'; pkt[3]='D';
            pkt[4]=1; pkt[5]=0x17;
            pkt[6]=(u8)(f->dest & 0xFF); pkt[7]=(u8)(f->dest >> 8);
            pkt[8]=f->channel; pkt[9]=f->flags;
            pkt[10]=(u8)(len & 0xFF); pkt[11]=(u8)(len >> 8);
            memcpy(pkt + 12, f->data, len);
            socSendto(s_sock, pkt, 12 + len, 0, (struct sockaddr *)&vps, sizeof(vps));
            g_netTxCount++;
            __dmb();
            g_txTail++;
        }

        // Heartbeat ~every 2 s; timestamp it so the ACK gives us an RTT.
        if((tick % 400) == 0)
        {
            static const char msg[] = "NWMR:HOST";
            socSendto(s_sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&vps, sizeof(vps));
            g_netTxCount++;
            g_pingTick = svcGetSystemTick();
        }

        // Association request (ConnectToNetwork): resend NWMJ ~every 500 ms until NWMC.
        if(g_wantJoin && !g_joined && (tick % 100) == 0)
        {
            static const char jmsg[] = "NWMJ\x01";
            socSendto(s_sock, jmsg, sizeof(jmsg) - 1, 0, (struct sockaddr *)&vps, sizeof(vps));
            g_netTxCount++;
        }

        // Session ended: tell the shim to drop its association.
        if(g_sendReset)
        {
            socSendto(s_sock, "NWMX", 4, 0, (struct sockaddr *)&vps, sizeof(vps));
            g_netTxCount++;
            g_sendReset = false;
        }

        for(;;)
        {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = socRecvfrom(s_sock, rx, sizeof(rx), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
            if(n <= 0)
                break;
            g_netRxCount++;

            if(n >= 4 && rx[0]=='N' && rx[1]=='W' && rx[2]=='M' && rx[3]=='R')
            {
                // Heartbeat ACK -> RTT estimate (SYSCLOCK_ARM11 ~268.11 ticks/us).
                if(g_pingTick != 0)
                {
                    g_rttUs = (u32)((svcGetSystemTick() - g_pingTick) / 268);
                    g_pingTick = 0;
                }
            }
            else if(n >= 14 && rx[0]=='N' && rx[1]=='W' && rx[2]=='M' && rx[3]=='B')
            {
                // NWMB: ver ch mac[6] len(u16le) data.
                u8  ch   = (u8)rx[5];
                u16 blen = (u16)((u8)rx[12] | ((u8)rx[13] << 8));
                if(blen > 0 && blen <= BEACON_MAX && (ssize_t)(14 + blen) <= n)
                {
                    memcpy(g_beacon, rx + 14, blen);
                    memcpy(g_beaconMac, rx + 6, 6);
                    g_beaconChannel = ch;
                    __dmb();
                    g_beaconLen = blen;
                }
            }
            else if(n >= 7 && rx[0]=='N' && rx[1]=='W' && rx[2]=='M' && rx[3]=='C')
            {
                // NWMC: ver node(u16le) host_node. The shim finished the association.
                if(!g_joined)
                {
                    g_rxTail = g_rxHead;   // only deliver data from the connection onward
                    g_clientNode = (u16)((u8)rx[5] | ((u8)rx[6] << 8));
                    g_joined     = true;
                    g_wantJoin   = false;
                    g_status     = NWM_CLIENT;
                    if(g_connEvent)
                        svcSignalEvent(g_connEvent);
                }
            }
            else if(n >= 10 && rx[0]=='N' && rx[1]=='W' && rx[2]=='M' && rx[3]=='P')
            {
                // NWMP: ver ch src(u16le) len(u16le) data -> RX ring for PullPacket.
                u8  ch   = (u8)rx[5];
                u16 src  = (u16)((u8)rx[6] | ((u8)rx[7] << 8));
                u16 dlen = (u16)((u8)rx[8] | ((u8)rx[9] << 8));
                if(dlen > 0 && dlen <= RX_MAX && (ssize_t)(10 + dlen) <= n)
                {
                    u32 h = g_rxHead;
                    if(h - g_rxTail < RX_RING)
                    {
                        RxFrame *f = &g_rx[h % RX_RING];
                        f->len = dlen; f->srcNode = src; f->channel = ch;
                        memcpy(f->data, rx + 10, dlen);
                        __dmb();
                        g_rxHead = h + 1;
                        g_rxCount++;
                    }
                }
            }
        }

        tick++;
        svcSleepThread(5ULL * 1000 * 1000);   // 5 ms keeps the added latency low
    }
}

MyThread *nwmRelayCreateNetThread(void)
{
    MyThread_Create(&s_netThread, nwmRelayNetMain, s_netStack,
                    sizeof(s_netStack), 0x20, CORE_SYSTEM);
    return &s_netThread;
}

// Snapshot for the Rosalina diagnostic menu (it can't reach the static globals).
void NwmRelay_GetDiag(u32 out[10])
{
    out[0] = (u32)g_netState;
    out[1] = g_status;
    out[2] = g_joined ? 1u : 0u;
    out[3] = g_clientNode;
    out[4] = g_netTxCount;
    out[5] = g_netRxCount;
    out[6] = g_hostIp;
    out[7] = g_rttUs;
    out[8] = g_rxCount;
    out[9] = g_beaconLen;
}

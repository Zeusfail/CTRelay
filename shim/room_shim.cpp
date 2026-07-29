// Shim bridging a real 3DS relay to an Azahar-hosted UDS session over a citra_room.
//
//   3DS relay  <--UDP-->  [this shim]  <--ENet-->  citra_room  <-->  Azahar
//
// The relay speaks a tiny UDP protocol; the shim is a full room member and does the
// UDS-level work (discovery, association handshake, data framing). Nothing here is
// encrypted: azahar's netplay ships every WifiPacket in the clear, so we only have to
// (de)serialize plain 802.11/UDS structs.
//
// Relay <-> shim UDP frames:
//   NWMR:HOST / NWMR:ACK   heartbeat, lets the shim learn the relay's public ip:port
//   NWMB  shim->relay      beacon:   ver ch mac[6] len(u16le) data
//   NWMJ  relay->shim      "associate me to the host"
//   NWMC  shim->relay      "connected": ver node(u16le) hostnode(u16le)
//   NWMX  relay->shim      "session over": drop the association
//   NWMD  relay->shim      SendTo:   ver cmd dest(u16le) ch flags len(u16le) data
//   NWMP  shim->relay      host data: ver ch src(u16le) len(u16le) data
//
// Build: g++ -O2 -o room_shim room_shim.cpp -lenet   (needs libenet-dev)

#include <enet/enet.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

// --- citra_room Packet encoding (big-endian; strings are u32 length + bytes) ---
static void putU8(std::vector<u8>& p, u8 v) { p.push_back(v); }
static void putU32(std::vector<u8>& p, u32 v) { u32 n = htonl(v); const u8* b = (const u8*)&n; p.insert(p.end(), b, b + 4); }
static void putStr(std::vector<u8>& p, const std::string& s) { putU32(p, (u32)s.size()); p.insert(p.end(), s.begin(), s.end()); }
static void putMac(std::vector<u8>& p, const u8 m[6]) { p.insert(p.end(), m, m + 6); }
static void putVec(std::vector<u8>& p, const std::vector<u8>& v) { putU32(p, (u32)v.size()); p.insert(p.end(), v.begin(), v.end()); }

// --- little/big-endian field helpers for the raw UDS structs ---
static void be16(std::vector<u8>& v, u16 x) { v.push_back(x >> 8); v.push_back(x & 0xFF); }
static void le16(std::vector<u8>& v, u16 x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
static u16 rdbe16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
static u16 rdle16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }

enum WifiType { WBeacon = 0, WData = 1, WAuth = 2, WAssocResp = 3, WDeauth = 4, WNodeMap = 5 };
static constexpr u16 ETH_SECUREDATA = 0x876D, ETH_EAPOL = 0x888E;

static void sendWifi(ENetPeer* peer, ENetHost* host, u8 type, u8 ch,
                     const u8 tx[6], const u8 dst[6], const std::vector<u8>& data) {
    std::vector<u8> p;
    putU8(p, 5);                         // IdWifiPacket
    putU8(p, type);
    putU8(p, ch);
    putMac(p, tx);
    putMac(p, dst);
    putVec(p, data);
    ENetPacket* pk = enet_packet_create(p.data(), p.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pk);
    enet_host_flush(host);
}

// SNAP LLC header UDS puts in front of every 802.11 payload.
static std::vector<u8> llc(u16 proto) {
    std::vector<u8> v = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
    be16(v, proto);
    return v;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* roomHost = argc > 1 ? argv[1] : "127.0.0.1";
    u16 roomPort = argc > 2 ? (u16)atoi(argv[2]) : 24872;
    std::string nick = argc > 3 ? argv[3] : "relayshim";
    u16 udpPort = argc > 4 ? (u16)atoi(argv[4]) : 24873;

    // UDP side (talks to the 3DS relay).
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(udpPort);
    if (bind(udp, (sockaddr*)&local, sizeof(local)) < 0) { perror("bind"); return 1; }
    fcntl(udp, F_SETFL, O_NONBLOCK);
    printf("[shim] UDP listening on 0.0.0.0:%u\n", udpPort);

    // ENet side (room member).
    if (enet_initialize() != 0) { fprintf(stderr, "enet_initialize failed\n"); return 1; }
    ENetHost* client = enet_host_create(nullptr, 1, 1, 0, 0);
    ENetAddress address;
    enet_address_set_host(&address, roomHost);
    address.port = roomPort;
    ENetPeer* peer = enet_host_connect(client, &address, 1, 0);
    printf("[shim] connecting to room %s:%u\n", roomHost, roomPort);

    u8 myMac[6] = {0};
    bool haveMac = false;
    u8 beacon[600];
    u16 beaconLen = 0;
    u8 beaconMac[6] = {0};
    u8 beaconCh = 11;
    u8 hostMac[6] = {0};
    u8 channel = 11;
    bool haveHost = false;
    const u8 bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    bool haveRelay = false;
    sockaddr_in relayAddr{};
    enum { NONE, AUTH, EAPOL, DONE } assoc = NONE;
    u16 assocId = 0, myNode = 0, seq = 0;
    u32 tick = 0, lastBeaconTick = 0, dataIn = 0, dataOut = 0;

    for (;;) {
        ENetEvent ev;
        while (enet_host_service(client, &ev, 5) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                std::vector<u8> p;
                u8 anyMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // NoPreferredMac
                putU8(p, 1);                                          // IdJoinRequest
                putStr(p, nick);
                putStr(p, std::string("shim-cid-") + std::to_string(getpid()));
                putMac(p, anyMac);
                putU32(p, 4);                                         // network_version
                putStr(p, "");                                        // password
                putStr(p, "");                                        // token
                ENetPacket* pk = enet_packet_create(p.data(), p.size(), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(peer, 0, pk);
                enet_host_flush(client);
                printf("[shim] join request sent\n");
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                const u8* d = ev.packet->data;
                size_t len = ev.packet->dataLength;
                u8 id = len ? d[0] : 0;
                if (id == 2 && len >= 7) {                            // IdJoinSuccess: assigned MAC
                    memcpy(myMac, d + 1, 6);
                    haveMac = true;
                } else if (id == 5 && len >= 19) {                   // IdWifiPacket
                    u8 wtype = d[1], wch = d[2];
                    const u8* tx = d + 3;
                    const u8* dst = d + 9;
                    u32 dlen;
                    memcpy(&dlen, d + 15, 4);
                    dlen = ntohl(dlen);
                    const u8* payload = d + 19;
                    size_t plen = len - 19;
                    if (dlen < plen) plen = dlen;
                    bool forMe = memcmp(dst, myMac, 6) == 0 || memcmp(dst, bcast, 6) == 0;

                    if (wtype == WBeacon && plen > 0 && plen <= sizeof(beacon)) {
                        memcpy(beacon, payload, plen);
                        beaconLen = (u16)plen;
                        memcpy(beaconMac, tx, 6);
                        beaconCh = wch;
                        lastBeaconTick = tick;
                        // Azahar picks a fresh MAC every time it rejoins the room, so
                        // always track the current host address (except mid-session).
                        if (assoc != DONE) { memcpy(hostMac, tx, 6); channel = wch; }
                        haveHost = true;
                    } else if (wtype == WAssocResp && assoc == AUTH && forMe) {
                        assocId = rdle16(payload + 4) & 0x3FFF;
                        std::vector<u8> f = llc(ETH_EAPOL);
                        be16(f, 0x201);                              // EAPoL-Start magic
                        be16(f, assocId);
                        f.push_back(0x01);                           // ConnectionType::Client
                        f.insert(f.end(), {0, 0, 0});                // padding
                        f.insert(f.end(), {0, 0, 0, 0, 0, 0, 0, 1}); // friend_code_seed (dummy)
                        const char username[10] = "RELAY3DS";        // fixed 10 UTF-16 units, zero-padded
                        for (char c : username) be16(f, (u16)(u8)c);
                        for (int i = 0; i < 4; i++) f.push_back(0);  // padding
                        be16(f, 0);                                  // network_node_id
                        for (int i = 0; i < 6; i++) f.push_back(0);  // padding
                        sendWifi(peer, client, WData, channel, myMac, hostMac, f);
                        assoc = EAPOL;
                    } else if (wtype == WData && forMe && plen >= 8) {
                        u16 proto = rdbe16(payload + 6);
                        if (proto == ETH_EAPOL && plen >= 14 && rdbe16(payload + 8) == 0x202 && assoc == EAPOL) {
                            myNode = rdbe16(payload + 12);           // EAPoL-Logoff -> assigned node
                            assoc = DONE;
                            printf("[shim] connected as node %u\n", myNode);
                        } else if (proto == ETH_SECUREDATA && assoc == DONE && plen >= 22 && haveRelay) {
                            u16 protoSize = rdbe16(payload + 8);
                            u8 dataCh = payload[8 + 7];
                            u16 src = rdbe16(payload + 8 + 12);
                            int paylen = (int)plen - 22;
                            if (protoSize >= 14 && (int)(protoSize - 14) <= paylen) paylen = protoSize - 14;
                            if (paylen > 0) {
                                std::vector<u8> p = {'N', 'W', 'M', 'P', 1, dataCh};
                                p.push_back(src & 0xFF); p.push_back(src >> 8);
                                p.push_back(paylen & 0xFF); p.push_back((paylen >> 8) & 0xFF);
                                p.insert(p.end(), payload + 22, payload + 22 + paylen);
                                sendto(udp, p.data(), p.size(), 0, (sockaddr*)&relayAddr, sizeof(relayAddr));
                                dataIn++;
                            }
                        }
                    }
                }
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                printf("[shim] room disconnected, reconnecting\n");
                peer = enet_host_connect(client, &address, 1, 0);
            }
        }

        for (;;) {
            u8 buf[2048];
            sockaddr_in from{};
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(udp, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n <= 0) break;

            if (n >= 4 && memcmp(buf, "NWMR", 4) == 0) {
                if (!haveRelay) {
                    printf("[shim] relay at %s:%u\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                    haveRelay = true;
                }
                relayAddr = from;
                sendto(udp, "NWMR:ACK", 8, 0, (sockaddr*)&from, sizeof(from));
            } else if (n >= 4 && memcmp(buf, "NWMX", 4) == 0) {
                // The relay's game session ended. Deauth our node so the host frees it
                // (otherwise a re-auth on the same MAC is rejected as already connected),
                // then go idle until the next join request.
                if (assoc == DONE) {
                    sendWifi(peer, client, WDeauth, channel, myMac, hostMac, {});
                }
                assoc = NONE;
                myNode = 0;
                seq = 0;
            } else if (n >= 4 && memcmp(buf, "NWMJ", 4) == 0) {
                // Any join request while already connected means a new attempt (the relay
                // stops sending NWMJ once joined), so tear down the old node first.
                if (assoc == DONE) {
                    sendWifi(peer, client, WDeauth, channel, myMac, hostMac, {});
                    assoc = NONE;
                    myNode = 0;
                    seq = 0;
                }
                if (haveHost && haveMac && (assoc == NONE || assoc == AUTH)) {
                    std::vector<u8> auth;
                    le16(auth, 0);                                   // OpenSystem
                    le16(auth, 1);                                   // SEQ1
                    le16(auth, 0);                                   // status
                    sendWifi(peer, client, WAuth, channel, myMac, hostMac, auth);
                    assoc = AUTH;
                }
            } else if (n >= 12 && memcmp(buf, "NWMD", 4) == 0) {
                u16 dest = buf[6] | (buf[7] << 8);
                u8 ch = buf[8];
                u16 dl = buf[10] | (buf[11] << 8);
                if (assoc == DONE && dl > 0 && (size_t)(12 + dl) <= (size_t)n) {
                    std::vector<u8> f = llc(ETH_SECUREDATA);
                    u16 protoSize = (u16)(dl + 14);
                    be16(f, protoSize);
                    f.insert(f.end(), {0, 0});                       // padding
                    be16(f, (u16)(protoSize - 4));                   // securedata_size
                    f.push_back(0);                                  // is_management
                    f.push_back(ch);
                    be16(f, seq++);
                    be16(f, dest);
                    be16(f, myNode);                                 // src_node_id
                    f.insert(f.end(), buf + 12, buf + 12 + dl);
                    sendWifi(peer, client, WData, channel, myMac, hostMac, f);
                    dataOut++;
                }
            }
        }

        if (haveRelay && beaconLen > 0 && tick - lastBeaconTick < 400 && tick % 60 == 0) {
            std::vector<u8> p = {'N', 'W', 'M', 'B', 1, beaconCh};
            p.insert(p.end(), beaconMac, beaconMac + 6);
            p.push_back(beaconLen & 0xFF);
            p.push_back(beaconLen >> 8);
            p.insert(p.end(), beacon, beacon + beaconLen);
            sendto(udp, p.data(), p.size(), 0, (sockaddr*)&relayAddr, sizeof(relayAddr));
        }
        if (assoc == DONE && haveRelay && tick % 25 == 0) {
            u8 c[8] = {'N', 'W', 'M', 'C', 1, (u8)(myNode & 0xFF), (u8)(myNode >> 8), 1};
            sendto(udp, c, 8, 0, (sockaddr*)&relayAddr, sizeof(relayAddr));
        }
        if (tick % 500 == 0) {
            printf("[shim] beacon=%s host=%s relay=%s assoc=%d node=%u in=%u out=%u\n",
                   beaconLen ? "y" : "n", haveHost ? "y" : "n", haveRelay ? "y" : "n",
                   (int)assoc, myNode, dataIn, dataOut);
        }
        tick++;
    }
}

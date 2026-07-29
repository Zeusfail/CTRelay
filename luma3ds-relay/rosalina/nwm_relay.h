/*
*   nwm_relay.h - software nwm::UDS server + VPS bridge (see nwm_relay.c).
*/
#pragma once

#include <3ds/types.h>
#include "MyThread.h"

// Service handler for "nwmrelay" (registered in Rosalina's service list).
void NwmRelay__HandleCommands(void *ctx);

// Creates the UDP bridge thread. Call once at Rosalina startup.
MyThread *nwmRelayCreateNetThread(void);

// Diagnostic snapshot for the Rosalina menu:
//   [0]netState [1]status [2]joined [3]node [4]tx [5]rx
//   [6]hostIp [7]rttUs [8]dataIn [9]beaconLen
void NwmRelay_GetDiag(u32 out[10]);

// Pre-termination handshake: main.c sets g_netStop and waits for g_netStopped so the
// bridge thread closes its socket before the network is torn down (otherwise the
// SOCKET module crashes on game exit).
extern volatile bool g_netStop;
extern volatile bool g_netStopped;

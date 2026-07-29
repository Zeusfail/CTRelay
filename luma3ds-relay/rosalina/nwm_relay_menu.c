/*
*   Rosalina menu to control the local Wi-Fi relay.
*
*   The relay is a system sysmodule, so its control lives here rather than in a game
*   plugin - a plugin reserves the game's RAM and larger titles (e.g. Mario Kart 7)
*   run out of memory and crash on launch.
*/
#include <3ds.h>
#include "menus/nwm_relay_menu.h"
#include "nwm_relay.h"
#include "draw.h"
#include "menu.h"
#include "csvc.h"

#define SERVICEOP_SET_RELAY_ENABLED  ((ServiceOp)5)   // must match ControlService.c

extern bool menuShouldExit;

static bool s_enabled = false;

Menu nwmRelayMenu = {
    "Local Wi-Fi relay",
    {
        { "Enable / disable relay",   METHOD, .method = &NwmRelayMenu_Toggle },
        { "Connection diagnostic",    METHOD, .method = &NwmRelayMenu_Diag },
        {},
    }
};

void NwmRelayMenu_Toggle(void)
{
    s_enabled = !s_enabled;
    Result res = svcControlService(SERVICEOP_SET_RELAY_ENABLED, s_enabled ? 1 : 0);

    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Local Wi-Fi relay");
        Draw_DrawFormattedString(10, 30, s_enabled ? COLOR_GREEN : COLOR_WHITE,
            "Relay %s          ", s_enabled ? "ON  (nwm::UDS is redirected)" : "OFF (stock behaviour)  ");
        if(R_FAILED(res))
            Draw_DrawFormattedString(10, 50, COLOR_RED, "svcControlService failed: 0x%08lx", (u32)res);
        Draw_DrawString(10, 80, COLOR_WHITE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(waitInput() & KEY_B) && !menuShouldExit);
}

void NwmRelayMenu_Diag(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        u32 d[10];
        NwmRelay_GetDiag(d);
        u32 ip = d[6];

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "Local Wi-Fi relay - diagnostic");
        Draw_DrawFormattedString(10, 30, COLOR_WHITE,
            "state=%ld  status=%lu  joined=%lu  node=%lu   ",
            (long)(s32)d[0], d[1], d[2], d[3]);
        Draw_DrawFormattedString(10, 45, COLOR_WHITE,
            "sent=%lu  received=%lu  hostData=%lu       ", d[4], d[5], d[8]);
        Draw_DrawFormattedString(10, 60, COLOR_WHITE,
            "console IP = %lu.%lu.%lu.%lu     ",
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        Draw_DrawFormattedString(10, 75, COLOR_WHITE,
            "RTT to VPS = %lu us (~%lu ms)   ", d[7], d[7] / 1000);
        Draw_DrawFormattedString(10, 90, COLOR_WHITE,
            "beacon cached = %lu bytes        ", d[9]);
        Draw_DrawString(10, 115, COLOR_WHITE, "Press B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while(!(waitInput() & KEY_B) && !menuShouldExit);
}

#pragma once
#include <stdbool.h>

#define DECK_AP_SSID "BitaxeDeck-Setup"
#define DECK_AP_PASS "satoshi21"

typedef enum {
    NET_CONNECTING = 0,
    NET_CONNECTED,
    NET_PORTAL, // eigener Access-Point aktiv, wartet auf Einrichtung
} net_state_t;

void net_init(void);
net_state_t net_state(void);
bool net_time_valid(void);
void net_ip(char *out, int len); // IP im Heimnetz oder 192.168.4.1 im Setup-Modus
int net_rssi(void);

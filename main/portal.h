#pragma once
#include <stdint.h>
#include <stdbool.h>

/* WiFi book transfer: hotspot (or home WiFi) + web server + captive DNS. */

typedef enum { PORTAL_OFF, PORTAL_CONNECTING, PORTAL_AP, PORTAL_STA } portal_mode_t;

typedef struct {
    portal_mode_t mode;
    char ssid[33];
    char pass[65];
    char ip[16];
    int clients;
    /* upload progress, written by the web server task */
    volatile bool uploading;
    char up_name[64];
    volatile uint32_t up_recv, up_total;
    volatile int received;     /* books received since start */
    volatile int changes;      /* incremented on any library change */
    char message[64];
} portal_status_t;

/* try_home: connect to the saved home network first (falls back to hotspot) */
void portal_start(bool try_home);
void portal_stop(void);
portal_status_t *portal_status(void);

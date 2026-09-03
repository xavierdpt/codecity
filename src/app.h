#ifndef APP_H
#define APP_H
#include "model.h"
#include "world.h"

#define BR_MAX 4096

typedef struct {
    Elf   *elf;
    City  *city;
    Player p;

    char   msg[320];
    float  msgT;
    char   prompt[192];
    int    promptKind;          /* 0 none, 1 portal, 2 door */
    int    promptIdx;

    int    showHelp, showMap, showBrowser, showDetail;
    int    wire, freefly;
    float  daylight;

    /* file browser */
    char   bdir[512];
    char  *bent[BR_MAX];
    int    nbent, bsel, bfilterlen;
    char   bfilter[64];
    int   *bfiltered, nbfiltered;

    int    winw, winh;
    float  fps;
    float  now;             /* seconds since start, drives the animation */
    int    nearIns;         /* instruction the player is standing next to */
    int    lastB, lastR;    /* room we last decoded, so we know when to free it */
} App;

void app_message(App *a, const char *fmt, ...);
#endif

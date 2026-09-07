#ifndef APP_H
#define APP_H
#include "model.h"
#include "world.h"

typedef struct Wisp Wisp;   /* wisp.h; App only ever holds a pointer */

#define BR_MAX 4096

typedef struct {
    Elf   *elf;
    City  *city;
    Player p;

    char   msg[320];
    float  msgT;
    char   prompt[192];
    int    promptKind;          /* 0 none, 1 portal, 2 door, 3 exit port */
    int    promptIdx;
    uint64_t promptAddr;        /* where an exit port leads */
    char   promptName[96];

    int    showHelp, showMap, showBrowser, showDetail, showState;
    int    wire, freefly;
    float  daylight;

    /* file browser */
    char   bdir[512];
    char  *bent[BR_MAX];
    int    nbent, bsel, bfilterlen;
    char   bfilter[64];
    int   *bfiltered, nbfiltered;

    /* §13's text stress test: 0 off, 1 through the mono atlas, 2 through
       the string cache -- the same 200 changing strings either way   */
    int    stress;

    /* the wisp: a CPU state walking the room you are standing in.  One at a
       time, spawned by `x` and freed when the room's decoding is.       */
    Wisp  *wisp;
    int    wispSeed;            /* bumped by `n` for a different run */
    int    keepWisp;            /* a room change the wisp itself asked for */
    /* the wisp is standing at a door with its hand on the handle: pilot
       mode never teleports without being told to */
    char   wispAsk[320];
    int    wispAskKind;         /* 0 none, 1 a call, 2 coming back, 3 leaving */
    /* Where the wisp is.  Usually the room you are in; when you have let it
       go on without you, somewhere else entirely -- and then its room is
       decoded as well as yours, which is the only time two are.      */
    int    wispBi, wispRi;
    int    wispAway;            /* it is not in the room you are standing in */
    int    wispDoorBi, wispDoorRi;   /* the call it went out by, so the room
                                        it left can still show where it went */
    uint64_t wispDoorAddr;

    int    winw, winh;
    float  fps;
    float  swapMs, stallMs;     /* last and worst time spent handing a frame over */
    float  now;             /* seconds since start, drives the animation */
    int    nearIns;         /* instruction the player is standing next to */
    int    lastB, lastR, lastU; /* room+alcove last decoded, so we know when to free */
} App;

void app_message(App *a, const char *fmt, ...);
#endif

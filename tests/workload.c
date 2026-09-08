/* workload.c -- the program docs/live-wisps.md measures against.
 *
 * §4's residency measurement needs a program that spends its time in its
 * own code rather than in libc: stepping /bin/ls from its entry point puts
 * 99.7% of the first 30 000 instructions in the loader, and nothing can be
 * learned about stepping from that.  `crunch` is a leaf -- no calls at all
 * -- so a live wisp inside it stays on the map for as long as you like,
 * which is what --steptest needs to measure a per-step cost.
 *
 *   cc -O0 -g -o tests/workload tests/workload.c
 *
 * -O0 on purpose: optimised, the loop folds away and there is nothing to
 * walk.  Not built by the Makefile -- it is a fixture, and the tests that
 * want it say so.
 */
#include <stdio.h>
#include <string.h>

/* §8.4's case: initialised data the program changes.  The file says what
   these started as; only the process knows what they are now, which is
   exactly the difference the live memory layer exists to show.      */
volatile unsigned long long mutated = 0x1111111111111111ull;
volatile unsigned long long counter = 0x2222222222222222ull;

/* a leaf: no calls, so a wisp in here never leaves the file */
static int crunch(int n){
    int s = 0;
    for (int i = 0; i < n; i++){
        s += (i * i) ^ (s >> 3);
        if (s & 1) s += 7;
    }
    return s;
}

int main(void){
    int t = 0;
    char buf[64];
    mutated = 0xfeedfacecafebeefull;      /* .data, changed at once */
    counter = 0x0badc0de0badc0deull;
    /* Short crunches, so main comes back round often and the walk meets a
       call out of the file every few hundred instructions -- that is what
       §4's step-over policy is measured against.                     */
    for (int k = 0; k < 400; k++){
        t += crunch(40);
        snprintf(buf, sizeof buf, "%d", t);   /* into libc, and back */
        t += (int)strlen(buf);                /* ... and again */
    }
    /* One long stay in the leaf, for measuring the cost of a step with
       nothing at all in the way.                                     */
    t += crunch(200000);
    printf("%d\n", t);
    return 0;
}

/* attachable.c -- a target for --attachtest.
 *
 * Yama's ptrace_scope=1, the desktop default, lets a process be traced only
 * by one of its ancestors.  gdb spawned by codecity is a *sibling* of a
 * process codecity spawned, so attaching to it is refused -- which is the
 * whole point of §10.2, and the reason --attach is the bonus rather than
 * the feature.  A program can opt in on its own behalf, and this one does,
 * so the permitted path can be tested on a stock machine too.
 *
 *   cc -O0 -g -o tests/attachable tests/attachable.c
 *
 * Not built by the Makefile: it is a fixture, and the test that wants it
 * says so.  Runs for a bounded time so a stray copy cannot outlive the
 * test that started it.
 */
#include <stdio.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static volatile unsigned long spins;

int main(int argc, char **argv){
#ifdef __linux__
    /* "anyone may trace me".  Only meaningful under ptrace_scope=1; under
       0 it changes nothing, under 2 or 3 it cannot help.               */
    if (argc < 2 || argv[1][0] != 'n')
        prctl(PR_SET_PTRACER, -1 /* PR_SET_PTRACER_ANY */, 0, 0, 0);
#else
    (void)argc; (void)argv;
#endif
    printf("%d\n", (int)getpid());
    fflush(stdout);
    for (int s = 0; s < 120; s++){
        for (int i = 0; i < 200000; i++) spins++;
        usleep(50000);
    }
    return 0;
}

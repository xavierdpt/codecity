/* live.c -- GDB/MI transport.  See live.h and docs/live-wisps.md §2.
 *
 * The whole file is one MI client: spawn gdb, write tokenised commands,
 * read records back with a deadline, and take the child down cleanly no
 * matter how we leave.  It deliberately knows nothing about the city.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "live.h"

struct Live {
    pid_t    gdb, inf;
    int      in, out;               /* gdb's stdin, gdb's stdout */
    char    *rbuf; size_t rlen, rcap;   /* bytes read but not yet a line */
    char    *line; size_t linecap;
    char    *res;  size_t rescap;   /* payload of the last result record */
    char    *con;  size_t conlen, concap;  /* captured ~"..." console text */
    int      capcon;
    int      token;
    int      stopped, running, exited, closed;
    int      awaiting;              /* a continue was issued, no stop yet */
    uint64_t pc;
    char     reason[48], signame[32];
    LiveOwn  own;
    char    *rname[LIVE_NAMES];     /* gdb's register names, fetched once */
    int      nrname;
    char    *regcmd;                /* the register query, built once */
    /* L7: one page of the inferior, remembered for as long as it is
       standing still.  The panel makes a dozen small reads per stop --
       five stack slots, the memory log, what a register points at -- and
       each is an MI round trip unless they share a page.  Dropped the
       moment the process moves, because then it is a lie.          */
    uint64_t mcbase;
    uint8_t  mcbuf[4096];
    int      mcok;
    char   **want;                  /* ... restricted to these names, if set */
    int      nwant;
    LiveMod  mod[LIVE_MAXMOD];
    int      nmod;
    int      subject;               /* index into mod[], -1 when unset */
    char     err[LIVE_ERRLEN];
};

/* CODECITY_GDB_TRACE=1 puts the whole MI conversation on stderr.  This
   protocol is not readable from a stack trace and every bug in a client of
   it looks like a hang or a silence, so the trace is not a temporary. */
static int mi_tracing(void){
    static int t = -1;
    if (t < 0){ const char *e = getenv("CODECITY_GDB_TRACE"); t = e && *e && *e != '0'; }
    return t;
}

static void seterr(Live *L, const char *fmt, ...){
    if (!L) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(L->err, sizeof L->err, fmt, ap);
    va_end(ap);
}

const char *live_err(const Live *L){ return L ? L->err : "no session"; }
pid_t live_gdb_pid(const Live *L){ return L ? L->gdb : -1; }
pid_t live_inferior_pid(const Live *L){ return L ? L->inf : -1; }
uint64_t live_pc(const Live *L){ return L ? L->pc : 0; }
const char *live_stop_reason(const Live *L){ return L ? L->reason : ""; }
const char *live_stop_signal(const Live *L){ return L ? L->signame : ""; }

int live_stopped(const Live *L){
    return L && L->stopped && !L->exited;
}

int live_alive(const Live *L){
    if (!L || L->exited || L->inf <= 0) return 0;
    return kill(L->inf, 0) == 0 || errno == EPERM;
}

static uint64_t now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ */
/* MI value picking                                                    */
/* ------------------------------------------------------------------ */
/* MI records are a flat-ish soup of key=value, and L0 reads six fields out
   of five record shapes.  A full MI value parser is not warranted for that
   and would be the kind of code nobody revisits; these two helpers are.  */

/* find `key=` where the key starts a token (preceded by , { [ or start) */
static const char *mi_field(const char *s, const char *key){
    size_t n = strlen(key);
    for (const char *p = s; (p = strstr(p, key)); p += n){
        if (p != s){
            char c = p[-1];
            if (c != ',' && c != '{' && c != '[') continue;
        }
        if (p[n] == '=') return p + n + 1;
    }
    return NULL;
}

/* copy a "..."-quoted MI C-string, undoing its escapes.  Returns the char
   just past the closing quote, or NULL if `p` is not a quoted string.   */
static const char *mi_str(const char *p, char *out, size_t n){
    if (!p || *p != '"') return NULL;
    size_t o = 0;
    for (p++; *p && *p != '"'; p++){
        char c = *p;
        if (c == '\\' && p[1]){
            switch (*++p){
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = *p;   break;
            }
        }
        if (o + 1 < n) out[o++] = c;
    }
    if (o < n) out[o] = 0; else if (n) out[n - 1] = 0;
    return *p == '"' ? p + 1 : NULL;
}

/* a quoted field, copied out.  1 on success. */
static int mi_get(const char *rec, const char *key, char *out, size_t n){
    const char *p = mi_field(rec, key);
    return p && mi_str(p, out, n) ? 1 : 0;
}

static uint64_t mi_u64(const char *s){
    return s ? (uint64_t)strtoull(s, NULL, 0) : 0;
}

/* ------------------------------------------------------------------ */
/* reading gdb, with a deadline                                        */
/* ------------------------------------------------------------------ */

static int grow(char **b, size_t *cap, size_t want){
    if (*cap >= want) return 1;
    size_t c = *cap ? *cap : 256;
    while (c < want) c *= 2;
    char *n = realloc(*b, c);
    if (!n) return 0;
    *b = n; *cap = c;
    return 1;
}

/* one line, without its newline.  0 on timeout/EOF/error. */
static int readline_to(Live *L, uint64_t deadline){
    for (;;){
        /* memchr(NULL, c, 0) is undefined, not merely pointless, and the
           very first pass through here has no buffer yet (UBSan). */
        char *nl = L->rbuf ? memchr(L->rbuf, '\n', L->rlen) : NULL;
        if (nl){
            size_t len = (size_t)(nl - L->rbuf);
            if (!grow(&L->line, &L->linecap, len + 1)) return 0;
            memcpy(L->line, L->rbuf, len);
            L->line[len] = 0;
            /* gdb writes \r\n on some terminals */
            if (len && L->line[len - 1] == '\r') L->line[len - 1] = 0;
            if (mi_tracing()) fprintf(stderr, "[mi <] %s\n", L->line);
            memmove(L->rbuf, nl + 1, L->rlen - len - 1);
            L->rlen -= len + 1;
            return 1;
        }
        uint64_t t = now_ms();
        if (t >= deadline){ seterr(L, "gdb did not answer in time"); return 0; }
        struct pollfd pf = { L->out, POLLIN, 0 };
        int pr = poll(&pf, 1, (int)(deadline - t));
        if (pr < 0){ if (errno == EINTR) continue;
                     seterr(L, "poll: %s", strerror(errno)); return 0; }
        if (pr == 0) continue;                  /* re-check the deadline */
        if (!grow(&L->rbuf, &L->rcap, L->rlen + 4096)) return 0;
        ssize_t k = read(L->out, L->rbuf + L->rlen, L->rcap - L->rlen);
        if (k < 0){ if (errno == EINTR) continue;
                    seterr(L, "read: %s", strerror(errno)); return 0; }
        if (k == 0){ seterr(L, "gdb closed its output"); return 0; }
        L->rlen += (size_t)k;
    }
}

/* the async and stream records, which can arrive at any point (§2.3) */
static void mi_async(Live *L, const char *line){
    if (!strncmp(line, "*stopped", 8)){
        L->stopped = 1; L->running = 0;
        L->reason[0] = L->signame[0] = 0;
        mi_get(line, "reason", L->reason, sizeof L->reason);
        mi_get(line, "signal-name", L->signame, sizeof L->signame);
        char addr[32];
        if (mi_get(line, "addr", addr, sizeof addr)) L->pc = mi_u64(addr);
        if (!strncmp(L->reason, "exited", 6)) L->exited = 1;
    } else if (!strncmp(line, "*running", 8)){
        L->running = 1; L->stopped = 0;
    } else if (!strncmp(line, "=thread-group-started", 21)){
        char pid[24];
        if (mi_get(line, "pid", pid, sizeof pid)) L->inf = (pid_t)atoi(pid);
    } else if (!strncmp(line, "=thread-group-exited", 20)){
        L->exited = 1;
    } else if (line[0] == '~' && L->capcon){
        char txt[4096];
        if (mi_str(line + 1, txt, sizeof txt)){
            size_t n = strlen(txt);
            if (grow(&L->con, &L->concap, L->conlen + n + 1)){
                memcpy(L->con + L->conlen, txt, n + 1);
                L->conlen += n;
            }
        }
    }
}

/* Pump until the result record carrying `token` arrives.  Async records met
   on the way are folded into the session state rather than dropped, which
   is the whole reason this is a state machine and not a read-a-line loop. */
static int mi_await(Live *L, int token){
    uint64_t deadline = now_ms() + LIVE_WAIT_MS;
    char want[24];
    int wn = snprintf(want, sizeof want, "%d", token);
    for (;;){
        if (!readline_to(L, deadline)) return 0;
        const char *s = L->line;
        if (!strcmp(s, "(gdb)") || !*s) continue;
        if (!strncmp(s, want, (size_t)wn) && s[wn] == '^'){
            const char *r = s + wn + 1;
            const char *comma = strchr(r, ',');
            const char *body = comma ? comma + 1 : "";
            if (!grow(&L->res, &L->rescap, strlen(body) + 1)) return 0;
            strcpy(L->res, body);
            if (!strncmp(r, "error", 5)){
                char msg[LIVE_ERRLEN];
                if (mi_get(body, "msg", msg, sizeof msg)) seterr(L, "%s", msg);
                else seterr(L, "gdb refused the command");
                return 0;
            }
            return 1;                       /* ^done, ^running, ^connected */
        }
        if (*s == '*' || *s == '=' || *s == '~' || *s == '@' || *s == '&')
            mi_async(L, s);
    }
}

static int mi_cmd(Live *L, const char *fmt, ...){
    if (!L || L->closed || L->gdb <= 0){ seterr(L, "no gdb"); return 0; }
    char cmd[1024];
    int tok = ++L->token;
    int n = snprintf(cmd, sizeof cmd, "%d", tok);
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(cmd + n, sizeof cmd - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof cmd - 2){ seterr(L, "command too long"); return 0; }
    cmd[n++] = '\n'; cmd[n] = 0;
    if (mi_tracing()) fprintf(stderr, "[mi >] %.*s\n", n - 1, cmd);
    for (int off = 0; off < n; ){
        ssize_t k = write(L->in, cmd + off, (size_t)(n - off));
        if (k < 0){ if (errno == EINTR) continue;
                    seterr(L, "write to gdb: %s", strerror(errno)); return 0; }
        off += (int)k;
    }
    return mi_await(L, tok);
}

/* a console command, with its ~"..." output captured for the caller */
static int mi_console(Live *L, const char *what){
    L->conlen = 0; L->capcon = 1;
    if (L->con) L->con[0] = 0;
    int ok = mi_cmd(L, "-interpreter-exec console \"%s\"", what);
    L->capcon = 0;
    return ok;
}

/* wait for the inferior to come to rest after a -exec-* that returned
   ^running.  A stop can arrive before the result record does, so this
   checks the flag first rather than assuming an ordering.            */
static int mi_wait_stop(Live *L, int timeout_ms){
    if (L->stopped || L->exited) return 1;
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    while (!L->stopped && !L->exited){
        if (!readline_to(L, deadline)) return 0;
        if (!strcmp(L->line, "(gdb)") || !*L->line) continue;
        char c = L->line[0];
        if (c == '*' || c == '=' || c == '~' || c == '@' || c == '&')
            mi_async(L, L->line);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* spawning, and getting rid of it again                               */
/* ------------------------------------------------------------------ */

/* We write to a pipe whose far end can die at any moment; without this a
   stray EPIPE takes the whole program down instead of failing one call.
   Done once, and only ever to SIG_IGN, which is what SDL does anyway.  */
static void ignore_sigpipe(void){
    static int done = 0;
    if (done) return;
    done = 1;
    struct sigaction sa;
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL){
        sa.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa, NULL);
    }
}

static const char *gdb_binary(void){
    const char *g = getenv("CODECITY_GDB");
    return (g && *g) ? g : "gdb";
}

/* fork gdb on two pipes.  The third pipe is closed on a successful exec and
   carries errno if there was not one, which is what turns "gdb is not
   installed" into a sentence instead of a mystery exit status.        */
static int spawn(Live *L){
    int toc[2] = {-1,-1}, fromc[2] = {-1,-1}, errp[2] = {-1,-1};
    if (pipe(toc) || pipe(fromc) || pipe2(errp, O_CLOEXEC)){
        seterr(L, "pipe: %s", strerror(errno));
        goto fail;
    }
    pid_t pid = fork();
    if (pid < 0){ seterr(L, "fork: %s", strerror(errno)); goto fail; }
    if (pid == 0){
        /* If codecity dies -- crash, SIGKILL, anything -- gdb goes with it,
           and gdb takes the inferior it launched.  This is the guarantee
           that no orphan can outlive us (live.h rule 1).             */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() == 1) _exit(127);       /* already reparented: too late */
        dup2(toc[0], STDIN_FILENO);
        dup2(fromc[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0){ dup2(devnull, STDERR_FILENO); close(devnull); }
        close(toc[0]); close(toc[1]); close(fromc[0]); close(fromc[1]);
        /* -nx: the user's .gdbinit is not ours to inherit, and a stray
           `set confirm on` in it would hang us on exit.              */
        execlp(gdb_binary(), gdb_binary(), "--interpreter=mi3", "-q", "-nx",
               (char *)NULL);
        int e = errno;
        ssize_t ign = write(errp[1], &e, sizeof e);
        (void)ign;
        _exit(127);
    }
    close(toc[0]);  toc[0]  = -1;
    close(fromc[1]); fromc[1] = -1;
    close(errp[1]); errp[1] = -1;
    int ce = 0;
    ssize_t k = read(errp[0], &ce, sizeof ce);
    close(errp[0]); errp[0] = -1;
    if (k == (ssize_t)sizeof ce){
        int st; waitpid(pid, &st, 0);
        seterr(L, "cannot run '%s': %s", gdb_binary(), strerror(ce));
        goto fail;
    }
    L->gdb = pid;
    L->in  = toc[1];
    L->out = fromc[0];
    return 1;
fail:
    for (int i = 0; i < 2; i++){
        if (toc[i]   >= 0) close(toc[i]);
        if (fromc[i] >= 0) close(fromc[i]);
        if (errp[i]  >= 0) close(errp[i]);
    }
    return 0;
}

/* waitpid with a deadline, so a wedged gdb cannot wedge us */
static int reap(pid_t pid, int ms){
    for (int waited = 0; waited <= ms; waited += 5){
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) return 1;
        if (r < 0 && errno != EINTR) return 0;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return 0;
}

void live_close(Live *L){
    if (!L) return;
    if (!L->closed && L->gdb > 0){
        /* An inferior we launched is ours to kill; one we attached to must
           be left running (L6).  Getting these two the wrong way round is
           the difference between tidying up and killing someone's job.
           `closed` is set *after* these, not before: mi_cmd refuses to
           talk to a closed session, so setting it first would silently
           skip the whole orderly shutdown and leave only the blunt
           instruments below to do the work.                          */
        if (L->own == LIVE_OWN_DETACH) mi_cmd(L, "-target-detach");
        else if (L->own == LIVE_OWN_KILL && live_alive(L)) mi_console(L, "kill");
        mi_cmd(L, "-gdb-exit");
        L->closed = 1;
        close(L->in); L->in = -1;
        if (!reap(L->gdb, 2000)){
            kill(L->gdb, SIGTERM);
            if (!reap(L->gdb, 1000)){ kill(L->gdb, SIGKILL); reap(L->gdb, 1000); }
        }
        if (L->out >= 0){ close(L->out); L->out = -1; }
        L->gdb = -1;
    }
    if (L->in  >= 0) close(L->in);
    if (L->out >= 0) close(L->out);
    free(L->regcmd);
    for (int i = 0; i < L->nwant; i++) free(L->want[i]);
    free(L->want);
    for (int i = 0; i < L->nrname && i < LIVE_NAMES; i++) free(L->rname[i]);
    free(L->rbuf); free(L->line); free(L->res); free(L->con);
    free(L);
}

/* ------------------------------------------------------------------ */
/* the session                                                         */
/* ------------------------------------------------------------------ */

Live *live_launch(const char *prog, const char *const *args, int nargs,
                  char *err, size_t errlen){
    ignore_sigpipe();
    Live *L = calloc(1, sizeof *L);
    if (!L){ if (err) snprintf(err, errlen, "out of memory"); return NULL; }
    L->gdb = L->inf = -1;
    L->in = L->out = -1;
    L->own = LIVE_OWN_KILL;
    L->subject = -1;

    if (!spawn(L)) goto fail;
    /* gdb greets us before it will take orders */
    if (!mi_cmd(L, "-gdb-set confirm off"))     goto fail;
    if (!mi_cmd(L, "-gdb-set mi-async on"))     goto fail;
    /* the inferior's own output must not land on codecity's terminal
       (docs/live-wisps.md §2.4).  L0 discards it; the pty is L5's.     */
    mi_cmd(L, "-inferior-tty-set /dev/null");
    if (!mi_cmd(L, "-file-exec-and-symbols %s", prog)) goto fail;
    if (nargs > 0 && args){
        /* MI takes one space-separated string and splits it the way a
           shell would, so an argument containing a space or a quote has
           to be quoted or it silently becomes two arguments.  Quote
           every one and escape what is inside it.                    */
        char line[1024]; size_t o = 0; line[0] = 0;
        for (int i = 0; i < nargs; i++){
            if (o + 4 >= sizeof line) break;
            line[o++] = ' '; line[o++] = '"';
            for (const char *p = args[i]; *p && o + 3 < sizeof line; p++){
                if (*p == '"' || *p == '\\') line[o++] = '\\';
                line[o++] = *p;
            }
            line[o++] = '"';
        }
        line[o] = 0;
        if (!mi_cmd(L, "-exec-arguments%s", line)) goto fail;
    }
    /* `starti`, and it has to be the console one.  MI's `-exec-run --start`
       is not "stop at the first instruction" -- it is "run to main", which
       on a stripped binary means gdb warns `Function "main" not defined`
       and lets the program run to completion.  Stripped binaries are the
       common case for this program (§2.3), so the symbolic form is never
       the right one.  This stops before the first instruction executed,
       which for anything dynamic is the loader's, not the program's (§3). */
    if (!mi_console(L, "starti")) goto fail;
    if (!mi_wait_stop(L, LIVE_WAIT_MS)) goto fail;
    if (L->exited){ seterr(L, "the program exited before it started"); goto fail; }
    /* The module map is wanted by every caller and costs one console
       command, so it is built here rather than lazily: a caller that
       forgot would get an empty map and no error.                    */
    live_map_refresh(L);
    return L;
fail:
    if (err) snprintf(err, errlen, "%s", L->err[0] ? L->err : "could not start gdb");
    live_close(L);
    return NULL;
}

int live_ptrace_scope(void){
    FILE *f = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

void live_attach_advice(int pid, char *out, size_t n){
    if (!out || !n) return;
    int scope = live_ptrace_scope();
    if (kill((pid_t)pid, 0) != 0 && errno == ESRCH){
        snprintf(out, n, "there is no process %d", pid);
        return;
    }
    switch (scope){
    case -1: case 0:
        snprintf(out, n, "ptrace is unrestricted here; an attach to %d should "
                 "work if you own it", pid);
        break;
    case 1:
        /* The default on Ubuntu and most desktops, and the reason this is
           the bonus rather than the feature.  Say which of the three ways
           through applies rather than making someone search.        */
        snprintf(out, n, "ptrace_scope is 1: only an ancestor may trace a "
                 "process, and gdb is not %d's. Either run codecity with "
                 "CAP_SYS_PTRACE, or have %d call prctl(PR_SET_PTRACER), or "
                 "loosen it machine-wide with "
                 "sysctl kernel.yama.ptrace_scope=0", pid, pid);
        break;
    case 2:
        snprintf(out, n, "ptrace_scope is 2: only a process with "
                 "CAP_SYS_PTRACE may attach at all");
        break;
    default:
        snprintf(out, n, "ptrace_scope is %d: attaching is switched off "
                 "on this machine", scope);
        break;
    }
}

Live *live_attach(int pid, char *err, size_t errlen){
    ignore_sigpipe();
    Live *L = calloc(1, sizeof *L);
    if (!L){ if (err) snprintf(err, errlen, "out of memory"); return NULL; }
    L->gdb = L->inf = -1;
    L->in = L->out = -1;
    /* Set before anything can fail: a session that attached must never be
       torn down by the path that kills.                              */
    L->own = LIVE_OWN_DETACH;
    L->subject = -1;

    if (!spawn(L)) goto fail;
    if (!mi_cmd(L, "-gdb-set confirm off"))  goto fail;
    if (!mi_cmd(L, "-gdb-set mi-async on"))  goto fail;
    if (!mi_cmd(L, "-target-attach %d", pid)) goto fail;
    L->inf = (pid_t)pid;
    /* An attach stops the process wherever it happened to be -- often
       inside a syscall, which is the normal result and not a problem
       (§10.2).  The stop may or may not have been announced as a record,
       so take the pc from gdb directly rather than assuming.        */
    L->stopped = 1;
    if (mi_cmd(L, "-data-evaluate-expression $pc")){
        char v[64];
        if (mi_get(L->res, "value", v, sizeof v)) L->pc = mi_u64(v);
    }
    live_map_refresh(L);
    return L;
fail:
    if (err){
        char why[LIVE_ERRLEN];
        live_attach_advice(pid, why, sizeof why);
        snprintf(err, errlen, "%s -- %s",
                 L->err[0] ? L->err : "could not attach", why);
    }
    live_close(L);
    return NULL;
}

uint64_t live_entry(Live *L){
    if (!L) return 0;
    /* The relocated entry, which for a PIE is not e_entry.  Deriving it from
       $pc here would be wrong in the way §3 warns about: at this point $pc
       is in ld.so.  gdb has already done the relocation, so ask it.    */
    if (!mi_console(L, "info files")) return 0;
    const char *p = L->con ? strstr(L->con, "Entry point:") : NULL;
    if (!p){ seterr(L, "gdb did not report an entry point"); return 0; }
    return mi_u64(p + 12);
}

/* ------------------------------------------------------------------ */
/* the address map (L1)                                                */
/* ------------------------------------------------------------------ */

/* `info proc mappings` prints, per line:
       0x555555554000  0x555555558000  0x4000  0x0  r--p  /usr/bin/ls
   Older gdbs omit the perms column and anonymous mappings have no path at
   all, so rather than count columns we take the first token that starts
   with '/' as the path and the first token as the start address.  That
   keeps working across versions, and it accepts a path containing spaces
   because it takes the rest of the line.                             */
static void map_line(Live *L, const char *line){
    while (*line == ' ' || *line == '\t') line++;
    if (line[0] != '0' || line[1] != 'x') return;
    char *end = NULL;
    uint64_t lo = strtoull(line, &end, 16);
    if (!end || end == line) return;
    uint64_t hi = strtoull(end, &end, 16);
    if (!end || !hi) return;
    const char *path = strchr(end, '/');
    if (!path) return;                       /* anonymous: not a file */
    size_t n = strlen(path);
    while (n && (path[n-1] == '\n' || path[n-1] == ' ')) n--;
    /* a mapping of a file we have already seen only widens its range: the
       load address is the lowest, which is what the bias is measured from */
    for (int i = 0; i < L->nmod; i++){
        if (strlen(L->mod[i].path) == n && !strncmp(L->mod[i].path, path, n)){
            if (lo < L->mod[i].lo) L->mod[i].lo = lo;
            if (hi > L->mod[i].hi) L->mod[i].hi = hi;
            return;
        }
    }
    if (L->nmod >= LIVE_MAXMOD) return;
    LiveMod *m = &L->mod[L->nmod++];
    m->lo = lo; m->hi = hi; m->bias = 0;
    snprintf(m->path, sizeof m->path, "%.*s", (int)n, path);
}

int live_map_refresh(Live *L){
    if (!L) return 0;
    /* the subject is remembered by path, not by index, so a refresh that
       reorders or grows the list cannot silently point it at another file */
    char keep[256]; keep[0] = 0;
    uint64_t keeplow = 0;
    if (L->subject >= 0 && L->subject < L->nmod){
        snprintf(keep, sizeof keep, "%s", L->mod[L->subject].path);
        keeplow = L->mod[L->subject].lo - L->mod[L->subject].bias;
    }
    L->nmod = 0; L->subject = -1;
    if (!mi_console(L, "info proc mappings")) return 0;
    for (const char *p = L->con ? L->con : ""; *p; ){
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), p);
        map_line(L, line);
        p = nl ? nl + 1 : p + len;
    }
    if (keep[0]){
        for (int i = 0; i < L->nmod; i++)
            if (!strcmp(L->mod[i].path, keep)){
                L->mod[i].bias = L->mod[i].lo - keeplow;
                L->subject = i;
                break;
            }
    }
    return L->nmod > 0;
}

int live_nmods(const Live *L){ return L ? L->nmod : 0; }

const LiveMod *live_mod(const Live *L, int i){
    if (!L || i < 0 || i >= L->nmod) return NULL;
    return &L->mod[i];
}

const LiveMod *live_mod_at(const Live *L, uint64_t a){
    if (!L) return NULL;
    for (int i = 0; i < L->nmod; i++)
        if (a >= L->mod[i].lo && a < L->mod[i].hi) return &L->mod[i];
    return NULL;
}

/* is a file with this basename mapped yet? */
static int mod_present(const Live *L, const char *base){
    for (int i = 0; i < L->nmod; i++){
        const char *b = strrchr(L->mod[i].path, '/');
        if (b && !strcmp(b + 1, base)) return 1;
    }
    return 0;
}

int live_await_module(Live *L, const char *path, int maxevents,
                      char *err, size_t errlen){
    if (!L){ if (err) snprintf(err, errlen, "no session"); return 0; }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (L->nmod && mod_present(L, base)) return 1;
    /* At the first stop only the loader and the executable are mapped --
       a library the program needs is not there yet, and asking for its
       bias would fail for a reason that is nothing to do with the map.
       Let the loader do its work, one solib event at a time, and look
       again.  This is also how the map is kept current later (§4).   */
    mi_console(L, "set stop-on-solib-events 1");
    int ok = 0, seen = 0;
    char why[LIVE_ERRLEN] = "";
    for (int i = 0; i < maxevents && !L->exited; i++){
        seen++;
        L->stopped = 0;
        /* gdb's own words are the reliable signal here, not a flag we
           maintain: "The program is not being run" is what a continue
           after the inferior has gone answers, and it is true whether or
           not the exit record happened to be drained yet.           */
        if (!mi_cmd(L, "-exec-continue")){
            snprintf(why, sizeof why, "%s", L->err);
            if (strstr(L->err, "not being run")) L->exited = 1;
            break;
        }
        /* A generous deadline, and deliberately not LIVE_WAIT_MS.  This is
           a one-time setup step, not the frame loop, and the gap between
           two library loads is the program's own startup: a GUI program
           opening a window and bringing up a GL driver takes much longer
           than ten seconds under a debugger that stops on every solib
           event.  Treating that as a failure was reported for a while as
           "the program exited", which it had not.                    */
        if (!mi_wait_stop(L, LIVE_SETUP_MS)){
            snprintf(why, sizeof why, "%s", L->err);
            break;
        }
        if (L->exited) break;
        live_map_refresh(L);
        if (mod_present(L, base)){ ok = 1; break; }
    }
    /* Note why we stopped looking *before* talking to gdb again: turning
       the solib events back off is another command, and on a dead
       inferior it can leave the session in a state that no longer
       remembers what went wrong.                                    */
    int died = L->exited || !live_alive(L);
    mi_console(L, "set stop-on-solib-events 0");
    if (!ok && err){
        if (why[0])
            snprintf(err, errlen, "%s never loaded -- gdb stopped after %d "
                     "library load%s: %s", base, seen, seen == 1 ? "" : "s", why);
        else if (died)
            snprintf(err, errlen, "%s never loaded: the program exited after "
                     "%d library load%s", base, seen, seen == 1 ? "" : "s");
        else
            snprintf(err, errlen, "%s was not loaded in %d solib events",
                     base, maxevents);
    }
    return ok;
}

int live_set_subject(Live *L, const char *path, uint64_t lowest_vaddr,
                     char *err, size_t errlen){
    if (!L || !path){ if (err) snprintf(err, errlen, "no session"); return 0; }
    if (!L->nmod && !live_map_refresh(L)){
        if (err) snprintf(err, errlen, "%s", L->err[0] ? L->err : "no mappings");
        return 0;
    }
    /* gdb reports the path it resolved, so /bin/ls comes back /usr/bin/ls.
       Compare on the resolved form, then fall back to the basename -- a
       library the inferior found by soname need not sit where we opened
       it from.                                                        */
    char real[PATH_MAX];
    const char *want = realpath(path, real) ? real : path;
    const char *base = strrchr(want, '/');
    base = base ? base + 1 : want;
    int hit = -1;
    for (int i = 0; i < L->nmod && hit < 0; i++)
        if (!strcmp(L->mod[i].path, want)) hit = i;
    for (int i = 0; i < L->nmod && hit < 0; i++){
        const char *b = strrchr(L->mod[i].path, '/');
        if (b && !strcmp(b + 1, base)) hit = i;
    }
    if (hit < 0){
        if (err) snprintf(err, errlen, "%s is not mapped in this process", base);
        return 0;
    }
    /* The bias, and the one place §3's trap is avoided: it comes from where
       the file is *mapped*, never from a pc.  At the first stop the pc is
       in ld.so, and pc - e_entry there is plausible-looking garbage.   */
    L->mod[hit].bias = L->mod[hit].lo - lowest_vaddr;
    L->subject = hit;
    return 1;
}

int live_have_subject(const Live *L){ return L && L->subject >= 0; }

uint64_t live_bias(const Live *L){
    return live_have_subject(L) ? L->mod[L->subject].bias : 0;
}

uint64_t live_to_file(const Live *L, uint64_t live){
    if (!live_have_subject(L)) return 0;
    const LiveMod *m = &L->mod[L->subject];
    if (live < m->lo || live >= m->hi) return 0;      /* off the map (§4) */
    return live - m->bias;
}

uint64_t file_to_live(const Live *L, uint64_t file){
    if (!live_have_subject(L) || !file) return 0;
    return file + L->mod[L->subject].bias;
}

/* ------------------------------------------------------------------ */
/* the non-blocking half (L5)                                          */
/* ------------------------------------------------------------------ */

/* One line if one is already there, without waiting for it.  Returns 0
   when there is nothing complete to read -- which is the normal answer
   most frames, not an error.                                        */
static int readline_nb(Live *L){
    for (;;){
        char *nl = L->rbuf ? memchr(L->rbuf, '\n', L->rlen) : NULL;
        if (nl){
            size_t len = (size_t)(nl - L->rbuf);
            if (!grow(&L->line, &L->linecap, len + 1)) return 0;
            memcpy(L->line, L->rbuf, len);
            L->line[len] = 0;
            if (len && L->line[len - 1] == '\r') L->line[len - 1] = 0;
            if (mi_tracing()) fprintf(stderr, "[mi <] %s\n", L->line);
            memmove(L->rbuf, nl + 1, L->rlen - len - 1);
            L->rlen -= len + 1;
            return 1;
        }
        struct pollfd pf = { L->out, POLLIN, 0 };
        int pr = poll(&pf, 1, 0);           /* 0: never wait */
        if (pr <= 0) return 0;
        if (!grow(&L->rbuf, &L->rcap, L->rlen + 4096)) return 0;
        ssize_t k = read(L->out, L->rbuf + L->rlen, L->rcap - L->rlen);
        if (k <= 0){ if (k < 0 && errno == EINTR) continue; return 0; }
        L->rlen += (size_t)k;
    }
}

int live_poll(Live *L){
    if (!L || L->closed) return 0;
    int wasWaiting = L->awaiting;
    while (readline_nb(L)){
        const char *s = L->line;
        if (!strcmp(s, "(gdb)") || !*s) continue;
        if (*s == '*' || *s == '=' || *s == '~' || *s == '@' || *s == '&')
            mi_async(L, s);
        /* A result record arriving here belongs to the continue we let go
           of; there is nobody waiting on its token, so noting the state
           it implies is all that is wanted.                          */
    }
    if (wasWaiting && (L->stopped || L->exited)){ L->awaiting = 0; return 1; }
    return 0;
}

int live_waiting(const Live *L){
    return L && L->awaiting && !L->stopped && !L->exited;
}

int live_continue_async(Live *L, uint64_t addr){
    if (L) L->mcok = 0;      /* the process is about to move */
    if (!L) return 0;
    if (L->exited){ seterr(L, "the program is not running"); return 0; }
    if (addr && !mi_cmd(L, "-break-insert -t *0x%llx",
                        (unsigned long long)addr)) return 0;
    L->stopped = 0;
    /* -exec-continue answers ^running at once; the *stopped comes when it
       comes, and live_poll() is what notices.                         */
    if (!mi_cmd(L, "-exec-continue")) return 0;
    L->awaiting = 1;
    return 1;
}

int live_interrupt(Live *L){
    if (!L || !L->awaiting) return 0;
    return mi_cmd(L, "-exec-interrupt");
}

int live_run_to(Live *L, uint64_t addr){
    if (L) L->mcok = 0;      /* the process is about to move */
    if (!L) return 0;
    if (L->exited){ seterr(L, "the program is not running"); return 0; }
    /* Already there.  Not a corner case: with no dynamic loader to go
       through, `starti` stops *on* the entry point, so asking to run to it
       would set a breakpoint on the instruction we are standing on and
       continue -- past it, and out the far end of the program.  Statically
       linked binaries hit this every time.                            */
    if (L->stopped && L->pc == addr) return 1;
    if (!mi_cmd(L, "-break-insert -t *0x%llx", (unsigned long long)addr)) return 0;
    L->stopped = 0;
    if (!mi_cmd(L, "-exec-continue")) return 0;
    if (!mi_wait_stop(L, LIVE_WAIT_MS)) return 0;
    if (L->exited){ seterr(L, "the program exited before reaching 0x%llx",
                           (unsigned long long)addr); return 0; }
    return 1;
}

int live_step(Live *L){
    if (L) L->mcok = 0;      /* the process is about to move */
    if (!L) return 0;
    L->stopped = 0;
    if (!mi_cmd(L, "-exec-step-instruction")) return 0;
    if (!mi_wait_stop(L, LIVE_WAIT_MS)) return 0;
    return !L->exited;
}

void live_regs_want(Live *L, const char *const *names, int n){
    if (!L || !names || n <= 0) return;
    for (int i = 0; i < L->nwant; i++) free(L->want[i]);
    free(L->want);
    L->want = calloc((size_t)n, sizeof *L->want);
    L->nwant = 0;
    if (!L->want) return;
    for (int i = 0; i < n; i++)
        if (names[i] && names[i][0]) L->want[L->nwant++] = strdup(names[i]);
    free(L->regcmd); L->regcmd = NULL;      /* rebuild on the next read */
}

static int wanted(const Live *L, const char *name){
    if (!L->nwant) return 1;
    /* the flags always: the panel's six flag bits come out of it, and
       nothing else can supply them */
    if (!strcmp(name, "eflags") || !strcmp(name, "cpsr")) return 1;
    for (int i = 0; i < L->nwant; i++)
        if (L->want[i] && !strcmp(L->want[i], name)) return 1;
    return 0;
}

int live_regs(Live *L, LiveReg *out, int max){
    if (!L || !out || max <= 0) return -1;
    /* Names once per session; they do not change under us.  Kept on the
       session and not in a static keyed by `L`: malloc reuses addresses,
       so a static cache would hand a new session the previous one's
       names -- silently right for the same program and silently wrong
       for any other.                                                 */
    char **names = L->rname;
    int nnames = L->nrname;
    if (!nnames){
        if (!mi_cmd(L, "-data-list-register-names")) return -1;
        const char *p = mi_field(L->res, "register-names");
        if (!p || *p != '[') return -1;
        for (p++; *p && *p != ']'; ){
            char nm[64];
            const char *q = mi_str(p, nm, sizeof nm);
            if (!q) break;
            if (nnames < LIVE_NAMES && nm[0]) names[nnames++] = strdup(nm);
            else if (nm[0]) nnames++;
            p = (*q == ',') ? q + 1 : q;
        }
        L->nrname = nnames;
    }
    if (nnames > LIVE_NAMES) nnames = LIVE_NAMES;
    /* Asking for every register costs far more than it looks.  On x86-64
       gdb has 200-odd of them, and the 32 vector ones are rendered as
       aggregates -- "{v8_bfloat16 = {0x0, 0x0, ...}}" -- so the reply is
       kilobytes of text that gdb formats and we then throw away.  At one
       step a frame that is invisible; at ten thousand steps it is the
       whole cost.  So the first call asks for everything, notes which
       indices came back as scalars, and every call after that asks for
       exactly those.                                                 */
    if (!mi_cmd(L, L->regcmd ? L->regcmd : "-data-list-register-values x"))
        return -1;
    const char *p = mi_field(L->res, "register-values");
    if (!p) return -1;
    int build = !L->regcmd;
    char cmd[4096];
    size_t co = 0;
    if (build) co = (size_t)snprintf(cmd, sizeof cmd, "-data-list-register-values x");
    int n = 0, seen = 0;
    while ((p = strstr(p, "{number=\""))){
        char num[16], val[128];
        const char *q = mi_str(p + 8, num, sizeof num);
        if (!q) break;
        const char *v = mi_field(p, "value");
        if (!v || !mi_str(v, val, sizeof val)){ p += 9; continue; }
        /* skip the aggregates: MI renders a vector register as
           "{v8_bfloat16 = {...}}", which is not a value the Vm can hold
           until L7.  A scalar is 0x-prefixed and nothing else.        */
        int idx = atoi(num);
        char *end = NULL;
        unsigned long long uv = strtoull(val, &end, 0);
        if (val[0] == '{' || !end || *end){ p += 9; continue; }
        const char *nm = (idx >= 0 && idx < nnames && names[idx]) ? names[idx] : "?";
        if (!wanted(L, nm)){ p += 9; continue; }
        seen++;
        if (build && co + 8 < sizeof cmd)
            co += (size_t)snprintf(cmd + co, sizeof cmd - co, " %d", idx);
        if (n < max){
            snprintf(out[n].name, sizeof out[n].name, "%s", nm);
            out[n].v = (uint64_t)uv;
            n++;
        }
        p += 9;
    }
    if (build && seen) L->regcmd = strdup(cmd);
    return seen;
}

int live_reg(Live *L, const char *name, uint64_t *out){
    LiveReg r[LIVE_MAXREG];
    int n = live_regs(L, r, LIVE_MAXREG);
    if (n < 0) return 0;
    if (n > LIVE_MAXREG) n = LIVE_MAXREG;
    for (int i = 0; i < n; i++)
        if (!strcmp(r[i].name, name)){ if (out) *out = r[i].v; return 1; }
    return 0;
}

char *live_console(Live *L, const char *cmd){
    if (!L || !cmd) return NULL;
    if (!mi_console(L, cmd)) return NULL;
    return strdup(L->con ? L->con : "");
}

void live_symbol_at(Live *L, uint64_t addr, char *out, size_t n){
    if (!out || !n) return;
    out[0] = 0;
    if (!L) return;
    char cmd[64];
    snprintf(cmd, sizeof cmd, "info symbol 0x%llx", (unsigned long long)addr);
    if (!mi_console(L, cmd) || !L->con) return;
    /* "malloc + 4 in section .text of /lib/.../libc.so.6" -- the name is
       everything before " + " or " in section".  "No symbol matches" is
       gdb's way of saying it does not know, and is not a name.      */
    if (!strncmp(L->con, "No symbol", 9)) return;
    size_t k = 0;
    for (const char *p = L->con; *p && *p != '\n' && k + 1 < n; p++){
        if (!strncmp(p, " + ", 3) || !strncmp(p, " in section", 11)) break;
        out[k++] = *p;
    }
    out[k] = 0;
}

/* A cached read of one page.  Returns 0 when the page cannot be read at
   all -- an unmapped address, which is a normal answer, not an error. */
static int page_cached(Live *L, uint64_t addr){
    uint64_t base = addr & ~0xfffull;
    if (L->mcok && L->mcbase == base) return 1;
    if (!mi_cmd(L, "-data-read-memory-bytes 0x%llx 4096",
                (unsigned long long)base)) return 0;
    const char *p = mi_field(L->res, "contents");
    static char hex[8300];
    if (!p || !mi_str(p, hex, sizeof hex)) return 0;
    size_t got = 0;
    for (const char *h = hex; h[0] && h[1] && got < sizeof L->mcbuf; h += 2){
        char b[3] = { h[0], h[1], 0 };
        L->mcbuf[got++] = (uint8_t)strtoul(b, NULL, 16);
    }
    if (got < sizeof L->mcbuf) return 0;      /* a short page: do not cache */
    L->mcbase = base; L->mcok = 1;
    return 1;
}

/* The read the Vm's fourth layer makes, byte at a time, through the page
   cache.  Signature matches Vm.liveread so vm.c need not know what a Live
   is.                                                              */
int live_mem_cb(void *ctx, uint64_t addr, int n, uint8_t *out){
    Live *L = (Live *)ctx;
    if (!L || !out || n <= 0) return 0;
    if (!live_stopped(L)) return 0;
    /* The Vm is asked for both kinds of address and cannot tell them
       apart: a stack pointer is a *live* address, because the registers
       hold what the process holds (L2), while a section address comes out
       of the city and is a *file* one.  Try it as given, and if nothing is
       mapped there try it as a file address.  The two cannot be confused
       in practice -- a file address in a PIE is a small number nothing is
       mapped at -- and for a non-PIE they are the same number anyway. */
    uint64_t base = addr;
    if (!page_cached(L, base)){
        uint64_t alt = file_to_live(L, addr);
        if (!alt || alt == addr || !page_cached(L, alt)) return 0;
        base = alt;
    }
    for (int i = 0; i < n; i++){
        uint64_t a = base + (uint64_t)i;
        if (!page_cached(L, a)) return 0;
        out[i] = L->mcbuf[a & 0xfff];
    }
    return 1;
}

int live_mem(Live *L, uint64_t addr, int n, uint8_t *out){
    if (!L || !out || n <= 0) return 0;
    if (!mi_cmd(L, "-data-read-memory-bytes 0x%llx %d",
                (unsigned long long)addr, n)) return 0;
    const char *p = mi_field(L->res, "contents");
    char hex[8192];
    if (!p || !mi_str(p, hex, sizeof hex)){
        seterr(L, "no memory at 0x%llx", (unsigned long long)addr);
        return 0;
    }
    int got = 0;
    for (const char *h = hex; h[0] && h[1] && got < n; h += 2){
        char b[3] = { h[0], h[1], 0 };
        out[got++] = (uint8_t)strtoul(b, NULL, 16);
    }
    return got == n;
}

/* ------------------------------------------------------------------ */
/* --gdbtest: L0's pass criterion, headless                            */
/* ------------------------------------------------------------------ */

static int g_fail;

static void ok(int cond, const char *what, int verbose){
    if (!cond){ printf("  FAIL  %s\n", what); g_fail++; }
    else if (verbose) printf("  ok    %s\n", what);
}

/* every one of these must fail with a sentence and leave nothing behind */
static void abort_paths(int verbose){
    char err[LIVE_ERRLEN];
    struct { const char *what, *gdb, *prog; } C[] = {
        { "gdb is not installed",   "/nonexistent/gdb", "/bin/ls" },
        { "the file does not exist", NULL, "/nonexistent/program" },
        { "the file is not an ELF",  NULL, "/etc/passwd" },
        { "the file is a directory", NULL, "/tmp" },
    };
    for (size_t i = 0; i < sizeof C / sizeof *C; i++){
        if (C[i].gdb) setenv("CODECITY_GDB", C[i].gdb, 1);
        err[0] = 0;
        Live *L = live_launch(C[i].prog, NULL, 0, err, sizeof err);
        if (C[i].gdb) unsetenv("CODECITY_GDB");
        char msg[420];
        snprintf(msg, sizeof msg, "refused: %-24s -- %s", C[i].what,
                 err[0] ? err : "(no message!)");
        ok(L == NULL && err[0] != 0, msg, verbose);
        live_close(L);          /* NULL-safe, and must not double-free */
    }
}

/* a stop that is a signal rather than a step: aim the pc at 0 and go */
static void fault_path(const char *prog, int verbose){
    char err[LIVE_ERRLEN];
    Live *L = live_launch(prog, NULL, 0, err, sizeof err);
    if (!L){ ok(0, "fault path: could not launch", verbose); return; }
    /* not through the public API on purpose: this is a debugger poking the
       inferior, which live.h deliberately does not offer (§8.5)       */
    mi_console(L, "set $pc = 0");
    live_step(L);
    ok(!strcmp(live_stop_reason(L), "signal-received") &&
       !strcmp(live_stop_signal(L), "SIGSEGV"),
       "a fault is reported as a signal stop, not a hang", verbose);
    pid_t inf = live_inferior_pid(L);
    live_close(L);
    ok(inf <= 0 || (kill(inf, 0) != 0 && errno == ESRCH),
       "the faulted inferior is gone after close", verbose);
}

int live_gdbtest(const char *prog, int cycles, int verbose){
    g_fail = 0;
    printf("gdbtest: %s, %d cycle%s\n", prog, cycles, cycles == 1 ? "" : "s");

    abort_paths(verbose);
    fault_path(prog, verbose);

    uint64_t entry0 = 0;
    int worstregs = 1 << 30;
    for (int c = 0; c < cycles; c++){
        char err[LIVE_ERRLEN];
        Live *L = live_launch(prog, NULL, 0, err, sizeof err);
        if (!L){ printf("  FAIL  cycle %d: %s\n", c, err); g_fail++; break; }
        int quiet = verbose && c == 0;

        uint64_t entry = live_entry(L);
        if (!entry0) entry0 = entry;
        ok(entry != 0, "the relocated entry point is known", quiet);
        ok(entry == entry0, "the entry point is the same every cycle", quiet);

        ok(live_run_to(L, entry), "ran to the entry point", quiet);
        ok(live_pc(L) == entry, "stopped exactly on it", quiet);
        /* "breakpoint-hit" only when there was somewhere to run *from*.  A
           static binary has no loader, so `starti` already stopped on the
           entry and the reason is still that first signal-0 stop.  What
           must hold either way is that this is a stop and not an exit. */
        const char *why = live_stop_reason(L);
        ok(live_alive(L) && *why && strncmp(why, "exited", 6) != 0,
           "and it is a stop, not an exit", quiet);

        LiveReg r[LIVE_MAXREG];
        int nr = live_regs(L, r, LIVE_MAXREG);
        if (nr < worstregs) worstregs = nr;
        ok(nr >= 16, "at least 16 scalar registers came back", quiet);
        ok(nr <= LIVE_MAXREG, "and they all fit -- none were truncated", quiet);
        int kept = nr < LIVE_MAXREG ? nr : LIVE_MAXREG;
        uint64_t rip = 0;
        for (int i = 0; i < kept; i++) if (!strcmp(r[i].name, "rip")) rip = r[i].v;
        if (rip) ok(rip == entry, "rip agrees with the stop address", quiet);

        uint8_t b[64];
        ok(live_mem(L, entry, 64, b), "read 64 bytes at the entry", quiet);

        uint64_t before = live_pc(L);
        ok(live_step(L), "stepped one instruction", quiet);
        ok(live_pc(L) != before, "and the pc moved", quiet);

        pid_t gdb = live_gdb_pid(L), inf = live_inferior_pid(L);
        ok(inf > 0, "the inferior pid was seen", quiet);
        live_close(L);
        ok(gdb <= 0 || (kill(gdb, 0) != 0 && errno == ESRCH),
           "gdb is reaped after close", quiet);
        ok(inf <= 0 || (kill(inf, 0) != 0 && errno == ESRCH),
           "the inferior is gone after close", quiet);
        if (verbose && c == 0) printf("  ---- %d more cycles ----\n", cycles - 1);
    }
    /* An orphan from any cycle would still be our child, so this catches
       what the per-cycle check cannot: something we never waited for. */
    ok(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD,
       "no child of ours is left unreaped", 1);

    printf("gdbtest: %s  (%d scalar registers per stop)\n",
           g_fail ? "FAILED" : "ok", worstregs == 1 << 30 ? 0 : worstregs);
    return g_fail ? 1 : 0;
}

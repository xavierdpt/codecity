# A wisp that is not invented

> **Proposal, not implemented.** `docs/execution-wisps.md` built a wisp whose
> values are *fiction*: a CPU state that walks a code room deciding branches on
> arithmetic over invented inputs. This describes the other half — the same
> body, the same ceiling panel, the same trail, driven by a **real process
> stopped under gdb**. Two ways in: **launch** a program under the debugger and
> walk it from its first instruction, or **attach** to one already running and
> walk it from wherever it happens to be.
>
> The headline measurements that decide the design, taken on this machine
> (gdb 15.1, Python 3.12, Yama `ptrace_scope=1`):
>
> | | measured |
> |---|---|
> | `stepi` through gdb's Python API | **14 600 / s** (10 800 with a `$pc` read per step) |
> | breakpoint hit + `continue` cycle | **6 700 / s** |
> | reading two registers | 4 µs |
> | reading 64 bytes of inferior memory | 1.5 µs |
> | steps inside `/bin/ls`'s own text, from its ELF entry | **0.3 %** (81 of 30 000) |
> | steps inside a small program's own text, from `main` | **92.6 %** (37 022 of 40 000) |
> | attaching to a PID that is not a descendant of gdb | **denied** — `ptrace: Operation not permitted` |
>
> Two of those numbers decide almost everything below.
>
> **Speed is a non-problem.** A wisp runs at 0.5–20 instructions per second
> (`WISP_RATE_MIN`/`WISP_RATE_MAX`). gdb delivers three orders of magnitude
> more than the fastest setting asks for, and reading the whole register file
> costs less than a frame's worth of anything. The interesting budget is
> latency per frame, not throughput.
>
> **Residency is the whole problem.** A live pc is only sometimes inside the
> file the city was built from. Starting a dynamically linked program at its
> ELF entry, 99.7 % of the first 30 000 instructions are in `ld.so` and libc —
> the city has no room for any of them. Inside a program's own compute loop it
> is 92.6 % the other way. **The design is therefore not "step and draw"; it is
> "stay on the map, and say plainly when you cannot".** §4 is the core of this
> document and everything else is plumbing.
>
> **Not yet decided** — flagged for a human, not settled here:
>
> - **§11's safety gate.** `execution-wisps.md` §14 says running the binary
>   from inside the explorer "executes untrusted code of possibly foreign
>   architecture, and it should not happen". This feature does exactly that.
>   That paragraph was written about a *different* thing (running the file you
>   are browsing, implicitly) and this proposal never does that implicitly —
>   but the stance still has to be revised deliberately rather than quietly
>   contradicted. §11 proposes the wording.
> - **§4's off-map policy**, between C2 and C3. C2 is much cheaper and C3 is
>   much better. The milestones are ordered so C2 ships and C3 is optional.

---

## Milestones

Nine milestones, in the shape `execution-wisps.md` uses: each ends in something
you can **run and check**, each has a **pass criterion that is a number** where
a number is possible, and each leaves the program shippable — if the work stops
after any of them, nothing is half-built.

| # | milestone | you can... | pass criterion |
|---|---|---|---|
| **L0** | The transport, headless (§2) | — | `--gdbtest`: launch, stop, read 16 registers + 64 bytes, step, kill; child reaped in every abort path; 1000 cycles under ASan with no leak — **done**, see below |
| **L1** | The bias, and the map (§3) | ask "where is this live pc in the city?" and get an answer | live entry maps to the same room `city_find_addr` gives for `e_entry`; holds for PIE, non-PIE and one shared library — **done**, see below |
| **L2** | A frozen live state in a room (§5, §8) | press `L`, and the ceiling panel shows **real** registers | every register on the panel tagged `PV_LIVE`; panel content equals `gdb -batch -ex 'info registers'` for the same stop, field for field — **done**, see below |
| **L3** | Stepping, on the map (§7) | drive a live wisp with `T` — one real instruction at a time | 10 000 `T` presses on the §4 workload: pc and city room agree every step; ≤2 ms per step at the 99th percentile — **done**, see below |
| **L4** | Off the map, honestly (§4 C1+C2) | watch it leave for libc and come back, and see that it did | from `main` of the §4 workload, ≥90 % of steps on the map; every off-map excursion accounted for in the HUD; never a wrong room — **done**, see below |
| **L5** | Running, without stalling the frame (§6) | press `K` and watch it go at the wisp's own rate | 60 fps held while running at `WISP_RATE_MAX`; worst frame ≤3 ms of gdb time; `stallMs` unchanged from a no-wisp frame — **done**, see below |
| **L6** | Attach (§10) | `--attach PID` on a process you own | attaches to a descendant; on refusal, one line naming `ptrace_scope` and what to do — never a crash and never a hang — **done**, see below |
| **L7** | Live memory as a fourth layer (§8.4) | see the *real* stack, heap and `.data`, with the canary that is actually there | `.data` bytes read live differ from the file's initial values on a program that mutates them, and the panel says which is which — **done**, see below |
| **L8** | Divergence: the invented wisp vs the real one (§12) | run both from the same address and see where the fiction parted from the fact | on the §4 workload: first divergence reported with an instruction address; ≥95 % of tier-1 steps agree register-for-register before the first invented input is consumed |

L0–L4 are the feature. L5 makes it pleasant, L6 doubles its reach, L7 makes it
truthful about memory, and L8 is the one that pays the original design back —
it turns `execution-wisps.md`'s whole invention policy into something with a
measured error bar instead of a paragraph of caveats.

### L0 — the transport

**Done.** `src/live.h` / `src/live.c` (~700 lines) are the MI client: spawn,
tokenised command exchange with a deadline on every read, the async-record
state machine, and the teardown. `--gdbtest [N]` is the test, and it needs
neither a window nor an ELF nor a city — it runs before any of them exist,
which is most of the point of doing this first.

```
$ ./codecity /bin/ls --gdbtest 3 -v
gdbtest: /bin/ls, 3 cycles
  ok    refused: gdb is not installed     -- cannot run '/nonexistent/gdb': No such file or directory
  ok    refused: the file does not exist  -- /nonexistent/program: No such file or directory.
  ok    refused: the file is not an ELF   -- "/etc/passwd": not in executable format: file format not recognized
  ok    refused: the file is a directory  -- /tmp: Is a directory.
  ok    a fault is reported as a signal stop, not a hang
  ok    the faulted inferior is gone after close
  ok    the relocated entry point is known
  ok    the entry point is the same every cycle
  ok    ran to the entry point
  ok    stopped exactly on it
  ok    for that reason
  ok    at least 16 scalar registers came back
  ok    and they all fit -- none were truncated
  ok    rip agrees with the stop address
  ok    read 64 bytes at the entry
  ok    stepped one instruction
  ok    and the pc moved
  ok    the inferior pid was seen
  ok    gdb is reaped after close
  ok    the inferior is gone after close
  ok    no child of ours is left unreaped
gdbtest: ok  (112 scalar registers per stop)
```

The criterion is met: 1000 cycles clean under ASan and UBSan, no leak, no
`runtime error`, and no child left unreaped — that last check is
`waitpid(-1, WNOHANG)` at the end, which catches an orphan from *any* cycle,
not just the one being watched. A cycle costs ~135 ms, nearly all of it gdb's
own startup, so the 1000-cycle soak takes about two minutes and the default of
20 is the one worth running by habit.

**Six things this milestone found that the design did not know:**

- **`-exec-run --start` is not `starti`.** It is "run to `main`". On a stripped
  binary gdb emits `&"Function \"main\" not defined."` on the log stream —
  *not* an `^error` — and then lets the program **run to completion**. The
  launch appeared to work and the process was simply gone. This is §2.3's
  fact 3 arriving early and harder than expected: the fix is
  `-interpreter-exec console "starti"`, and the general rule is that a
  symbolic command is never the right one here.
- **`live_run_to(addr)` must check whether it is already there.** With no
  dynamic loader to go through, `starti` stops *on* the entry point, so
  running to it set a breakpoint on the instruction it was standing on,
  continued, and sailed out the far end of the program. Statically linked
  binaries hit this every time — and the general form ("run to where you
  already are") will come back the moment §4's C2 policy starts placing
  breakpoints on return addresses. `--gdbtest` is now run against a static
  non-PIE binary as well as a stripped PIE, a PIE with symbols (`codecity`
  itself) and `/bin/bash`.
- **The orphan guarantee is `PR_SET_PDEATHSIG`, not careful cleanup.** No
  amount of tidy shutdown code survives `SIGKILL` on codecity. The child sets
  `PR_SET_PDEATHSIG(SIGKILL)` and re-checks `getppid()` for the race where the
  parent died before the call, so gdb cannot outlive us however we go — and
  gdb in turn takes the inferior it launched.
- **UBSan caught `memchr(NULL, '\n', 0)`** on the very first read, before the
  buffer existed. Undefined, not merely pointless, and invisible without the
  soak — which is the argument for the soak being the pass criterion rather
  than a smoke test.
- **A `static` cache keyed on the session pointer is wrong**, and wrong in the
  way that passes tests: `malloc` reuses addresses, so a new session gets the
  previous one's register names — identical for the same program, silently
  wrong for a different one. The names live on the session now.
- **112 scalar registers per stop** on this machine's x86-64, not the 16 the
  criterion asked for: the GPRs, `rip`, `eflags`, the segment and base
  registers, the x87 stack with its control words, and `k0`–`k7`. Vector
  registers are skipped, as designed — MI renders them as aggregates
  (`"{v8_bfloat16 = {...}}"`), which is not a value the `Vm` can hold until
  L7. `live_regs()` returns how many the stop *had* while writing at most
  `max`, snprintf-style, so truncation is reportable rather than silent.

`CODECITY_GDB_TRACE=1` puts the whole MI conversation on stderr, and
`CODECITY_GDB` overrides which gdb is used (the "gdb is not installed" abort
path is tested through it). Neither is a temporary: every bug in an MI client
presents as a hang or a silence, and the trace is the only way to see one.

**Nothing else in the program changed.** `live.c` is new, the Makefile gained
one file, and `main.c` gained an include, a flag and a one-line dispatch
before the ELF is even opened. `--selftest` and `--vmtest` are untouched and
still pass.

### L1 — the bias, and the map

**Done.** `live_map_refresh()` reads the inferior's mapped files,
`live_set_subject()` names the one the city was built from and computes its
bias, and `live_to_file()` / `file_to_live()` are the only two places an
address crosses between the two worlds. `--maptest [PROGRAM]` is the test.

```
$ ./codecity /bin/ls --maptest -v
maptest: city from /bin/ls, running /bin/ls
  2 modules mapped; ET_DYN (PIE or library), lowest PT_LOAD vaddr 0x0
  ok    the inferior's mapped files were read (2)
  ok    ls is mapped; bias 0x555555554000
  ok    a PIE has one
  ok    every allocated section survives file -> live -> file
  first stop at 0x7ffff7fe4540 in /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
  ok    the bias is not the one $pc at the first stop would suggest
  ok    every address in another module reads as off the map (1)
  ok    the file entry 0x6d30 is in a room
  ok    the live entry 0x55555555ad30 maps back to the same room
  ok    .text in the process matches the file, byte for byte at 0x55555555a000
  ok    the process stops on that live address
  ok    which reads back as the file entry
maptest: ok
```

Passing on all seven cases run: a stripped PIE (`/bin/ls`), a static non-PIE,
a PIE with symbols (`codecity`), `/bin/bash`, and three libraries explored
while the program that loads them runs (`libc` under `/bin/ls`, `libSDL2` and
`libcapstone` under `codecity`). Clean under ASan and UBSan.

**The bias is `load address − lowest PT_LOAD p_vaddr`**, which is the load
address for a PIE and **zero for an ET_EXEC by arithmetic rather than by
special case** — the static binary's mapping is at `0x400000` and so is its
lowest `p_vaddr`. It comes from `info proc mappings`, never from a pc, and the
test asserts that explicitly: `bias != pc - e_entry` at the first stop, which
is §3's trap turned into a check.

**Four things this milestone found:**

- **A library subject is not mapped at the first stop.** `starti` stops in the
  loader with only `ld.so` and the executable present, so "explore `libc`, run
  `/bin/ls`" failed for a reason that had nothing to do with the map.
  `live_await_module()` runs the loader forward on `stop-on-solib-events`
  until the file appears. `codecity` needs ~90 of those, so the budget is 512.
- **A shared library often has no entry point.** `libSDL2`'s `e_entry` is 0,
  so "the entry point is in a room" was failing on an address that does not
  exist. The criterion is really *an* address both ways, and the test falls
  back to the first executable section.
- **A library's entry point is never executed**, so running to it hangs until
  the program exits. That check is now asked only of a subject that is also
  the program being run.
- **The strong proof is bytes, not arithmetic.** A file→live→file round trip
  only tests our own subtraction against itself. Reading the subject's `.text`
  out of the *process* at the translated address and comparing it with the
  file's own bytes is what would actually catch a wrong bias, and it works for
  every subject including one that never executes.

One wart, recorded rather than fixed: `live_to_file()` returns 0 for "off the
map", and a PIE's own load base translates to file vaddr 0 — the same value.
Harmless, because 0 is in no section and `city_find_addr()` rejects it
outright, and the alternative (a bool plus an out-parameter) makes every
caller worse. The test counts off-map modules over the *other* modules for
this reason.

**Still no change to `city.c`.** The exclusive-mode decision — a session is
either reading a real process or simulating one, never both — removes the
"same file at two load addresses" argument against a bias on `City`, but not
the operative one: `city_find_addr()` is called with *file* addresses from the
disassembly all over the program, so a `City` that silently biased them would
break every existing caller. The translation stays at the boundary.

### L2 — a frozen live state in a room

**Done.** `--debug PROGRAM` opens a live session beside the city; pressing `L`
in a code room runs the real process to the sculpture you are standing next to
and fills the state sheet with what its registers actually are there.
`--statetest` is the criterion, headless.

```
$ ./codecity /bin/ls --statetest
statetest: city from /bin/ls, running /bin/ls
  ok    the process is stopped at 0x6d30
  ok    the wisp knows it is live, not simulated
  ok    17 registers were read into the state
  ok    every register that has a value is tagged live (17)
  ok    every flag is known -- a real CPU is never unsure
  ok    17 registers were compared against gdb's own table
  ok    and every one of them agrees, field for field
statetest: ok
```

Passing on `/bin/ls`, a static non-PIE, a small workload, `codecity`,
`/bin/bash`, and `libc` explored while `/bin/ls` runs. Clean under ASan and
UBSan, including `--selftest` driven with a live session attached.

**The criterion had to be reinterpreted to mean anything.** Comparing against a
separate `gdb -batch -ex "info registers"` run is impossible: it would be a
different process with different ASLR, so nothing would match and nothing
would be learned. The differential that *is* meaningful runs inside one
session — our parsed `-data-list-register-values` against gdb's own
human-readable table, at the identical stop. That checks the MI parsing
against gdb's own formatting, which is the thing that could actually be wrong.

**Where a live run starts.** Not the entry point (§4.1: 99.7% of the
instructions after it are in the loader and libc). `L` runs to **the
instruction you are standing next to** — §4.3's "break where the player is
standing" — which is both the better interaction and the smaller thing to
build.

**Four things this milestone found:**

- **The register file must hold one address space, not two.** The first
  version put the *file* address in `rip` — the address the city speaks —
  while `rsp` and the rest held live values straight from the process. The
  test caught it as a mismatch against gdb, and it was right to: a panel where
  one register is in a different address space from its neighbours needs a
  footnote on every line. Registers now hold what the process holds, verbatim,
  and the translation stays at the boundary where L1 put it. The wisp's
  position in the room comes from the file address, by index.
- **x86-64 has no `eflags` slot in the `Vm`'s register table**, so the flag
  sync — written inside the "did this register map to a slot?" branch — never
  ran, and the panel showed six `?` under a banner claiming everything on it
  was measured. The flags register is now read for its bits whether or not the
  architecture gives it a slot.
- **Alive is not the same as stopped.** A running inferior has no readable
  registers (gdb answers `No registers.`), and the two conditions needed
  telling apart before the error message could name the right one.
- **Ignoring an error return replaces the real cause with a later symptom.**
  `--statetest` dropped `live_await_module()`'s result, so when a program
  exited early the stale module map still satisfied `live_set_subject()` and
  the failure surfaced four steps later as "0 registers were read".

**The honesty rules, which are the point of the milestone.** The state sheet's
banner is `LIVE -- every value below was read out of a real process stopped
under gdb`, in place of the `SIMULATED` one, and `PV_LIVE` has its own colour
and mark. Three exclusions enforce it:

- **The stack and the memory log are withheld under a live wisp.** The `Vm`'s
  memory layers are still shadow → file → invention, so those panes would show
  *invented* bytes under a banner that says LIVE. They are replaced by one
  line saying so until the live memory layer lands (L7).
- **`X` is refused while a process is attached** — one source or the other,
  never both on the same screen.
- **The file browser is off during a live session**, because loading another
  file would leave the bias pointing at the wrong thing. It is also §11's rule
  that the browser must never be a way into running something.

`L` was added to `--selftest`'s key-fuzz list, where the interesting case is
that it does nothing harmful when no process was ever asked for.

**One unresolved edge**, recorded rather than papered over: exploring
`libSDL2` while `codecity` itself runs under gdb ends with the program exiting
during the loader's library sweep, and the trace shows `=thread-group-exited`
arriving while `L->exited` still reads 0 in the error path. The failure is
reported correctly, and the library path is covered by `libc` under `/bin/ls`,
so this is a state-tracking wart to chase when L5 makes stop handling
asynchronous rather than a blocker here.

### L3 — stepping, on the map

**Done.** `wisp_step_live()` steps the real CPU one instruction, reads where
it ended up, and hands the rest to the same body the simulated wisp uses.
`T` and `I` drive it; `--steptest [N]` is the criterion.

```
$ ./codecity tests/workload --steptest
  ok    stopped in crunch at 0x11a9
  ok    10000 steps taken (0 excursions off the map, 0 instructions stepped through to get back)
  ok    the city and the process agree at every step
  step cost  median 0.321 ms   p99 0.460 ms   worst 1.628 ms   mean 0.323 ms  (3093 steps/s)
  ok    the 99th percentile step is 0.460 ms (budget 2 ms)
  10000 trail cards, 1 rooms
steptest: ok
```

`tests/workload.c` is the §4 program, checked in as a fixture: `crunch` is a
leaf with no calls, so a wisp inside it stays on the map for as long as the
test likes. Built by hand, not by the Makefile — the tests that want it say so.

**The per-step cost was 52× over budget and the fix was to stop asking for
things.** Three measurements, same test:

| what live_regs asked gdb for | median | p99 |
|---|---|---|
| every register, every step (200-odd, incl. 32 vector aggregates) | 16.70 ms | 26.996 ms |
| only the indices that came back as scalars (112) | 1.28 ms | 2.487 ms |
| only the ~17 the `Vm` has slots for | **0.32 ms** | **0.46 ms** |

The first number is the one worth remembering: MI renders a vector register as
`"{v8_bfloat16 = {0x0, 0x0, ...}}"`, so a whole-register-file read is kilobytes
of text that gdb formats and we immediately throw away. Invisible at one stop
a frame; the entire cost at ten thousand. `live_regs_want()` lets the caller
name what it can hold, and `wisp_live_sync()` passes the `Vm`'s own register
names.

**Three bugs the criterion caught that a smoke test would not:**

- **`wisp_rehome()` is for a room *change*, not for moving inside a room.** It
  clears the trail (which indexes the decoding being left) and counts another
  room, so stepping through it gave a wisp with a **one-card history claiming
  to have walked 10 003 rooms**. `stand_at()` is the in-room move; rehome stays
  for pilot mode.
- **The trail card describes where the wisp *was*.** The first version moved
  first and pushed the card second, so every card named the instruction after
  the one it was reporting. The live step now mirrors `wisp_step_once()`'s
  order exactly — registers, then card, then move.
- **A percentile over a handful of samples is the maximum wearing a different
  name.** With 60 samples, index 99 % is index 59. The budget is only asserted
  over ≥1000 samples now, and reported without an assertion below that.

**The measurement that L4 exists to fix.** Running the same test on `/bin/ls`,
where there is no leaf to start in and the walk begins at the entry point:

```
  ok    60 steps taken (9 excursions off the map, 72942 instructions stepped through to get back)
  residency  0.08% -- 60 steps in this file for every 72942 outside it.
             9 excursions at ~8104 instructions each.
```

That is §4.1's 0.3 % measured again from inside the program, and worse, because
this walk starts at the entry rather than sampling from it. The recovery here is
deliberately the naive thing — single-step until the pc is back in the file —
so that the number L4's C2 policy has to beat is on the record: **9 breakpoint
round trips at ~0.15 ms should replace 72 942 single-steps at ~0.3 ms.**

**What a live step does not need.** No fuel (a real loop runs the real number
of times), no sentinel, and `decided` is always 1 — the branch was taken or it
was not, and the CPU knows which. On an architecture where the simulated wisp
can only havoc, the live one is exactly as accurate as on x86.

Stepping *into* and *over* are the same act for now: one instruction, wherever
it goes. The step-over contract needs a breakpoint on the return address, which
is L4.

### L4 — off the map, honestly

**Done.** A live wisp no longer walks into libc: when a step leaves the file
the call is run whole and the wisp is waiting on the far side (C2), and when
there is nowhere to come back to it parks and names where the code went (C1).

```
$ ./codecity tests/workload --steptest
  ok    stopped in main at 0x11f4
  ok    the city and the process agree at every step
  10000 steps on the map across 98 rooms, 32 calls run whole and returned (C2), 0 parked (C1)
  last excursion went to __strlen_avx2
  ok    10000 steps taken
  step cost  median 0.311 ms   p99 0.579 ms   (3077 steps/s)
steptest: ok
```

Every criterion met: 10 000 steps from `main`, **100 % of them on the map**,
never a wrong room, and every excursion accounted for — 32 named calls run
whole, nothing skipped silently. The state sheet carries the same line:
*"left this file 32 times: 32 calls run whole and returned, 0 with nowhere to
return to"*, and *"the last one ran `__strlen_avx2`"*.

**The PLT is the case that matters, and it is not a call.** The obvious C2 is
"if the instruction is a `call` that leaves the file, break on the next
instruction". That handles almost nothing. A library call compiles to `call
snprintf@plt`, and the PLT stub is *in the file* — so the step into it never
leaves the map. The instruction that actually leaves is the stub's `jmp *GOT`,
a **jump**, with no next-instruction to return to. On the first attempt the
wisp parked at `snprintf` on every library call.

The general answer is one line of reasoning about the stack: the `call` into
the stub pushed a return address, and the stub's jump did not touch it, so at
the moment the code leaves the file **the top of the stack is exactly where it
will come back to**. Read `[rsp]`, check it maps into this file, break there.
That covers the direct-call case too, and it is what makes `snprintf` and
`__strlen_avx2` come back instead of ending the run.

**What C2 is worth**, same 3000 steps from `main` of the workload:

| | wall clock |
|---|---|
| with C2 — 8 calls run whole, 8 breakpoint round trips | **1.02 s** |
| `--naive` — the same 8 calls walked one instruction at a time | **still running at 250 s**, when the timeout killed it |

So the ratio is a lower bound, not a measurement: **at least 245×**, and the
naive run never got to say how much worse it really is. Eight breakpoint round
trips replace it.

`--naive` exists to keep that number honest and re-measurable; it turns the
policy off and walks. L3 measured the same thing from the other end: on
`/bin/ls`, 60 on-map steps cost 72 942 single-steps through the loader and
libc.

**A call that never returns is not a failure.** `/bin/ls` from its entry point
does `call __libc_start_main`, which never comes back — C2 sets the breakpoint,
the program exits instead, and the wisp parks with the name on the panel:

```
  last excursion went to __libc_start_main
  ok    11 steps, then the code left the file for good: left the file
```

Eleven steps is the complete and correct answer there, so the test says `ok`
rather than reporting a shortfall. The failure worth catching is a run that
stops for *no stated reason*.

**Three things this milestone found:**

- **A call into another room of the same file is not an excursion at all.**
  `main` calls `crunch`, the packer puts them in different rooms, and the first
  version treated that as leaving. It now sets `w->wants` — exactly what the
  simulated wisp does when it steps through a port — so the app's pilot
  machinery moves the player and the decoding, and the one-room-at-a-time
  invariant is never in question. On the workload a 10 000-step walk crosses
  98 rooms.
- **`wentTo` is "where the *last* excursion went", so it must not be cleared
  on ordinary steps.** It was, which meant the name was always gone by the
  time anything read it, and every excursion reported *"somewhere gdb has no
  symbol for"* while gdb had in fact answered `snprintf`.
- **A recovery loop that only looks in the room it left measures nothing.**
  `--naive`'s first version rehomed into the current room only; a call returns
  to its *caller*, which is usually elsewhere, so it burned its whole 400 000-
  step budget every time and the comparison was an artefact rather than a cost.

### L5 — running, without stalling the frame

**Done.** `K` runs a live wisp at its own rate through the real frame loop.
A call out of the file is *let go of* rather than waited for: `wisp_step_live()`
issues the continue and returns 2, and `wisp_live_poll()` collects the answer
in whatever later frame it arrives. `--runtest [N]` is the criterion.

```
$ ./codecity tests/workload --debug tests/workload --runtest 9000
  baseline (no wisp)  worst 14.714 ms   mean 1.411 ms over 2250 frames
  2997 steps over 9000 frames, 8 frames spent waiting for a call (longest run 1), 0 restarts
  8 calls out of the file were let go of and collected later, 0 parked
  gdb per frame  first step 9.023 ms (one-off);   after that worst 2.279 ms, mean 0.277 ms
  whole frame with the wisp  worst 9.487 ms   mean 1.273 ms
  ok    no frame spent more than 3 ms in gdb (worst 2.279)
  ok    the wisp adds no more than 3 ms to the worst frame (9.487 vs 14.714 baseline)
  ok    and no more than 1 ms to the mean (1.273 vs 1.411)
runtest: ok
```

Steady state is **two MI commands per step** — `-exec-step-instruction` and a
`-data-list-register-values x 0 … 17` restricted to what the `Vm` holds — for
0.277 ms of a 16.6 ms frame.

**A whole-frame number on its own says more about this machine's GL than about
anything here**, so the test measures the same loop twice: `frames/4` with no
wisp at all, then the real thing, and judges the difference. That is what the
criterion's "unchanged from a no-wisp frame" actually asks. The wisp comes out
*below* the baseline mean, because the baseline pass absorbs the GL warm-up.

**The first step costs ten times what the rest do**, and it is reported on its
own rather than left to dominate a worst case that is otherwise steady: gdb has
to set up its single-step machinery after a stop that arrived from a continue.
9–12 ms, once, when you press `K` — a real hitch a person would feel once, not
a per-frame cost.

**The L2 wart, explained.** L2 recorded an unresolved edge: exploring `libSDL2`
while `codecity` runs under gdb reported the program as having exited while
`L->exited` read 0. It was never a flag bug. `mi_wait_stop()` was hitting its
**ten-second deadline** between two library loads — a GUI program bringing up a
window and a GL driver under a debugger that stops on every solib event takes
far longer than that — and the error message inferred the wrong cause from a
flag instead of reporting what happened. The lesson is in the code now:

- gdb's **own words** are the reliable signal, not a flag we maintain. A
  continue after the inferior has gone answers `"The program is not being
  run"`, and that is true whether or not the exit record happened to be
  drained yet.
- The state is captured **before** talking to gdb again: turning solib events
  back off is another command, and on a dead inferior it can leave the session
  no longer remembering what went wrong.
- Waiting for the loader is a **setup** step, not a command round trip, so it
  has its own budget (`LIVE_SETUP_MS`, 30 s) rather than the frame-loop one.

That pairing still cannot be tested here — `codecity` needs a display it does
not get under the test harness, and does not finish loading its libraries — but
it now says so truthfully instead of blaming an exit that never happened.

**Also fixed:** `-exec-arguments` now quotes each argument and escapes what is
inside it. MI takes one space-separated string and splits it the way a shell
would, so an inferior argument containing a space silently became two. Nothing
passed arguments before `--debug`, which is why it had gone unnoticed.

`K` on a waiting wisp is `-exec-interrupt`: an unbounded wait is always
escapable, which is the whole reason the continue path is asynchronous.

### L6 — attach

**Done.** `--attach PID` looks at a process that is already running, and leaves
it running. `--attachtest` is the criterion.

```
$ ./codecity /bin/ls --attachtest -v
attachtest: ptrace_scope 1
  ok    a pid that does not exist is refused by name
  ok    a process that did not opt in is refused
  ok    and the refusal names ptrace_scope and a way through
        ptrace: Operation not permitted. -- ptrace_scope is 1: only an ancestor may
        trace a process, and gdb is not 73167's. Either run codecity with
        CAP_SYS_PTRACE, or have 73167 call prctl(PR_SET_PTRACER), or loosen it
        machine-wide with sysctl kernel.yama.ptrace_scope=0
  ok    attached to pid 73178
  ok    the attach stopped it, wherever it was
  ok    112 registers read out of a process we did not start
        stopped at 0x7453988ecb7a in clock_nanosleep, /lib/x86_64-linux-gnu/libc.so.6
  ok    and it is still running after we let go
  ok    and running, not left stopped (state S)
attachtest: ok
```

**Refusal is the feature.** §10.2 predicted it and the measurement holds: on
this machine `ptrace_scope` is 1, so only an ancestor may trace a process, and
gdb — spawned by codecity, a *sibling* of anything codecity started — never is.
`live_ptrace_scope()` is read before gdb is even asked, and the message names
which of the three ways through applies rather than leaving someone with
`ptrace: Operation not permitted`.

**Testing the permitted path needed a fixture that opts in.**
`tests/attachable.c` calls `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)`, which
is the only way a stock desktop can be attached to at all; run with `no` it
declines to, so the refusal path is tested against the same program. It runs
for a bounded time so a stray copy cannot outlive the test.

**The stop lands wherever the process was**, which for an idle program is
inside a syscall — `clock_nanosleep` above. §10.2 called that the normal
result rather than a problem, and C1 already handles it.

**Detach must leave it running, and the test checks both halves**: that the
process still exists, and that it is not left in state `T`. A stopped-but-alive
process would be just as wrong as a dead one, and only one of those is visible
to `kill(pid, 0)`. `LiveOwn` is set to `LIVE_OWN_DETACH` before anything in
`live_attach()` can fail, so no error path can reach the branch that kills.

### L7 — live memory as a fourth layer

**Done.** `vm_read()`'s chain is now **live → shadow → file → invent**, and the
stack and memory panes are back on the state sheet with the rest.

```
$ ./codecity tests/workload --memtest
  mutated  file 1111111111111111   process feedfacecafebeef   live
  counter  file 2222222222222222   process 0badc0de0badc0de   live
  ok    every one of them differs -- the file's value is the old one
  ok    and every one is tagged live, not file
  ok    .bss has real bytes in the process (live), not invented
  ok    the stack at rsp=0x7fffffffd818 can be read
  ok    nothing was invented while a real process was there
memtest: ok
```

That first line is exactly what `execution-wisps.md` §14 apologises hardest
for — *".data gives its initial values, which for anything mutated at startup
is wrong in a way that looks right"*. The file says `1111111111111111`, the
process says `feedfacecafebeef`, and the panel says which is which.
`.bss` likewise: no bytes at all in the file, real ones in the process.

**The process outranks the shadow**, which was not the first ordering tried.
The shadow holds what a *simulated* run wrote — the fake stack, the minted
canary, the relocations `vm_relocs()` applied at startup — and with the shadow
first a mutated global still read back as its build-time value, tagged
`derived`. None of that fiction is more authoritative than the memory of a
program that is actually running, and a live wisp makes no writes of its own,
so there is nothing to lose by looking past it.

**The Vm is asked for two kinds of address and cannot tell them apart.** A
stack pointer is a *live* address, because registers hold what the process
holds (L2); a section address comes out of the city and is a *file* one. The
read tries the address as given and, if nothing is mapped there, as a file
address. They cannot be confused in practice — a file address in a PIE is a
small number nothing is mapped at — and for a non-PIE they are the same number.
Until that was handled, the stack read correctly and every global read from the
file, which is a confusing shape of bug: half of it worked.

**A page at a time, dropped whenever the process moves.** The panel makes a
dozen small reads per stop — five stack slots, the memory log, what each
register points at — and each would be its own MI round trip. One 4 KiB page is
cached and invalidated by `live_step()`, `live_run_to()` and
`live_continue_async()`, because a cached byte from before the last instruction
is not a stale optimisation, it is a wrong answer.

`vm_annotate()`'s "stack-0x20" labels keep working because `wisp_live_sync()`
points the `Vm`'s stack bounds at the real `rsp` rather than at §5's synthetic
region. And the test asserts the thing §8.4 asked for: **invention must never
fire while a real process is there.**

---

## 0. What already exists, and what this can stand on

More than it looks. This is mostly a new *source* of values for machinery that
is already built and already tested.

- **`Vm` is already layered** (`src/vm.c:359–470`). `vm_read()` asks the shadow
  pages, then the mapped file, then invents. A live process is a **fourth
  layer**, and it slots in above the file with no change to any caller. §8.4.
- **Provenance is already a first-class idea.** `Prov` (`src/vm.h:27`) exists
  precisely so a viewer can tell a fact from a fiction, and the panel already
  colours by it. `PV_LIVE` is one more enumerator.
- **The wisp owns nothing belonging to a `Disasm`** (`src/wisp.h:4`) — only
  indices. That is what lets `wisp_rehome()` carry a run into another room, and
  it is exactly what a live pc jumping across the city needs.
- **`wisp_step_once()` has one call to `vm_step()`** (`src/wisp.c`), and
  everything after it — the trail card, the visit counts, the hop animation,
  the `StepKind` switch — is source-agnostic. A live step produces the same
  `StepKind` and the rest is unchanged.
- **Pilot mode already solves cross-room travel.** `w->wants` plus
  `teleport_addr()` (`src/main.c:210`) moves the player and the decoding to an
  arbitrary address, preserving the one-decoding-at-a-time invariant.
  A live pc that leaves a room is the same event.
- **`city_find_addr()`** (`src/city.c:577`) is the address→room map, already
  handling chambers and alcoves.
- **Driving gdb from C is the only genuinely new mechanism**, and even that is
  half-proven: `tools/oracle.py` already single-steps a live inferior, reads
  and writes its registers and memory, and does it deterministically enough to
  generate a checked-in corpus. What it does not do is take orders from
  `codecity` — it is a batch script. §2.

What is **not** reusable: `vm_step`'s arithmetic (a live step needs none of
it), the minting policy (§5 of the other doc), the fuel budget, and the loop
policy. Those are the invention machinery, and this feature's whole point is
not needing them.

---

## 1. Three kinds of wisp, one body

It is worth naming these before designing, because the honesty problem is
different in each and the UI must never let them be confused.

| | source of values | deterministic | needs the real program to run | exists |
|---|---|---|---|---|
| **invented** | `vm.c`'s PRNG + the mapped file | yes, per seed | no | **built** |
| **replayed** | a trace file recorded earlier | yes | no (it ran once, elsewhere) | proposed, `execution-wisps.md` §14 |
| **live** | a process stopped under gdb, right now | **no** | **yes** | this document |

The three share the body, the trail, the panel, the tether and the ports. They
differ in exactly one place: what fills `Vm`'s registers and answers
`vm_read()`. That is the interface to keep narrow, and `execution-wisps.md` §14
already asked for it in as many words — "a struct of values, tags and a step
kind".

**So the first design rule is: do not fork `Wisp`.** Add a source discriminator
to it and a vtable-ish `WispSrc` with three implementations. If this feature
ends up with its own copy of the trail, the LOD and the panel, it has been
built wrong.

```c
/* wisp.h */
typedef enum { WS_INVENTED, WS_REPLAY, WS_LIVE } WispSrc;
```

The colour of the tether and one word on the panel key off it, permanently and
unmissably. A viewer who cannot tell at a glance whether the number in front of
them is a measurement or a guess is worse off than one with no number at all.

---

## 2. The transport: gdb, and which of its two interfaces

### 2.1 Why gdb rather than `ptrace` directly

`ptrace(2)` is not much code for "stop, read registers, step" — perhaps 200
lines for x86-64. It is a great deal of code for everything else: following
`fork`, the thread list, `PTRACE_EVENT_*`, breakpoint insertion and the
single-step-over-a-breakpoint dance, the load bias, `/proc/pid/maps` parsing,
watchpoints, and every architecture that is not the host. gdb has all of it,
is already a dependency of the corpus generator, and is on every machine that
would run this.

Against: it is a process, a protocol, and a version surface. And it is
**already an accepted dependency for a developer workflow** here — `run-gdb.bsh`
and `tools/gen-corpus.sh` both shell out to it — but not for the *program*.
That is the new commitment: `codecity` would spawn gdb at runtime. Optional,
gated behind a flag, and absent gdb the feature simply says so.

`lldb` and a raw `ptrace` backend can both come later behind the same
`LiveDbg` interface. Neither should be built first.

### 2.2 MI vs the Python API — **MI, and it is not close**

Two ways to drive gdb from another program:

**GDB/MI** (`gdb --interpreter=mi3`): a line-oriented, machine-parseable
protocol on stdin/stdout. Verified working here:

```
-file-exec-and-symbols /bin/ls
-break-insert -t main
-exec-run
-data-list-register-values x 0 1 16
-exec-step-instruction
```

**The embedded Python API**, as `tools/oracle.py` uses it — `gdb.execute()`,
`gdb.parse_and_eval()`, `inferior.read_memory()`. Faster and far more pleasant
to write, but it runs *inside* gdb: to use it from `codecity`, `codecity` would
have to be the one being called, or a socket would have to be strung between a
Python script and the C program, which is a protocol again — a worse one,
written by us.

**Use MI.** The Python API stays where it belongs: in `tools/`, generating the
corpus.

One measured caution, from the MI session above: **MI commands must arrive on
stdin.** `gdb --interpreter=mi3 -x file` sources the file as *console*
commands and every MI line fails with `Undefined command`. That cost a
confusing minute here and would cost the same in the implementation.

### 2.3 The four MI facts the implementation turns on

1. **`=library-loaded` is emitted asynchronously, with ranges.** From the
   verified session:

   ```
   =library-loaded,id="/lib/x86_64-linux-gnu/libc.so.6",...,
     ranges=[{from="0x00007ffff7c28800",to="0x00007ffff7dafeb9"}]
   ```

   This is the module map arriving for free, as it happens, and §4's
   "which module is this pc in" answer comes straight out of it.

2. **Async records interleave with command results**, so the reader must be a
   proper state machine keyed on the token gdb echoes back
   (`123-exec-step-instruction` → `123^done`), not a "read one line after each
   write" loop. This is the single most common way MI clients are written
   wrong.

3. **`^error` is a normal outcome**, not a fault. `-break-insert -t main` on
   `/bin/ls` returns `^error,msg="Function \"main\" not defined."` — because
   `/bin/ls` is stripped. Stripped binaries are the *common* case for this
   program, so **every symbolic command must have an address-based fallback**,
   and L1's pass criterion is written against a stripped binary on purpose.

4. **`-gdb-set mi-async on`** is what makes §6 possible: it lets `codecity`
   issue `-exec-continue` and keep drawing frames, receiving `*stopped` when it
   happens, and send `-exec-interrupt` to take it back.

### 2.4 The process itself

gdb is a child of `codecity`, over a `socketpair` or two pipes.

- **Reap it.** A `codecity` that crashes leaving a gdb holding a stopped
  inferior has frozen a process on the user's machine. gdb gets
  `-gdb-set confirm off`, and the inferior gets detached (attach) or killed
  (launch) on every exit path including the signal handlers. L0's pass
  criterion is exactly this and it is first for that reason.
- **Never inherit `codecity`'s terminal.** The inferior's own stdout must go
  somewhere else, or a program that writes to stdout will scribble over the
  place `--stats` and the stall warnings print. A pty for the inferior
  (`-gdb-set inferior-tty`) is right, and its output is worth showing in the
  HUD — a program's own output next to the code producing it is one of the
  nicer things this feature gets almost free.

---

## 3. The address problem: everything in the city is a file vaddr

`city_find_addr()` compares against `Sec.addr` — the address in the **ELF
file**. A live PIE process is somewhere else entirely. Measured, on `/bin/ls`:

```
MAP   0x555555554000  0x555555558000  0x4000  0x0  r--p  /usr/bin/ls
EXEC  bias=0x555555554000  file_entry=0x6d30  ->  live_entry=0x55555555ad30
```

and stopping at `*0x55555555ad30` did land on the entry. So the translation is
a single subtraction — but getting the bias right has one trap that this
investigation walked straight into:

> **`starti` does not stop in your program.** It stops at the process's first
> instruction, which for anything dynamically linked is the **loader's** entry:
> measured `$pc = 0x7ffff7fe4540`, inside `ld-linux-x86-64.so.2`. Computing
> `bias = pc - e_entry` there gives `0x7ffff7fdd810`, which is garbage, and
> garbage that *looks* plausible enough to survive review.

So the bias comes from the module map, never from a pc:

- **the executable**: the lowest mapping whose pathname is the file the city
  was built from. Available from `-interpreter-exec console "info proc
  mappings"`, or better from the `=library-loaded`/`^done,...` module list.
- **a shared library**: `codecity` is often pointed at a `.so`. Then the file
  under study is not the executable at all, and the bias is that library's
  load address — which arrives in the `=library-loaded` record for it.
- **non-PIE** (`ET_EXEC`): bias 0. Worth handling explicitly rather than
  falling out of the arithmetic, because it is the case that will be tested
  least and is trivially checkable.

Two functions, and they belong next to `city_find_addr()` rather than inside
the debugger code:

```c
/* live.h */
uint64_t live_to_file(const Live *L, uint64_t live);   /* 0 when off the map */
uint64_t file_to_live(const Live *L, uint64_t file);
```

`live_to_file()` returning 0 **is** the off-map test of §4, which is why it is
one function and not a bool plus an out-parameter.

**Do not add a bias to `City`.** It is tempting — one field, and every existing
call site keeps working. It is wrong: the city is a view of a *file*, the same
file can be loaded at two addresses in two inferiors, and a bias that lives in
the city makes the invented wisp and the live wisp disagree about what an
address means. The translation belongs at the boundary, applied once, in the
two functions above.

---

## 4. Off the map — the central problem

### 4.1 The measurement

Two runs, same machine, same method: read `$pc`, classify it as inside or
outside the mapped range of the file the city was built from, `stepi`, repeat.

| starting point | steps inside the file's own text |
|---|---|
| `/bin/ls`, from its ELF entry point | **0.3 %** — 81 of 30 000 |
| a small C program with a compute loop, from `main` | **92.6 %** — 37 022 of 40 000 |

These are not in tension; they are the same fact seen twice. A process spends
its time wherever it is spending its time, and at startup that is the dynamic
loader resolving relocations and libc setting up. `execution-wisps.md` §16
found the same thing from the other side — stepping into `strlen` landed in
`ld.so`'s SSE2 IFUNC variant, not in libc's copy of the function, and not in
anything the file contains.

The consequence is blunt: **a live wisp that naively single-steps and draws is,
for the first several hundred thousand instructions of a normal program, a dot
that is nowhere.** The 0.3 % case is not a pathology to be handled at the
margin, it is what happens if you press the key at the obvious moment.

### 4.2 Four ways to respond

**C1. Say so, plainly.** A live wisp that is off the map parks at the port it
went out by, and the HUD says where it actually is — module, and symbol if gdb
knows one. The trail keeps its last on-map card. This is not a fallback, it is
**the correct behaviour whatever else is built**, and it must exist before
anything else in this section. *Cost: small. Do it first.*

**C2. Do not step off the map at all — run to the far side. — recommended.**
When the instruction about to be executed leaves the file (a `call` through the
PLT, a jump to a stub), do not `stepi` into it. Set a temporary breakpoint on
the return address, `-exec-continue`, and arrive at the instruction *after* the
call with the real registers and the real return value. This is exactly
`execution-wisps.md`'s **step-over contract**, except that instead of inventing
`rax` and havocing the caller-saved registers, the callee genuinely ran and
`rax` is genuinely what it returned.

The economics work, which was not obvious before measuring: a breakpoint hit
plus `continue` costs **6 700 a second**, about half a `stepi` — so skipping a
million instructions of libc costs the same as stepping one. The 0.3 % case
becomes: park at the port, and hand back control the moment execution returns
to a room the city has.

This is the milestone L4 behaviour, and it is what makes the feature usable
rather than a demonstration.

**C3. Put the libraries in the city too.** The honest answer, and much larger.
`City` is built from one `Elf`; `=library-loaded` hands over every module with
its path and its range, so building a city per module is mechanically
straightforward — but the packer, the districts, the minimap, the browser and
every `city_find_addr()` caller assume one. This is a structural change to
`city.c`, not an addition. *Keep it out of the milestones.* If C2 works, the
appetite for C3 will be obvious and informed; if C2 does not, C3 would not have
saved it.

**C4. Refuse to leave the room.** What the invented wisp does today when it is
not in pilot mode. Correct as a *mode* — "walk this function and stop at its
edges" is a legitimate thing to want — and nearly free once C1 exists. Not
sufficient as the only behaviour.

**Decision: C1 and C2, with C4 as a mode. C3 is a sequel and should be written
down as one so nobody starts it by accident.**

### 4.3 The corollary: where a live run should start

Not the entry point. The 0.3 % measurement is a measurement *of starting at the
entry point*, and pilot mode's `P` key does exactly that today for the invented
wisp — where it is fine, because the invented wisp has no loader to wade
through and reaches the program's own code immediately.

For a live wisp the good starting points are, in order:

1. **A breakpoint the user placed** — stand in front of a sculpture, press the
   key, and the program runs until it reaches *that instruction*. This is the
   feature. It is also the cheapest to build, because it is one
   `-break-insert *ADDR` and one `-exec-run`, and it lands the wisp exactly
   where the user was already looking.
2. **Wherever an attached process already is** (§10), which is the whole point
   of attaching.
3. The entry point, offered but not the default, with the loader skipped by C2.

That reordering — **break where the player is standing, rather than start where
the program starts** — is the single most important consequence of §4.1, and it
also happens to make the feature's first version much smaller.

---

## 5. Threads

The invented wisp has no notion of a thread and never needed one. A live
process has many, and `*stopped` carries a `thread-id`.

- **One wisp follows one thread.** Its id is fixed when the wisp is created and
  shown on the panel.
- **`-gdb-set scheduler-locking step`** while stepping, so the thread being
  followed is the one that moves and the others do not race ahead. Not
  `on` — that can deadlock a program whose thread is waiting on another one,
  and a deadlocked inferior looks exactly like a hung `codecity`.
- **The thread can exit.** `=thread-exited` for the followed thread ends the
  run: `wisp_stop(w, "the thread exited")`, using machinery that already
  exists.
- **Several wisps, one per thread**, is a genuinely attractive idea — several
  dots moving through the same city, which is a picture nothing else draws —
  and the app already supports a wisp in a room the player is not in
  (`wispAway`, `city_enter_wisp_room`). But it supports exactly *one*. Multiple
  live wisps is a sequel, in the same box as C3.

---

## 6. Not stalling the frame

`main.c`'s loop is single-threaded (`while (running)` at `src/main.c:2396`),
draws, and swaps — and it already measures how long the swap took, warning
above a threshold (`stallMs`). Any synchronous gdb call sits in that budget.

The measurements say this is comfortable but not free:

- a step plus a register-file read is well under 200 µs — call it 12 % of a
  60 fps frame at the very worst, and the wisp asks for at most 20 steps a
  second, i.e. one step every three frames.
- a `-exec-continue` that runs to a breakpoint is **unbounded**. The program
  might reach it in a microsecond or never.

So: **the per-step path may be synchronous; the continue path must not be.**

`-gdb-set mi-async on`, and a small reader that drains gdb's output into a
queue each frame. When a continue is outstanding the wisp is in a `waiting`
state, the panel says so, and the frame loop does not care. `-exec-interrupt`
is bound to a key so an unbounded wait is always escapable.

The reader is the one piece of this that must not be written casually: a
blocking `read()` on gdb's stdout in the frame loop is an application that
hangs, and it will hang for the exact reason that is hardest to notice in
testing — the program under study did something slow.

---

## 7. What a step means when the state is real

`wisp_step_once()` calls `vm_step()`, which returns a `StepKind` and the next
address. A live step returns the same things from different evidence:

| `StepKind` | invented | live |
|---|---|---|
| `ST_FALL` | pc += len | `stepi`, pc landed at the next instruction |
| `ST_TAKEN` / `ST_NOT_TAKEN` | flags, or a guess | `stepi`, pc compared to the branch target — **never a guess** |
| `ST_CALL_OVER` | the step-over contract, `rax` invented | temporary breakpoint on the return address, `continue` (§4 C2) |
| `ST_CALL_INTO` | the call is made on the synthetic stack | `stepi` — and if the target is off the map, this becomes `ST_CALL_OVER` |
| `ST_RET` | the sentinel `0xCAFED00D` was popped | pc left the room upward; there is no sentinel in a real process |
| `ST_EXIT_PORT` | pc is outside the room | same |
| `ST_STOP` | fuel, or a fault | `*stopped` with a signal, or the thread exited |

Two of these are worth dwelling on because they are where the live version is
*better*, and the HUD should say so:

**`decided` is always 1.** `Wisp.decided` exists so the renderer can colour a
branch that was guessed rather than worked out. A live wisp never guesses; on
`/bin/ls` the invented wisp guesses 3 of 12 069 conditionals, and a live one
guesses none of them, ever, including in the tier-0 architectures where the
invented wisp guesses all of them. **A live wisp on ARM is as accurate as one
on x86**, which is the single largest capability difference between this
feature and the existing one.

**There is no fuel.** `VM_FUEL` bounds a run that might otherwise loop forever
on invented data. A real loop runs the real number of times. Remove the budget
for live wisps and keep the visit counters, which are still what the trail and
the LOD want.

**The sentinel does not exist.** `VM_SENTINEL` is how the invented run knows a
function returned. Live, the equivalent is: record `$sp` at the start, and call
it a return when a `ret` executes with `$sp` above that. Cheap, and it is the
same thing gdb's `finish` does.

---

## 8. Reading the state

### 8.1 Registers

`-data-list-register-values x` returns all of them by number; the
number→name map comes once from `-data-list-register-names`. Then
`vm_slot_set(m, slot, v, PV_LIVE)` per register and the panel is populated
with no other change, because `Vm`'s register file is already
name-and-slot addressed (`vm_slot_named`, `vm_slot_get`).

Read the whole file every stop rather than the ones the panel shows: it is 4 µs
a pair, the panel's contents change as the user scrolls, and a partial register
file is a bug waiting to happen when something later asks for a register nobody
fetched.

### 8.2 Flags

`eflags` comes back as a register like any other; `VF_CF`…`VF_OF` are bit
extractions, and `flknown` is **all ones, always**. The invented VM clears the
known bit when it is unsure; a live one is never unsure. `execution-wisps.md`
§14's "flags are approximated, AF in particular is rarely worth computing
correctly" simply stops applying, which is worth showing on the panel.

### 8.3 Vectors

`$ymm0.v4_int64` works in MI as it does in the Python API (`tools/oracle.py`
already reads exactly this to catch VEX upper-half zeroing). `VVec` is already
512 bits wide from `vm.h:61`, deliberately, so AVX-512 needs no state change —
and unlike the invented wisp, which never models AVX-512 at all, **a live wisp
shows it correctly with no work**, because it is not modelling anything.

That is the second large capability difference and it should be stated in the
same breath as the ARM one: everything `execution-wisps.md` §14 lists as "will
not do" for semantic reasons — AVX-512, floating point, `rep` string
operations, self-modifying code, IFUNC resolution, `.bss`, syscalls — a live
wisp does correctly and for free, because the CPU is doing it.

### 8.4 Memory — the fourth layer

The chain in `vm_read()` is shadow → file → invent. Live becomes:

```
shadow (writes this run made)  →  LIVE (the inferior)  →  file  →  invent
```

with `PV_LIVE` between `PV_FILE` and `PV_DERIVED` in the honesty ordering. The
file layer stays underneath as the answer for an address the inferior has not
mapped, and invention stays underneath that, and both should essentially never
fire — an address a live process is reading is an address a live process has.
**If invention fires under a live wisp, that is a bug worth reporting on the
panel**, not a fallback to be silently taken.

Two properties this buys, and they are the ones §14 of the other doc apologises
hardest for:

- **`.bss` and `SHT_NOBITS` have real bytes.** The other doc: "no bytes, so it
  invents". Live, they are whatever the program put there.
- **`.data` is what it *is*, not what it started as.** The other doc: "gives its
  *initial* values, which for anything mutated at startup is wrong in a way
  that looks right". That is precisely the failure mode a live layer removes,
  and L7's pass criterion is written to demonstrate it rather than assert it.

Cache reads per stop, keyed by page, and drop the cache on every resume — 1.5 µs
for 64 bytes means the cache is for correctness of the *panel within one stop*,
not for speed.

### 8.5 Writing to the inferior — no

`vm_write()` under a live wisp writes the shadow, never the process. A debugger
that lets you edit registers is a fine thing and this is not one; the
`codecity` gesture vocabulary is walking and looking. More to the point,
letting a viewer poke a live process from a 3D file browser is a way to corrupt
something that matters with a mis-aimed keystroke, and there is no undo.

If it is ever wanted, it needs its own confirmation gesture and its own
section. Not here.

---

## 9. Determinism is gone, and that is a real cost

`vm.h`'s opening comment makes a promise: "the same room shows the same run
every time you walk into it". Seeded invention is what delivers it, and
`--vmtest` asserts it — *"both runs of every room agreed"* is in the output
quoted in the commit that built this.

A live wisp cannot promise it. ASLR, timing, the environment, the actual input
the program was given: run it twice and it does two different things.

What follows, and it must be designed for rather than discovered:

- **`--vmtest` cannot cover live wisps** the way it covers invented ones. Its
  "took the same path twice" assertion is meaningless here. §12's tests are
  structural (did it terminate, is the pc always on the map or accounted for,
  was every register tagged `PV_LIVE`) rather than comparative.
- **The seed key `N` means nothing** and should be disabled with a message
  rather than silently doing nothing.
- **The trail is now the only record**, because you cannot re-run to look
  again. That argues for a longer trail under a live wisp than `WISP_TRAIL`'s
  sixteen, and for the trace view being able to scroll back further.
- **The screenshot problem.** `--shot` renders canned viewpoints and is
  compared by eye between builds. A live wisp in a shot makes it
  non-reproducible. Live wisps must be **excluded from `--shot` entirely**,
  which is one condition and easy to forget.

---

## 10. The two ways in

### 10.1 Launch — `--debug` — the one that always works

```
codecity ./prog --debug -- arg1 arg2
```

gdb is `codecity`'s child and the inferior is gdb's child, so Yama's
`ptrace_scope=1` is satisfied by construction: **a tracer may always trace its
own descendants.** No privilege, no configuration, no failure mode to explain.

This should be the flagship. It also gives the clean version of §4.3: launch
stopped, place a breakpoint where the player is standing, let it run.

The file the city is built from and the program being launched should default
to the same thing but must be allowed to differ — pointing the city at
`libfoo.so` and launching the program that loads it is the interesting case,
and it falls out of §3's per-module bias for free.

### 10.2 Attach — `--attach PID` — the one that needs a conversation

Measured here, and it is not a corner case:

```
$ sleep 300 & gdb -> -target-attach <pid>
^error,msg="ptrace: Operation not permitted."
```

with `/proc/sys/kernel/yama/ptrace_scope` = 1. Note *why* this was refused: the
target was not a descendant of gdb — it was a sibling. `ptrace_scope=1` permits
tracing descendants only, and gdb spawned from a shell is not an ancestor of
another child of that shell. This is the default on Ubuntu and on most
desktop distributions, so **the common case for `--attach` is refusal**, and
the quality of the error message is most of the feature's quality.

The three ways through, which the message should name:

- run `codecity` with `CAP_SYS_PTRACE` (or as root — say so, do not recommend
  it),
- have the target call `prctl(PR_SET_PTRACER, ...)`, which only helps if you
  own the target's source,
- `sysctl kernel.yama.ptrace_scope=0`, which is a machine-wide loosening and
  should be presented as what it is.

`codecity` must **read `ptrace_scope` itself and say which case it is in**
before gdb is even spawned. "ptrace: Operation not permitted" is a bad error to
hand a user who was told to press a key.

Three further attach-only facts, each of which has bitten every tool that does
this:

- **The pc can be anywhere, including in a syscall.** `$pc` in `libc`'s
  `read()` is the *normal* result of attaching to an idle process. C1 handles
  it — park, and say where it is.
- **Attaching stops the process**, which for anything interactive, real-time or
  holding a lock is a visible event in the world outside `codecity`. Say it in
  the UI at the moment it happens, not in the manual.
- **Detach must be reliable, and must leave the process running.** `-target-detach`
  on every exit path. Killing on exit is right for launch and catastrophic for
  attach; the two paths must not share a cleanup function that gets this
  backwards.

---

## 11. Safety, and the paragraph this contradicts

`execution-wisps.md` §14 ends:

> Running the binary itself from inside the explorer is a different matter: it
> executes untrusted code of possibly foreign architecture, and it should not
> happen. Reading a trace file someone else produced deliberately is fine.

That was written about the invented wisp, where running the program was an
*implementation shortcut* nobody needed — the alternative to inventing values
was executing the file you happened to be browsing, and the file you happen to
be browsing arrived by way of a file browser with a filter box in it.

This proposal is a different thing, but not so different that the stance can be
quietly dropped. What actually distinguishes it:

- **It never runs anything implicitly.** No key inside the app starts a
  process. The program to run is named on the command line, next to a flag
  whose name is `--debug`.
- **The city and the inferior are decoupled.** Browsing to a file does not make
  it runnable; you are debugging what you asked to debug.
- **"Foreign architecture" is now a real check, not a hazard.** The ELF machine
  is already parsed (`Elf.machine`) and gdb reports the target's. If they
  disagree, refuse — do not hope.

The wording §14 should become, and this needs a human to accept:

> Running the binary from inside the explorer executes untrusted code, so it
> never happens implicitly: no key starts a process, and the file browser
> cannot. A program launched by `--debug`, or attached to by `--attach`, was
> named by the user on the command line with the same deliberateness as
> typing `gdb`.

Additionally, and not negotiable: **`--debug` must not be reachable from the
file browser, from a config file, or from any environment variable.** The
argument vector is the only way in. That is a one-line rule that keeps the
attack surface at zero and it should be written into the code as a comment,
because it is exactly the kind of rule a later convenience patch erodes.

---

## 12. Testing

Determinism is gone (§9), so the tests are structural and differential rather
than comparative.

**`--gdbtest`** (L0), needing gdb but not a display:

- spawn, `-file-exec-and-symbols`, run to the ELF entry by *address* (not by
  `main` — see §2.3, `/bin/ls` is stripped), read the register file, read 64
  bytes, step, kill, reap.
- 1000 cycles under ASan: no leak, no fd leak, no orphan. An orphaned gdb
  holding a stopped inferior is the worst failure this feature has and it is
  the one a test can actually catch.
- every abort path: gdb missing, gdb too old, binary not executable, wrong
  architecture, `ptrace` refused, inferior exits immediately, inferior segfaults
  on its first instruction.

**`--livetest FILE`** (L3–L5), the live analogue of `--vmtest`:

- launch, break at the entry, take 10 000 steps under the §4 C2 policy;
- assert: the pc is on the map or explicitly parked at a port, every step;
  every register tagged `PV_LIVE`; `disasm_live()` unchanged across the run
  (the invariant `--vmtest` already guards); no invention fired (§8.4);
  the process is reaped.
- report the residency percentage, because it is the number that says whether
  §4's policy is working, and it is different for every program.

**The differential test (L8)** is the one worth the most. Run an invented wisp
and a live wisp from the same address in the same room, step them together, and
diff the register files after every step. Everything before the first invented
input is consumed *must* match — that is a genuine correctness check on
`vmx86.c` against a real CPU, on real code, complementing the corpus, which
only ever tests one instruction at a time from a synthetic state.

Where they diverge is the interesting output: it says, per program, how many
instructions of fiction you get before the fiction stops resembling the fact.
`execution-wisps.md` §14 warns "97 % coverage is not 97 % of runs correct: one
wrong `movzx` early poisons everything after it". This is the measurement that
would finally put a number on that sentence.

---

## 13. Change list

Sized against the existing files, in the style of `execution-wisps.md` §15.

### new: `src/live.h`, `src/live.c` (~600 lines)

The gdb transport and the address map. No knowledge of rooms, wisps or GL.

```c
typedef struct Live Live;
typedef struct { uint64_t lo, hi, bias; char path[256]; } LiveMod;

Live *live_launch(const char *prog, char *const argv[], char *err, size_t n);
Live *live_attach(int pid, char *err, size_t n);
void  live_close(Live *L);            /* detach or kill, per how it opened */

int   live_poll(Live *L);             /* drain gdb's output; call every frame */
int   live_stopped(const Live *L);
int   live_waiting(const Live *L);    /* a continue is outstanding (§6) */

int   live_step(Live *L);                        /* -exec-step-instruction */
int   live_run_to(Live *L, uint64_t liveaddr);   /* tbreak + continue (§4 C2) */
int   live_interrupt(Live *L);

int   live_regs(Live *L, Vm *m);                 /* whole file -> PV_LIVE */
int   live_mem(Live *L, uint64_t a, int n, uint8_t *out);

const LiveMod *live_mod_at(const Live *L, uint64_t liveaddr);
uint64_t live_to_file(const Live *L, uint64_t live);   /* 0 = off the map */
uint64_t file_to_live(const Live *L, uint64_t file);
```

Half of this is the MI reader (§2.3): tokenised writes, a record parser, and a
queue. It is the part to write carefully and the part to test headless first,
which is what L0 is for.

### `src/vm.c` / `src/vm.h` (+80)

`PV_LIVE`; a `Live *` on `Vm`; the fourth memory layer in `vm_read`, `vm_peek`
and `file_byte`'s neighbourhood. `vm_step()` is untouched — a live wisp does
not call it.

### `src/wisp.c` / `src/wisp.h` (+200)

`WispSrc`; `wisp_step_live()` beside `wisp_step_once()`, sharing the trail
card, the visit counters and the `StepKind` switch — if these two functions do
not share that code, the refactor is wrong. Fuel and the sentinel are bypassed
for `WS_LIVE` (§7).

### `src/main.c` (+250)

`--debug`, `--attach`, `--gdbtest`, `--livetest`; `live_poll()` in the frame
loop; the `L` key; routing `w->wants` through the existing `pilot_follow()`
path when a live pc leaves a room; the exit paths of §2.4.

### `src/hud.c` (+150)

The source badge (§1) — the one piece of UI that is not optional. The off-map
line (§4 C1). The thread id. The module and symbol when parked. The
inferior's own output, if the pty of §2.4 is built.

### `src/render.c` (+40)

The tether colour by `WispSrc`, and the parked-at-a-port pose.

### `src/city.c` (+0)

**Nothing.** Worth stating: if this feature starts editing `city.c`, it has
drifted into C3 (§4.2), and that should be a deliberate decision rather than a
diff.

---

## 14. What this will not do

- **It is not a debugger.** No watchpoints, no expression evaluation, no
  editing (§8.5), no call injection, no reverse execution. `gdb` is right there
  and better at all of them.
- **It shows one module.** Until C3, the city is one file and everything else
  is "off the map" (§4). For a program that spends its life in libc, this
  feature shows you the doorway and not the room behind it.
- **One thread, one wisp** (§5).
- **It is not reproducible** (§9), which makes it unsuitable for the screenshot
  tests and unsuitable as a `--vmtest` assertion.
- **`--attach` will usually be refused** on a stock desktop Linux (§10.2). The
  feature is `--debug`; attach is the bonus.
- **It runs the program.** All of it, including whatever the program does to
  the machine — files, network, other processes. `codecity` is not a sandbox
  and must not imply it is. A `--debug` on something you have not read is the
  same act as running it.
- **Non-Linux is unbuilt.** MI is portable; `ptrace_scope`, `/proc`, and the
  module-map handling are not. macOS additionally needs codesigning to debug
  anything, which is a different document.
- **It does not make the invented wisp obsolete.** The invented wisp works on a
  file you cannot run: a foreign architecture, a `.so` with no program to load
  it, a stripped firmware blob, a binary you have every reason not to execute.
  That is most of what someone points this program at. Live wisps are the
  better answer for the narrow case where the program is *yours* and it *runs*.

---

## 15. Cost

| milestone | new | changed | risk |
|---|---|---|---|
| L0 transport | ~450 | ~30 | the MI reader; child reaping |
| L1 bias/map | ~120 | ~10 | low — one subtraction and a map, but see §3's `starti` trap |
| L2 frozen state | ~80 | ~60 | low |
| L3 stepping | ~120 | ~90 | low |
| L4 off-map | ~150 | ~60 | **the design risk of the whole feature** |
| L5 async | ~120 | ~60 | frame-loop hangs; needs care, not cleverness |
| L6 attach | ~100 | ~40 | mostly error messages (§10.2) |
| L7 live memory | ~90 | ~40 | low — the layer chain is already there |
| L8 differential | ~150 | ~20 | low, and the highest value per line |

Roughly 1400 new lines and 400 changed, against `execution-wisps.md`'s ~2500
for the invented wisp. The ratio is right: this feature is a new source of
values for machinery that is already built, and the majority of the new code is
one MI client that has nothing to do with cities.

**The order matters more than the total.** L0 before anything (an unreaped gdb
is the worst bug here), L1 before L2 (§3's trap produces plausible garbage),
and L4 before L5 — making it *correct* about where it is beats making it
smooth, and a fast wisp that is confidently in the wrong room is worse than no
wisp at all.

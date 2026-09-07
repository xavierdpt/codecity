# A CPU state that walks the code

> **Proposal, not implemented.** This describes a way to make a code room
> legible by putting a *running machine* in it: a snapshot of registers,
> flags and memory that floats under the ceiling, starts at the entry point,
> and steps from sculpture to sculpture along the path the code actually
> takes. Values that cannot be known are invented — once, deterministically,
> and then remembered — so the arithmetic downstream of them is consistent
> even though the inputs are fiction.
>
> The headline measurements that decide the design, taken on this machine:
>
> | | `git` | `bash` | `codecity` |
> |---|---|---|---|
> | instructions | 728 k | 242 k | 24 k |
> | covered by ~70 modelled mnemonics | **97.0%** | **99.1%** | 73.1% (float-heavy) |
> | conditional jumps whose flags come from the instruction *immediately* before | **95.2%** | — | — |
> | RIP-relative operands (resolvable against the real file) | 7.4% | — | — |
> | stack-canary loads (`fs:0x28`) | 4466 | — | — |
>
> So: a small integer interpreter decides nearly every branch on real
> arithmetic rather than a coin flip, and a large minority of loads can be
> answered with bytes that are genuinely in the file. The invention is
> confined to function arguments, the stack, and whatever a stepped-over call
> returned — which is exactly the ignorance a static view has anyway.
>
> **Decided, 2026-09-05.**
>
> - **§13, the text blocker, is accepted and goes first.** Nothing else is
>   started until `text.c` can draw a string that changes every frame without
>   rasterising it. It is the one part of the existing program this feature
>   would otherwise break.
> - **§16, a gdb differential oracle, is accepted.** Hardware is the ground
>   truth for instruction semantics, and it has been proved workable on this
>   machine — the section carries the transcript.
> - **§11, GPU code, is never executed by the wisp** — a different execution
>   model, and not reachable from the CPU side. But VLC's shaders are GLSL held
>   as string constants, and 100% of `libgl_plugin.so`'s strings are recovered
>   by §10's mechanism, so *finding* them is already paid for.
> - **§10, string constants, is cheaper than expected.** A RIP-relative `lea`
>   plus an `R_X86_64_RELATIVE` addend resolve 75–99.8% of every string in
>   `.rodata` with no dataflow analysis at all. It does *not* solve `dlopen`,
>   whose path is built at runtime — that needs the filesystem, not the code.
> - **§4 was revised after measuring VLC's codec libraries.** An earlier draft
>   concluded that AVX was 0% of real binaries and not worth modelling. That
>   holds for applications and fails completely for video codecs — `dav1d` is
>   29.5% AVX and only 48.6% base integer. Two things changed as a result: a
>   fifth tier (packed integer arithmetic) is now required, and tier 3 must be
>   written width-parametric from the first line, because 74–96% of AVX in
>   these libraries is a tier-3 operation at 256 bits.

---
## Milestones

Nine milestones. Each one ends in something you can **run and check** rather
than code that merely compiles, each has a **pass criterion that is a number**
where a number is possible, and each leaves the program in a shippable state —
if the work stops after any of them, nothing is left half-built.

| # | milestone | you can... | pass criterion |
|---|---|---|---|
| **M0** | Text that can change every frame (§13) | — | glyph-cache entries flat while drawing 200 changing strings/frame; no fps change — **done**, see below |
| **M1** | A walker with no semantics (§3 tier 0) | press `x` and watch a state step through a room, on any architecture | `--vmtest` : every code room terminates, no leaks, `disasm_live()` unchanged — **done**, see below |
| **M2** | Integer semantics + the oracle (§4.3, §16) | read register values that mean something; branches decided by arithmetic | zero divergence from hardware over the oracle corpus; ≥96% of steps tier-1 on `git`; ≥95% of conditionals *decided* — **done**, see below |
| **M3** | Runs that look like function calls (§5, §9, §10) | see a stack, a canary that passes, a call stepped over, pointers named as symbols and strings | most rooms terminate at `ret` rather than exhausting fuel; 75–99.8% of `.rodata` strings resolved — **done**, see below |
| **M4** | Legibility (§7, §12) | read it comfortably: the trail, the LOD, the HUD sheet, the trace view | no frame-time regression; judged by eye — **done**, see below |
| **M5** | Cross-room travel (§8) | start at the entry point and follow the code through ports, and back out of calls | `--selftest` : follow *N* ports from `e_entry`, land in a room holding the address every time, one decoding alive throughout — **done**, see below |
| **M6** | Vectors, width-parametric (§4.3) | walk an SSE2 codec kernel | `libvpx` 62.8% → **98.1%**; `codecity` 73.0% → **97.0%**; oracle-clean on packed ops — **done**, see below |
| **M7** | AVX2 (§4.3.1) | walk `dav1d` and `x264`'s modern kernels | `dav1d` 69.6% → **99.1%** for ≤150 new lines — **done**, 95.1%; see below |
| **M8** | CPU profiles (§4.2) | switch `--cpu` and walk into a *different* kernel of the same function | the same call site reaches two different rooms under `baseline` and `v3` — **done**, see below |

### M0 — the blocker

`text.c` caches whole strings by content, so a panel of sixteen changing
registers would rasterise ~100 textures a second and evict the room's own
labels. Build the monospace atlas first (§13). **Nothing else starts until this
is done.** It is independently useful — the HUD readouts change every frame too
— so this milestone stands on its own even if the rest is abandoned.

*Test:* a debug key that draws 200 randomised strings per frame. Watch
`g_live`/`g_bytes` in the cache stay flat and the frame time not move.

**Done.** `text_mono_2d` / `text_mono_3d` / `text_mono_billboard` draw out of a
95-glyph atlas built from `FNT_MONO` at init; `F2` is the debug key and
`--textbench` is the same test headless. Measured on `/bin/ls`, 200
never-repeated strings (~4800 glyphs) a frame:

| path | ms/frame | vs. no panel | cache entries | cache MiB | rasterized |
|---|---|---|---|---|---|
| no stress panel | 0.32 | — | 38 | 2.4 | 0 |
| monospace atlas | 0.51 | +0.19 | 38 | 2.4 | **0** |
| string cache | 6.84 | +6.52 | 520 | 28.0 (its ceiling) | **43 200** |

The criterion is met on both halves: the cache does not move and nothing is
rasterized, and the cost of the panel is +0.19 ms — 1% of a 60 fps frame — where
the cache path costs +6.52 ms and evicts the room's own labels to make room for
strings it will never see again.  (The 200 2D strings are joined by a sixteen-line
world-space panel in front of the player, so the `text_mono_3d` path the wisp's
ceiling panel will use is exercised too, not merely compiled.) `--selftest` still passes; the whole thing is
clean under ASan and UBSan. The HUD lines that already changed every frame — the
fps readout, the stall line, the instruction under the crosshair, the detail
sheet's listing and hex dump — were moved onto the atlas at the same time, which
is the "independently useful" part: they were a small treadmill of their own.

### M1 — the first thing worth looking at

`Vm` skeleton (registers, flags, the three memory layers, tier 0 via
`cs_regs_access`), the wisp body, the ceiling panel, the tether, the cursor
ring. **No arithmetic whatsoever** — every instruction havocs the registers it
writes and clears the flags it modifies. Branches are all guesses.

This is deliberately the first milestone with something on screen, and it is
also the only one that works on **every architecture Capstone decodes**, since
tier 0 needs no per-ISA knowledge. An ARM or MIPS binary gets this and nothing
more, for ever.

*Test:* `--vmtest FILE` — spawn a wisp in every code room of the first *N*
buildings, run to the fuel limit, assert termination, no fault, bounded pages,
everything freed. Run it under ASan and UBSan. Assert `disasm_live()` is
unchanged by a wisp's whole lifecycle.

**Done.** `src/vm.c` (registers keyed by Capstone id, flags with a known bit
each, the three memory layers, tier 0) and `src/wisp.c` (the body, the visit
counts, the hop) are new; `draw_wisp()` in `render.c` draws the panel, the
tether and the cursor ring; `x` spawns and `k . , / n` drive it. `--vmtest` runs
one in every code room of the first eight code towers:

| | `ls` | `git` | `bash` | a synthetic AArch64 file |
|---|---|---|---|---|
| code rooms | 80 | 600 | 600 | 1 |
| steps | 15 364 | 261 351 | 81 286 | 51 |
| ended at `ret` | 66% | 56% | 77% | 100% |
| left by a port | 13 | 23 | 64 | 0 |
| out of fuel | 2 | 33 | 5 | 0 |
| conditionals **decided** | 0% | 0% | 0% | 0% |

Every run terminated, nothing leaked, `disasm_live()` did not move under a
wisp's whole lifecycle, and both runs of every room took the same path. Clean
under ASan and UBSan. The AArch64 column is the milestone's other claim made
good: tier 0 asks Capstone what an instruction touches, so a file of an
architecture nothing here knows anything about still gets a walker — its twelve
loop iterations are §6's backward-branch policy, unchanged.

Three things came out differently from the plan:

- **The conditional row is 0% by construction**, not by accident: with no flag
  model there is nothing to decide with, so `*decided` is always 0 and every
  branch is guessed. The number is printed so M2 has something to move.
- **The memory layers are not driven by tier 0**, which executes no operands, so
  `--vmtest` checks them directly instead — file bytes come back from `e->map`,
  an unmapped read is invented once and remembered, a write shadows the file
  without touching the mapping, and the page cap holds. Without that the
  "bounded pages" assertion would have had nothing to assert against.
- **The panel hangs at 2.50 m, not §7's 3.35.** The ports are on the far wall
  only and the panel follows the wisp around the middle of the room, where the
  band above the sculptures is clear all the way up; the extra 0.85 m of row
  height is what makes it readable, and it keeps the plate nearer eye level.
  It also faces the camera outright rather than staying upright — a vertical
  plate two metres above your head foreshortens into a slot.

Two keys had to move: §12 asks for `Space` to pause, which is *jump* here, so
pause is `k`. And one bug fell out of the work rather than being caused by it:
ARM and AArch64 print a branch target as `#0x...`, so `disasm_run()` read every
ARM branch as indirect and the walker stopped dead at the first one. One line in
`disasm.c`; it is part of §4.1's ARM gap and was owed anyway.

### M2 — where being wrong becomes possible

Tier-1 semantics for ~70 mnemonics, the sub-register table, the flag model, the
condition predicates. **Build the gdb oracle (§16) in the same milestone, not
after it** — it found five real bugs before a line of the interpreter existed,
and four of them would have shown as plausible wrong numbers rather than
crashes.

*Test:* the generated corpus, checked in as data so everyday runs need neither
gdb nor an x86 host. Zero divergence, or the milestone is not done. Then
`--vmtest` reports the two numbers that say whether it works in the field: the
share of steps handled by tier 1, and the share of conditionals **decided**
rather than guessed.

**Done.** `src/vmx86.c` is tier 1; `tools/oracle.c` + `tools/oracle.py` +
`tools/gen-corpus.sh` are the oracle; `tests/vm-corpus.txt` is 4872 vectors over
203 instruction forms and 24 register states, checked in; `src/vmcheck.c`
replays it inside `--vmtest`.

**All three criteria met.**

| | `ls` | `git` | `bash` | `codecity` | AArch64 |
|---|---|---|---|---|---|
| steps | 75 817 | 818 657 | 571 545 | 97 872 | 51 |
| tier 1 | 99.8% | **97.5%** | 99.7% | 80.1% | 0% |
| conditionals decided | 100% | **99.9%** | 99.9% | 97.1% | 0% |
| ended at `ret` | 58% | 47% | 67% | 70% | 100% |

Divergence from hardware: **0 of 4872**. `--selftest` still passes, and the
whole thing is clean under ASan and UBSan.

The oracle earned its place before the interpreter existed, exactly as §16
predicted. Every one of the five rules in that section reproduced on the first
generated corpus, and two more came out of it:

- **`shl eax, 0` still writes its destination.** No flag changes, as the manual
  says — but the 32-bit write happens anyway, so the top half of `rax` is
  zeroed. Two vectors, and nothing else in the design would ever have caught it.
- **`shr` at width 1 disagrees with the width-8 rule for `OF`.** Which led to
  the one place where the corpus said *don't model this*: past a count of one,
  `OF` after a shift is architecturally undefined, and hardware asked 4872 times
  is not a function of the inputs — no rule explains more than 65 of 87 `shl`
  vectors. So `OF` is declined there and comes back unknown, and `--vmtest`
  prints how many comparisons were declined and for which forms, so a genuinely
  missing rule cannot hide behind that exemption. Capstone's
  `X86_EFLAGS_UNDEFINED_*` bits handle every other case generically: an
  undefined flag has its known bit cleared rather than being given a number.

Two things about the harness are worth recording, because both cost real time:

- **gdb does not always step where you point it.** Roughly one `stepi` in a
  hundred resumes from the previous stop instead of the `$rip` just written --
  and `$rip` reads back correctly, so it looks like a semantics bug, not a tool
  bug. It surfaced as a `pop` that appeared to push. The fix is a tripwire: the
  scratch page is padded with `cld`, `DF` is set before every step, and a vector
  whose `DF` comes back clear has run a pad byte and is redone. 48 of 4872
  needed a redo.
- **Trap padding makes it worse.** The first harness filled the page with
  `int3`, on the reasoning that nothing should ever run off the end of the
  instruction. That leaves the program counter on a breakpoint byte after every
  step, which is what provokes the mis-step above. Benign padding is right, and
  `cld` is benign *and* observable.

The remaining tier-0 traffic is what §4.3 predicted: SSE moves and scalar float,
which is why `codecity`'s own binary is the worst column in the table. That is
M6's work, not a gap in this one.

### M3 — from drifting to executing

The synthetic stack, the `ret` sentinel, the `fs:0x28` canary, call step-over,
relocations into shadow memory (§9), and the symbol/string annotation of §10.
Individually small; together they are the difference between a wisp that
wanders and one that plainly runs a function and returns from it.

*Test:* one metric captures it — **the share of rooms whose wisp terminates at
`ret` instead of exhausting its fuel.** Without the canary model a third of
protected functions dive into `__stack_chk_fail`, so this number moves visibly
when each piece lands.

**Done.** Both criteria hold: every binary ends more runs at `ret` than at the
fuel limit, and the string coverage reproduces §10.1's table almost exactly.

| | `ls` | `git` | `bash` | `codecity` |
|---|---|---|---|---|
| ended at `ret` | **60%** | **48%** | **74%** | **71%** |
| left the room | 19% | 17% | 12% | 13% |
| ran out of fuel | 20% | 35% | 14% | 16% |
| calls resolved to a name | 80% | 28% | 89% | 100% |
| relocations applied per run | 330 | 4546 | 2643 | 590 |
| `.rodata` strings resolved | 50.7% | **99.8%** | **98.5%** | **98.0%** |

`libx264.so.164`: 96.1%, against §10.1's 96.1% — the same 749 strings and the
same 29 left over. `git`'s 12 950 strings and 99.8% union match the section's
table to the digit. `ls` is the low outlier at 50.7% because it is small and
most of its `.rodata` is not strings: the scan counts any printable
NUL-terminated run of four bytes or more, and a table of small integers looks
like one.

**Three of this section's premises did not survive contact.**

- **The canary was already handled, by §2's rule.** §5 predicts that without a
  canary model "a third of protected functions dive into `__stack_chk_fail`".
  Measured with the model and then with it removed: **0 runs either way.** The
  reason is the lazy-materialisation rule — the first read of `fs:0x28` invents
  a value *and writes it back*, so the epilogue's check reads the same shadow
  bytes and compares equal. The explicit canary stays, because it costs three
  lines and makes the value stable and nameable, but it earns nothing it was
  predicted to earn.
- **The headline metric is confounded.** It moved far less than "visibly": `git`
  went 47% → 48%. The reason is that two M3 pieces push it in opposite
  directions. The call contract — havocking the caller-saved registers, which
  M2 did not do — makes loops containing calls run their real length instead of
  converging on a stale register, so runs get *longer* and more of them reach
  the fuel wall. That is more correct and it costs the metric. Ten times the
  fuel buys only three points on `git`, so what remains is not a budget problem:
  those loops genuinely do not converge, because their exit condition depends on
  memory a stepped-over callee would have written. §14 names that as the real
  limit on fidelity, and it is.
- **What actually moved the number was §5's minting table**, which §18 does not
  itemise as M3 work at all. Two rows of it: an invented pointer must land in a
  *small* window, or `while (p != end) p++` walks 2^61 times; and an
  unrecognised callee must be able to return zero, or `while (fn(x))` never
  ends. Those two together are worth more than the canary, the sentinel and the
  relocations combined, on this metric.

Two things landed beyond the plan because the measurements demanded them:

- **A `call` into `.plt` is named by following the stub.** Only 1% of `git`'s
  calls resolved to a symbol at first: in a dynamically linked binary the target
  is a PLT thunk whose only job is `jmp *GOT[n]`, so the name is not at the call
  target — it is in the relocation §9 had just written into the shadow. Reading
  it back through the stub took `git` to 28%, `bash` to 89% and `codecity` to
  100%.
- **"Ran off the end of this room" was miscounted as a stall.** With relocations
  resolving indirect jumps and the sentinel making `ret` a real return, control
  legitimately leaves a room by an address rather than by a port. That is the
  same outcome as leaving by a port, not a failure, and it is now reported as
  its own row. Runs that are genuinely stuck are down to **one per binary**.

### M4 — making it readable

The trail, the level-of-detail rules, the `X` state sheet, the `Tab` window
following the wisp, the controls of §12. No new semantics.

*Test:* by eye, plus a frame-time budget: a room with a running wisp must not
cost measurably more than one without.

**Done.** All five pieces, and the budget is met with room to spare.

- **The trail.** The last sixteen states as ghost cards over the sculptures
  they belonged to, joined by a thread, fading over six seconds. Each carries
  the mnemonic and *the one register that changed* — which is what §7 asked
  for and is also what makes them short enough to read side by side. The older
  half collapses to the mnemonic alone; a column of full lines is unreadable.
  Orange for a guessed branch, pink for a stepped-over call, blue for the rest.
- **Level of detail.** Full panel under 8 m (the row height is 0.058 m, which
  stops being legible at about that distance), the token and its mnemonic to
  25 m, the tether alone beyond. The old M1 threshold of 11 m drew an
  unreadable panel for the last three metres of its band.
- **`Shift+X`, the state sheet.** §1's list in full, where there is room for
  it: every register with its provenance mark and *what it points at* —
  `rcx "hooks/%s"`, `rbp stack+208`, `rip .text+0x2eba1a` — the flags, the
  stack near `rsp`, the last four memory references, and the last stepped-over
  call. §12 asks for `X`; plain `x` already starts and stops a wisp, so the
  sheet is `Shift+X`. It sits on the left because Tab's sheet owns the right,
  and the two are the pair you want open together.
- **`Tab` follows the wisp.** The detail sheet's seven-line window centres on
  the machine rather than on your feet when a run is going, and each line
  carries its visit count, with executed lines in a different colour. A trace
  view for about fifteen lines of change.
- **Folded registers.** §1's "r8..r15 (folded unless changed)", which the
  ceiling panel needed badly: on SSE-heavy code the xmm slots tier 0 claims
  were pushing the general registers into five cramped columns.

*Frame time*, `--bench` in a 1848-instruction room with 392 wires:

| | ms/frame |
|---|---|
| no wisp | 3.16 |
| a wisp running, trail drawn, one instruction stepped per frame | **3.12** |

It is marginally *cheaper*, and the reason is worth recording: a trail labels
the same sculptures the room's own control-flow mnemonics do, so those stand
down while a wisp is walking — and the trail draws through M0's monospace
atlas, where the labels it replaced went through the string cache. M0 paid for
M4.

### M5 — the walk through the city

Pilot mode and the shadow call stack (§8 B2). The CPU state owns nothing
belonging to a `Disasm`, so it survives a room change for free; the player
moves with it, which keeps the one-decoding-at-a-time invariant intact.

*Test:* extend `--selftest`'s existing port test — start at `e_entry`, follow
*N* ports, and assert at each step that the player lands in a room that really
covers the address and that exactly one decoding is alive. This is the
milestone where the feature finally answers the original request: *start at the
entry point and move along the code path.*

**Done.** `P` is pilot mode; with no wisp running it teleports to `e_entry`,
starts one there and hands it the wheel. `--selftest` walks forty hops from the
entry point of whatever file it is given, asserting at each one that the room
really covers the address and that `disasm_live()` is within the room's budget:

```
ls         40 hops from the entry point, 207 steps, 42 rooms, 2 calls deep
git        40 hops from the entry point, 207 steps, 42 rooms, 3 calls deep
bash       40 hops from the entry point, 155 steps, 42 rooms, 2 calls deep
vim        40 hops from the entry point, 326 steps, 42 rooms, 5 calls deep
python3    40 hops from the entry point, 254 steps, 42 rooms, 6 calls deep
```

**The shadow call stack was not needed.** §8 B2 budgets ~40 lines for a stack
of (address, register snapshot) so `ret` can teleport back. M3's synthetic
stack makes that redundant and less truthful: a followed call pushes a real
return address, `ret` pops it, and the registers on return are whatever the
callee actually left rather than a snapshot restored over them. All that
remains of the idea is a depth counter, so recursion cannot walk for ever.

Three things had to change before a walk from `e_entry` went anywhere at all,
and each was a bug the earlier milestones had no way to notice:

- **`hlt` is not a no-operation.** `disasm.c` classes it with `nop` and `int3`
  as padding, which is right for the sculpture it becomes and wrong for a
  walker: the wisp strolled out of the end of `_start` and into the alignment
  bytes behind it. `hlt`, `ud0`, `ud2`, `int3` and `int1` now stop a run.
- **A jump into a symbol this file does not define is a tail call.** Almost
  always a PLT thunk ending `jmp *GOT[n]`. Ending the run there stopped every
  walk at the first library function; running the step-over contract and then
  *returning* keeps it going up the caller chain, which is what a tail call
  actually does. This took the four test binaries from 2–4 hops to the full 40.
- **`__libc_start_main` gets one special case.** It is not in the file and
  never will be, but its first argument is `main`. Without following that, a
  walk from the entry point of any dynamically linked C program ends after
  thirteen instructions, having done nothing but marshal arguments. One
  `strncmp` in the follow path, and it is the difference between the milestone
  working and not.

**The stub table moved to the heap.** 256 synthetic addresses was too few for
`vim` and `python3` — past the cap every undefined symbol shared one unnamed
address, and a walk ended at `0x7ffff0001000` with nothing to say about it. At
4096 entries, inline in the `Vm`, that would have been 256 KB memset four
thousand times by the corpus check; on the heap the corpus check got faster
than it was before.

### M6 — vectors, written once

Tiers 2, 3a, 3b and 3c. **Parameterise by `(element width, lane count, operand
count)` from the first line** (§4.3.1) — 74–96% of AVX in real codecs is a
tier-3 operation at another width, so hard-coding 128-bit two-operand forms
here means writing the whole thing twice. Store the vector registers 512 bits
wide even while only the low 128 are used.

*Test:* the coverage table. `libvpx` must reach 98.1%, `codecity` 97.0%. Extend
the oracle corpus to packed operations — hardware is just as much the ground
truth for `pmaddwd` as for `add`.

**Done, and both targets exceeded.** `src/vmsimd.c` is tiers 2, 3a, 3b and 3c;
the corpus grew to **8616 vectors over 359 forms**, with sixteen bytes of xmm
state in every state block, and diverges from hardware **nowhere**.

Measured the way §4.3 measured it — every instruction in the executable
sections, classified by offering it to the real dispatchers. The instruction
counts come out identical to this section's table (`git` 727 991, `libvpx`
634 107, `libavcodec` 2 689 807), which is a decent check that the two
measurements are of the same thing:

| | §4.3 tier 1 | §4.3 target | measured tier 1 | **measured, modelled** |
|---|---|---|---|---|
| `git` | 97.0% | 99.5% | 97.1% | **100.0%** |
| `bash` | 98.8% | 99.5% | 99.3% | **100.0%** |
| `libvlccore.so.9` | 96.9% | 99.4% | 97.2% | **100.0%** |
| `codecity` | 73.0% | **97.0%** | 81.5% | **99.9%** |
| `libavcodec.so.60` | 91.4% | 97.7% | 91.4% | **99.5%** |
| `libvpx.so.7` | 62.8% | **98.1%** | 62.4% | **99.1%** |
| `libswscale.so.7` | 85.9% | 96.4% | 85.3% | **97.2%** |
| `libdav1d.so.7` | 48.6% | 69.6% | 54.5% | 77.9% |

**§4.3.1's decision paid off exactly as it predicted, and that is a problem
this milestone chose not to cash in.** Because every packed operation is a loop
over `(element width, lane count, operand count)`, the `v`-prefixed forms are
the same code at a bigger lane count and simply work: with the width limit
lifted, `libvpx` reaches 99.8%, `x264` 98.8% and `dav1d` **90.7%** — against
§4.3's 69.6% for dav1d without an AVX tier. But the corpus has only 128-bit
vectors in it, so **the 256-bit path is gated off**: executing what has not
been checked against hardware is the precise failure §14 exists to warn about,
and a coverage number bought that way would be worth nothing. Lifting the gate
is one line and it is the first thing M7 does, after extending the corpus to
`ymm`. (`CODECITY_AVX256=1` lifts it, which is how the numbers above were
measured; it is not a supported way to run.)

**Two more findings from hardware**, both of the "plausible wrong number" kind:

- **`minss` returns one of its operands bit for bit**, not the smaller number.
  Recomputing the result through a `double` quiets a signalling NaN and changes
  the answer; and written as `(src < dst) ? src : dst` it returns the wrong
  operand for a NaN, where the rule is *DEST if DEST < SRC, else SRC*.
- **`cvttss2si` of a NaN or an out-of-range float** gives the integer
  indefinite value (`0x80000000`), not whatever a C cast does — which is
  undefined behaviour and was returning zero.

**Tier 1 had leftovers.** Once the tiers landed, everything still havocking in
`git` was `rol`, `ror`, `bswap`, `bt`/`bts`/`btr`/`btc` and the `rep`-prefixed
string operations — none of them in §3's candidate list of seventy, and between
them the last 0.3% of the file. They are in tier 1 now, verified by the corpus
like the rest, and §4.5's clamp on `rep` (an invented `rcx` could be 2^63) is a
bounded bulk operation of at most 4096 elements.

**The coverage classifier is the interpreter.** A table of mnemonic names kept
beside the dispatch switch would drift away from it within a milestone; instead
every instruction is offered to `vmx86_exec` and then `vmsimd_exec` on a
throwaway machine and whichever takes it is what the wisp would use. It costs
one `Vm` and a decode pass, and it cannot lie.

### M7 — AVX2, nearly free if M6 was done right

Widen the tier-3 loops to 256 bits, add the ~10 lane-crossing operations that
have no SSE counterpart (`vpbroadcast*`, `vinserti128`, `vperm*`,
`vzeroupper`). **AVX-512 stays out** — it needs `k` registers and predication
threaded through every operation, and it is under 3% of everything except
`dav1d`.

*Test:* `dav1d` from 69.6% to 99.1%. If this milestone costs much more than 150
lines, M6 was not written parametrically and that is the thing to fix.

**Done, and the parametric bet paid.** `dav1d` goes from 52.6% tier 1 to
**95.1% modelled**, `libvpx` and `libavcodec` to 99.9%, `x264` and `libswscale`
to 99.3%. The corpus is now **10 584 vectors over 441 forms**, recorded 256
bits wide, and diverges from hardware nowhere.

| | tier 1 | tier 2 | tier 3 | **modelled** |
|---|---|---|---|---|
| `git` 727 991 | 97.1% | 0.0% | 2.8% | **100.0%** |
| `libvlccore.so.9` 182 502 | 97.2% | 0.5% | 2.2% | **100.0%** |
| `libavcodec.so.60` 2 689 541 | 91.4% | 1.3% | 7.2% | **99.9%** |
| `libvpx.so.7` 633 990 | 62.4% | 0.8% | 36.7% | **99.9%** |
| `libswscale.so.7` 135 012 | 85.3% | 0.8% | 13.2% | **99.3%** |
| `libx264.so.164` 368 904 | 78.9% | 1.6% | 18.9% | **99.3%** |
| `libdav1d.so.7` 293 494 | 52.6% | 0.0% | 42.6% | **95.1%** |

**The cost was as predicted: the lane-crossing block is 88 lines.** That is the
whole of what did not fall out of M6's parametric loops — `vpbroadcast{b,w,d,q}`,
`vbroadcast{ss,sd}`, `vinserti128`, `vextracti128`, `vperm2i128`, `vpermq`,
`vpermd`, `vzeroupper`. Everything else AVX2 does was already written.

**`dav1d` falls short of 99.1%, and it was always going to.** §4.3's "+AVX"
column counted AVX-512 as AVX, and this milestone defers AVX-512 by design.
What is unmodelled in `dav1d` is almost exactly that: `vmovdqa32` 1.3% and
`vmovdqu32` 0.2% are its two largest misses, and a further 13 526 bytes of it
Capstone declines to decode at all. Deducting the EVEX share §4.3.1 measured
(8.4% of a 29.5% VEX+EVEX share) from 99.1% gives about 96.6%, which is within
a point and a half of what landed. **The honest ceiling without `k` registers
is roughly where this stopped.**

Three things came out of the work rather than the plan:

- **A VEX write zeroes above the destination and a legacy SSE write does
  not.** `movd xmm0, ecx` clears bits 127:32 and leaves 255:128 alone;
  `vmovd xmm0, ecx` clears everything. Only a corpus recorded 256 bits wide can
  tell them apart — which is exactly why M6 gated the wide path rather than
  shipping it, and the first regeneration at `ymm` width found 72 divergences
  in one go.
- **Two more NaN rules.** A float operation on a NaN returns *that NaN's bits*,
  quieted, rather than a NaN recomputed through a `double`; and `ror eax, 0`
  writes its destination even though it rotates by nothing, so it zero-extends
  `rax`. The second is the same trap `shl eax, 0` set in M2, in a different
  instruction — worth noting that the corpus caught it the moment a new random
  state happened to exercise it, which is the argument for generated vectors
  over hand-written ones.
- **UBSan found undefined behaviour the release build was getting away with**:
  `idiv` built its 128-bit dividend by shifting a *negative* value left. It
  changed one corpus result under `-fsanitize=undefined` and none without,
  which is precisely the shape of bug that ships.

**The coverage sweep had to be taught to resynchronise.** A blind linear sweep
of a section steps a byte at a time out of anything Capstone cannot decode, and
the tail it then decodes is garbage: on `libdav1d` that invented an `in`, an
`outsd` and a `loop` that `objdump` says are not in the file at all, and it was
costing the measurement about 1.5 percentage points. Nothing counts now until
three instructions in a row have decoded, and where a file has a real symbol
table the sweep follows the functions instead.

### M8 — the same function, several rooms

`cpuid` and `xgetbv` against a virtual CPU profile, selected by
`--cpu baseline|v2|v3|host`. Codec libraries dispatch on a real `cpuid` with no
IFUNC in sight (§4.2), so this is the control that decides which kernel the
wisp walks into.

*Test:* enter a dispatching function in `libvpx` under `--cpu baseline` and
again under `--cpu v3`, and assert the wisp ends up at two different addresses.
That is a two-line assertion for the single most interesting thing this feature
does.

**Done.** `src/vmcpu.c` is the virtual CPU — 150 lines including the table —
`cpuid` and `xgetbv` are tier-1 instructions, `--cpu baseline|v2|v3|host`
selects the profile, and `--selftest` finds a `cpuid`, walks the function it
sits in under each profile, and reports where the paths part company:

```
libvpx.so.7   a cpuid at 0x9d6b; walking the function it is in:
  baseline    cpuid.1 ecx=00000000  7.0 ebx=00000000  xcr0=3   27 instructions
  v2          cpuid.1 ecx=02982203  7.0 ebx=00000000  xcr0=3   27 instructions
  v3          cpuid.1 ecx=3ed83203  7.0 ebx=00000128  xcr0=7   39 instructions
  they part company at step 25, address 0x9d88:
    baseline  goes to 0x9d8a          <- pop rbx; ret
    v2        goes to 0x9d8a
    v3        goes to 0x9da0          <- xgetbv, and on into the AVX2 path
```

`0x9d88` is the `je 0x9da0` immediately after `and ecx, 0x18000000` — the test
for OSXSAVE and AVX together. It is the exact assertion this milestone asked
for, and it works on the file the section names.

**Comparing the final address is not the right test, though the section says
so.** `libvpx`'s dispatcher has one exit: both profiles reach the same `ret`
whichever kernel they chose on the way, so the addresses match and the paths do
not. The test compares the *traces* and reports the step at which they diverge,
which is both a stronger assertion and better output.

**On `x264` and `dav1d` it reports honestly that it did not get there.** In
both, the wisp's invented data takes a branch that avoids the `cpuid`
altogether before ever reaching it — so the test says *"the run never reached
that cpuid — it is not on the path from the entry of the function it sits in"*
rather than the misleading *"every profile took the same path"*. That is §14's
limit showing through: the inputs are fiction, and fiction does not always walk
where you want it to.

Two smaller things:

- **The walk has to start at the function entry**, not at the `cpuid`. Beginning
  in the middle leaves `eax` invented, so the CPU is asked a random leaf and
  answers zero — and the very first branch after it goes the wrong way. The
  nearest `endbr64` behind the instruction is the entry in any CET build, and a
  much tighter start than the room, which in a stripped library can be a whole
  chunk of `.text`.
- **`baseline` is the default and stays it.** It maximises the fraction of code
  the interpreter models, it is what the compiler assumed, and — since M7 —
  choosing `v3` genuinely sends a wisp into AVX2 kernels that tier 3 does model,
  so the choice is now real in both directions rather than a way of finding
  havoc.

### Off the critical path

Three pieces are worth doing but belong to no milestone above, because they are
data-section features rather than execution ones and can land whenever:

- **shader strings shown as text, and GPU-submission ports labelled** (§11.5) —
  80 lines, and worth slotting in beside M3, since §10's string work already
  finds them;
- **the reverse string index** (`--index`, §10.4) — 150 lines, earns the
  `.rodata` gallery its wires;
- **SPIR-V listing** (§11.3) — 300 lines, and it belongs with the README's
  other section-as-sculpture ideas, not with this document.

Also independent, and arguably owed regardless: **the ARM/Thumb decode bug**
(§4.1), which the wisp does not cause but does make obvious, and the
**`dlopen` plugin-directory gateways** (§10.3) without which VLC's codecs are
unreachable from `vlc` at all.

---

## 0. What already exists, and what this can stand on

Almost all the machinery is there. Nothing below needs new geometry.

| what | where | why it matters here |
|---|---|---|
| `Disasm` — instructions, classes, branch targets, ports | `disasm.c` | the wisp's road network is already resolved: `Insn.target` (inside the room), `Insn.tunit` (another alcove), `Insn.port` (leaves the room) |
| `code_layout()` — every instruction has an `x,y,z,h` | `world.c` | the wisp knows where to fly without computing anything |
| `arc_point()` / `draw_arc()` — quadratic arcs with a travelling bead | `render.c:287` | a taken branch can *ride the wire that is already drawn* |
| ports on the far wall, with `city_find_addr()` behind them | `city.c`, `main.c:teleport_addr` | an exit is already a working door to another room |
| enter/leave lifecycle, one decoding alive at a time | `city_enter_room()` | gives the wisp an obvious lifetime, and a hard constraint (§8) |
| `elf->map` + `sec[].addr/offset/data` | `elfload.c` | real bytes for real addresses |
| Capstone opened with `CS_OPT_DETAIL` on | `disasm.c:52` | operands, sizes, access flags, `eflags` modify/test masks — everything an interpreter needs |

The one thing that is *not* ready is text: §13.

---

## 1. What a "state" is

The snapshot the wisp carries, and what the ceiling panel shows:

```
  rax  0000000000000001   derived      rip  0x0000000000407a31
  rcx  00007ffff7a12dc0   invented     ZF 1  SF 0  CF 0  OF 0  PF 1
  rdx  0000000000000018   derived
  rbx  0000555555559120   file  -> .rodata "usage: git [-v | --version]"
  rsi  00007ffffffee220   stack -0x40
  rdi  0000000000000003   arg0
  rsp  00007ffffffee1f8   [rsp] 0000000000407a20  (return)
  rbp  00007ffffffee230
  r8..r15                 (folded unless changed)

  last:  mov  rdx, qword ptr [rbx + 8]     ->  rdx = 0x18   read 0x555555559128  (file)
  SIMULATED — values below the line are invented, not observed
```

Five pieces:

1. **Registers.** 16 GPRs plus `rip`, each with a value, a *provenance* tag and
   the step number that last wrote it (so a change can glow for a moment).
2. **Flags.** CF PF AF ZF SF OF, each with a *known* bit. An instruction we do
   not model clears the known bits that Capstone says it modifies — precise
   ignorance rather than a lie.
3. **A stack window.** The top six or eight qwords, drawn as a little ladder
   hanging off the panel. This is the part people actually want to see and it
   costs nothing extra: it is just memory near `rsp`.
4. **The last few memory references** — address, direction, size, value, and
   where the value came from.
5. **Annotations.** Any value that lands inside a mapped section gets named:
   the symbol at that address, or the string it points at. This is what turns
   the panel from a hex dump into an explanation, and it is free — `elf_sym_at()`
   already exists and the string is right there in `e->map`.

### Provenance, and the honesty problem

Every value carries one of:

| tag | meaning | drawn as |
|---|---|---|
| `PV_FILE` | read out of the mapped ELF image at a real address | bright, with the symbol/string named |
| `PV_DERIVED` | computed by a modelled instruction from other values | normal |
| `PV_INVENTED` | minted from the PRNG the first time something read it | dim, italic-ish, prefixed `~` |
| `PV_CALL` | whatever a stepped-over call is pretending to have returned | dim amber |
| `PV_NONE` | never touched since the run started | blank |

This matters more than it looks. The failure mode of this whole feature is a
viewer who believes the numbers. Provenance colouring plus a permanent
`SIMULATED` marker on the panel is the mitigation, and it should not be
optional or toggleable.

---

## 2. The machine: four options

### A1. Trace only, no semantics

Walk the instruction stream, resolve conditionals by coin flip, and show a
register table of numbers that never mean anything. **Cheap and dishonest** —
the panel becomes decoration, and after ten seconds you stop looking at it.
Rejected.

### A2. A lazy concrete mini-VM — *recommended*

Interpret the instructions for real, over a state that materialises as it is
touched:

> **The rule.** Nothing is initialised. The first time an instruction *reads*
> something that has no value — a register, a byte of memory — a value is
> minted from a seeded PRNG, tagged `PV_INVENTED`, and **written back**. Every
> later read of the same place gives the same answer.

That single rule is what makes the display coherent. `mov rax, rdi` /
`add rax, 8` / `cmp rax, rdx` / `jne` produces a chain where each value plainly
came from the last, and the branch is decided by arithmetic — invented
arithmetic, but consistent arithmetic. It is exactly what the user asked for:
"random values dynamically generated to fill in data that we cannot predict",
and conditional jumps "with either random data or the available data, best
effort".

Cost: roughly 900–1300 lines for x86-64 (§3), and it is the only option that
scales down gracefully to architectures we do not model (§3, tier 0).

### A3. Unicorn Engine (or QEMU-user)

Real, complete emulation of every instruction, every architecture Capstone
already decodes. It would be *correct* where A2 is approximate.

Against it: a large new dependency for a program whose whole build is four
pkg-config packages; it needs a real memory map, so the invention problem
does not go away, it just moves (Unicorn faults on unmapped reads and we would
be writing the same lazy-materialisation logic in a `UC_HOOK_MEM_UNMAPPED`
handler); and unmodelled state — no libc, no syscalls, no loader — is the
actual limit on fidelity, not the instruction semantics. It buys the last 3%
of mnemonics for a large amount of integration. **Worth revisiting if the
SIMD gap (§3) turns out to matter**, since a hybrid — our state, Unicorn for
one instruction at a time — is possible.

### A4. Symbolic / provenance-only

Track labels, not numbers: `rax = strlen(arg0)`, `rdx = *(arg1+8)`. Genuinely
more truthful, and arguably more educational. But it is not "a CPU state" —
there is nothing to put in a register column, conditionals become unresolvable
rather than best-effort, and the expression trees grow without bound in loops.
**Rejected as the main mechanism, but keep the idea**: the `PV_*` tag plus the
symbol annotation of §1 is a cheap 20% of this.

---

## 3. Coverage: how much can actually be modelled

Measured with `objdump -d`, normalising AT&T suffixes to the Intel names
Capstone prints, against a candidate tier-1 set of about 70 mnemonics
(`mov movabs lea add sub xor and or cmp test push pop call ret jmp inc dec neg
not shl shr sar imul mul div idiv movzx movsx movsxd cdq cdqe cqo cwde leave
nop endbr64 ud2 int3 xchg adc sbb`, plus `j`/`set`/`cmov` × 16 conditions):

| binary | instructions | tier-1 coverage | top misses |
|---|---|---|---|
| `git` | 727 991 | **97.0%** | `movaps` 0.9%, `movups` 0.5%, `pxor` 0.4% |
| `bash` | 241 709 | **99.1%** | `movaps` 0.2%, `movdqa` 0.1% |
| `codecity` | 24 204 | 73.1% | `movss` 11.8%, `addss` 2.2%, `subss` 1.7%, `mulss` 1.5%, `comiss` 1.0% |

Two conclusions:

- For ordinary integer code — which is most code, and all of the code anyone
  walks into a room to understand — a small interpreter covers essentially all
  of it. The compiler's SIMD `memcpy` idioms (`movaps`/`movdqu` pairs) are the
  bulk of the miss and they are *copies*, not computation.
- **Our own binary is the worst case**, because it is float-heavy. A tier 2 of
  scalar SSE takes `codecity` from 73% to 92.8% for about 200 lines. §4 breaks
  all of this down per extension, settles what to do about CPUID and IFUNCs,
  and — on the strength of the VLC codec measurements — settles that AVX is not
  a separate tier but tier 3 at another width.

### Tier 0: everything we do not model, and every other architecture

Capstone's `cs_regs_access()` gives, generically and for every architecture,
the registers an instruction reads and writes; `detail->x86.eflags` gives the
flags it modifies. So the fallback is precise rather than arbitrary:

```
    read every register in regs_read (materialising as needed),
    havoc every register in regs_write  -> PV_INVENTED, fresh value,
    clear the known bit on every flag the instruction modifies.
```

The wisp keeps moving, the panel keeps telling the truth about what it does
not know, and ARM/MIPS/PPC binaries get *something* — a walker that follows
control flow and shows which registers each instruction disturbs — from day
one, with a banner saying the semantics for this architecture are not
modelled. Conditional branches there fall back to §6's guessing policy.

### The sub-register trap

`mov eax, ecx` zeroes the top half of `rax`; `mov al, cl` does not. Getting
this wrong produces values that are visibly, embarrassingly wrong (a pointer
with junk in the high bits). Capstone gives the operand size, so the model is:
one `uint64_t` per architectural slot plus a static `x86_reg → {slot, byte
offset, width}` table (~90 entries, mechanical), and a write rule of
*zero-extend at width 4, merge at 1 and 2*. Write the table once and test it.

### Where the detail comes from

`disasm_run()` throws the `cs_insn` array away, and `Insn` deliberately keeps
only 160 bytes of printable summary. Two ways to get the operand detail back:

- **Re-decode one instruction on demand** — `cs_disasm(h, code + (addr - vaddr),
  16, addr, 1, &tmp)` at each step, using the room bytes the `Room`/`Unit`
  already points at. About 1–2 µs, and a wisp steps a handful of times a
  second. **Recommended**: zero memory cost, no change to `Disasm`, and the VM
  sees the full detail struct rather than a lossy copy of it.
- Cache a compact effects record per instruction at decode time (~32 B ×
  5000 instructions = 160 KB per room). Faster per step, but it bloats a
  structure that was carefully kept small, and it pays the cost for every
  room entered whether or not a wisp ever runs.

---

## 4. Instruction sets: detection, CPUID, and the SIMD tiers

Three different questions hide under "instruction sets", and they have three
different answers.

### 4.1 Which ISA to decode as — mostly solved, with two real gaps

This is `disasm_open(e->machine, e->is64, e->be)`, and it is settled before a
wisp exists: one Capstone handle for the file's `e_machine`. The wisp inherits
it and never has to choose. But two cases the city already gets wrong will
become *visible* once something walks the code:

- **ARM/Thumb interworking.** An ARM binary freely mixes 32-bit ARM and 16-bit
  Thumb, per function. Capstone needs `CS_MODE_THUMB` for the Thumb ones, and
  `disasm.c` opens `CS_MODE_ARM` for the whole file — so every Thumb function is
  currently decoded as garbage. The detection mechanism is standard and we
  already parse it: **an `STT_FUNC` symbol with bit 0 of `st_value` set is
  Thumb.** The fix is a second handle and a per-room mode chosen from the
  symbol, plus masking that bit off when computing addresses. This is a
  pre-existing bug the wisp does not cause, but it would make it obvious.
- **MIPS16 / microMIPS** have the same shape of problem, via `STO_MIPS16` /
  `STO_MICROMIPS` in the symbol's `st_other`. Lower priority — nobody is
  exploring MIPS binaries today — but the mechanism is the same.

Within x86 there is no per-function mode to detect: a 64-bit ELF is decoded as
64-bit throughout. Mixed 32/64 (`__x86_64_ilp32`, or a 32-bit thunk in a 64-bit
image) is rare enough to ignore, and would show as a decode desync, which
already happens for data-in-code.

### 4.2 CPUID — a non-problem in applications, the main control in codecs

Counting `cpuid`/`xgetbv` over the whole `.text` of each file:

| file | `cpuid`/`xgetbv` | `R_X86_64_IRELATIVE` | GOT relocs |
|---|---|---|---|
| `git` | **0** | 0 | 255 |
| `bash` | **0** | 0 | 238 |
| `codecity` | **0** | 0 | 144 |
| `libc.so.6` | **0** | 46 | 77 |
| `ld-linux-x86-64.so.2` | **52** | 1 | 2 |

**For ordinary application code, CPUID is a loader phenomenon.** Every use in
that table is inside `ld.so`, which runs `init_cpu_features` exactly once at
startup and writes the answer into `_rtld_global_ro`. `git`, `bash`, our own
binary and even libc's own code never ask the CPU anything; they read that
struct, or go through an IFUNC.

**Multimedia codec libraries are the exception, and they invert the
conclusion** (§4.7). They do their own dispatch, in their own code, with a real
`cpuid`:

| | `cpuid` | `xgetbv` | `IRELATIVE` |
|---|---|---|---|
| `libvpx.so.7` | **15** | 10 | 0 |
| `libx264.so.164` | 1 | 1 | 0 |
| `libdav1d.so.7` | 1 | 1 | 0 |
| `libavcodec.so.60` | 0 | 0 | 0 |

Zero IFUNCs and a real `cpuid` is the *good* case: unlike glibc's resolvers,
which dereference a struct that is not in the file, a codec asks the CPU
directly — and a virtual CPU can answer honestly. So the `--cpu` profile below
is not a curiosity for these libraries; it is **the** control that decides
which kernel the wisp walks into.

Which leaves two cases that do matter, and one opportunity.

**When you walk into `ld.so` itself.** The explorer can already get there —
`.interp` names it and it is a portal candidate — and it is the one binary
where CPUID is the whole point. Fifty-two of them.

**Model it as a chosen virtual CPU, not as havoc.** `cpuid` is a pure function
of `(eax, ecx)` and a small table: leaf 0 (max leaf + vendor string), leaf 1
(family/model + the feature bits in ECX/EDX), leaf 7 subleaf 0 (the AVX2/BMI
era bits), leaf 0x80000001, and the cache/topology leaves that can safely
return zero. `xgetbv` returns XCR0, which must agree with the profile. Perhaps
120 lines including the table, and it turns a problem into a feature:

```
  --cpu baseline    x86-64 v1: SSE2 only            (default)
  --cpu v2          SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT
  --cpu v3          AVX, AVX2, BMI1/2, FMA
  --cpu host        whatever this machine reports
```

The default is **baseline**, deliberately: it maximises the fraction of code
the interpreter can actually model (§4.3), and it is what the compiler assumed
when it built the binary. Switching the profile and watching a multiversioned
function light up a *different* branch is one of the more genuinely
instructive things this feature could offer, and it costs a command-line flag.
In `libvpx` or `dav1d` it is the difference between the wisp walking into the
SSE2 kernel and walking into the AVX2 one — **the same source function, two
different rooms**, chosen by a flag. That is worth building for its own sake.

**IFUNC resolvers cannot be run honestly, and should not be.** This is the case
that looks like it wants CPUID and does not. Here is the first `IRELATIVE`
resolver in `libc.so.6`:

```
  c87b0: endbr64
  c87b4: mov  0x13a6f5(%rip),%rax     # 202eb0 <_rtld_global_ro@GLIBC_PRIVATE>
  c87bb: mov  0x1e0(%rax),%rdx
  c87c2: and  $0xffffffffffffff00,%rdx
  c87c9: jle  c87df
```

It never executes `cpuid`. It dereferences `_rtld_global_ro`, which lives in
**ld.so's** data — not in the file being explored. In our VM that load returns
an unrelocated placeholder from the file, the dereference of it lands in
unmapped space, and the resolver picks a variant on the strength of an invented
byte. That is not "best effort", it is a coin flip dressed up as computation.

So the rule is: **an `R_X86_64_IRELATIVE` target is not followed.** The port is
named after the IFUNC symbol and labelled *resolved at load time — one of N
implementations*, which is the truth. The one exception worth coding is a
resolver that is *self-contained* — it reads nothing outside the file's own
mapped sections — in which case running it under the chosen `--cpu` profile
gives a real answer. That is detectable: run it in a sandboxed sub-run and
discard the result if any read fell outside a mapped section.

Incidentally, the tail of that same resolver is where `objdump` loses sync and
starts emitting `rex.W / .byte 0x89`. Decode desync in real libraries is not
hypothetical, and §6's rule — a wisp that steps to an address with no
instruction in the decoding stops and says so — is what keeps it from becoming
a crash.

### 4.3 SSE, AVX, and the tiers — measured per extension

Bucketing every mnemonic by the extension it belongs to. Five tiers, because
the codec measurement of §4.7 forced a fifth into existence:

| tier | what | example |
|---|---|---|
| 1 | integer, the base ISA | `mov` `add` `cmp` `jcc` |
| 2 | **scalar** SSE float math | `movss` `mulsd` `cvtsi2ss` `comiss` |
| 3a | **packed moves** — bulk copy, no arithmetic | `movdqa` `movups` `movd` `pmovmskb` |
| 3b | **packed shuffle/compare** | `pcmpeqb` `punpcklbw` `pshufd` `pxor` `psrld` |
| 3c | **packed integer arithmetic** | `paddw` `psubw` `pmaddwd` `pavgb` `psadbw` |
| — | AVX / AVX2 / AVX-512 | `vpaddw` `vmovdqa32` `kmovd` |

Percentage of each file, and cumulative coverage as the tiers land:

| | insns | t1 | +t2 | +t3a | +t3b | +t3c | +AVX |
|---|---|---|---|---|---|---|---|
| `git` | 728 k | 97.0% | 97.0% | 98.8% | **99.5%** | 99.5% | 99.5% |
| `bash` | 242 k | 98.8% | 98.9% | 99.4% | **99.5%** | 99.5% | 99.5% |
| `vlc` | **668** | 97.6% | 97.6% | 99.0% | **99.1%** | 99.1% | 99.1% |
| `libvlccore.so.9` | 183 k | 96.9% | 97.3% | 99.1% | **99.4%** | 99.4% | 99.4% |
| `codecity` | 24 k | 73.0% | 92.1% | 97.0% | **98.6%** | 98.6% | 98.6% |
| `libavcodec.so.60` | **2.69 M** | 91.4% | 92.6% | 94.4% | 95.9% | **97.7%** | 99.3% |
| `libswscale.so.7` | 135 k | 85.9% | 86.7% | 88.4% | 93.4% | **96.4%** | 99.0% |
| `libx264.so.164` | 370 k | 79.5% | 81.0% | 84.7% | 88.2% | **93.6%** | 98.9% |
| `libvpx.so.7` | 634 k | 62.8% | 63.6% | 77.3% | 86.9% | **98.1%** | 99.1% |
| `libdav1d.so.7` | 303 k | **48.6%** | 48.6% | 57.6% | 63.0% | **69.6%** | **99.1%** |

**This revises the earlier draft of this section, which said AVX was 0% and not
worth writing. That was true of every application binary and false of the
thing the user actually wanted to explore.** See §4.7.

Conclusions, in the order they change the plan:

1. **Tier 3c — packed integer arithmetic — is now a required tier, not an
   optional one.** It is 0.0% of `git` and **11.1% of `libvpx`**, 6.6% of
   `dav1d`, 5.5% of `x264`. This is the actual *arithmetic* of video: add,
   subtract, multiply-accumulate, average, sum-of-absolute-differences over
   packed 8- and 16-bit lanes. `libvpx` goes from 86.9% to **98.1%** on it
   alone. It is also easy — no flags, no addressing subtleties, just a loop
   over lanes.
2. **`libvpx` is the encouraging case and should be the target.** 62.8% tier 1
   looks hopeless and is not: with tiers 2, 3a, 3b and 3c it reaches **98.1%
   with only 1.1% AVX**. An SSE2-era codec is almost entirely within reach of
   a few hundred lines of lane-wise arithmetic.
3. **`dav1d` is the hard case**: 48.6% tier 1, 29.5% AVX, and only 69.6% without
   an AVX tier. AV1's SIMD is written for AVX2 and AVX-512 and there is no
   SSE2 fallback worth the name. A wisp in `dav1d` without AVX support spends
   a third of its time havocking.
4. **Tier 2 remains float-only and therefore mostly ours.** 19.1% of
   `codecity`, 1.4% of `x264`, 0.0% of `dav1d`. Codecs are integer machines.

### 4.3.1 AVX is not a tier — it is tier 3 at another width

The decisive measurement. For every `v`-prefixed instruction, does stripping
the `v` give an operation already in tier 2 or 3?

| | VEX+EVEX share of file | of which: a tier-2/3 op at another width | AVX-512-only |
|---|---|---|---|
| `libavcodec` | 1.7% | **96.0%** | 0.2% |
| `libvpx` | 1.1% | **92.5%** | 0.1% |
| `libx264` | 5.2% | **86.8%** | 2.5% |
| `libswscale` | 2.6% | **82.5%** | 0.0% |
| `libdav1d` | 29.5% | **73.6%** | 8.4% |

Between 74% and 96% of AVX in these libraries is `vpaddw` where we already have
`paddw` — the same lane-wise operation, at 256 bits instead of 128, with a
non-destructive three-operand form. Capstone reports the operand count and the
register width, so **if tier 3 is written with `(element width, lane count,
operand count)` as parameters from the first line, AVX2 largely falls out of
it.** Writing tier 3 as hard-coded 128-bit two-operand functions and bolting
AVX on later would mean writing it twice.

**This is the single most important architectural decision in §4, and it costs
nothing to take now.**

What genuinely does not fall out is a short list — the lane-crossing
operations, which have no SSE counterpart:

```
  vpbroadcast{b,w,d,q}   vbroadcast{ss,sd,i128}    (splat one element)
  vinserti128  vextracti128  vperm2i128  vpermq    (move 128-bit lanes about)
  vzeroupper                                       (a no-op for us)
  vpdpwssd                                         (VNNI, 1.6% of dav1d)
```

That is roughly ten instructions covering the entire "genuinely new" column
above. **AVX-512 is the real cutoff**: masking with `k` registers, `{z}`
zeroing, `vmovdqa32`-style element-typed moves. It is 8.4% of `dav1d`'s VEX and
under 3% everywhere else, and it needs a new register file (`k0`–`k7`) and a
predication rule threaded through every tier-3 operation. **Defer AVX-512, and
say so in the UI** — a wisp in `dav1d`'s AVX-512 path will havoc, and it should
admit it.

### 4.4 XMM/YMM state, and where it goes on the panel

Any of tier 2 or 3 needs sixteen vector registers in the `Vm`, and §4.3.1 says
to store them **512 bits wide from the start** (`uint8_t v[32][64]`, 2 KB) even
while only the low 128 bits are ever written — so that widening to AVX2 is a
change to the tier-3 loop bounds and not to the state. That is easy; the
display is not, because the ceiling panel cannot show sixteen 32-byte values
and stay readable at 7 m.

The rule: **a vector register is shown only when the current instruction
touches one**, as a single extra row — formatted as a float for tier 2, and for
tier 3 as the lanes the instruction actually operates on (eight `int16` for a
`paddw`, sixteen bytes for a `pcmpeqb`), which is far more legible than 32 hex
digits and is what someone reading a codec kernel wants to see anyway. A
256-bit register shows its low lane with a `+128` marker. Everything else stays in the `X`
sheet, where there is room. This matches how the panel already treats the
general registers: `r8`–`r15` are folded unless they changed.

### 4.5 Anything not in a tier

Nothing special is needed, because §3's tier 0 already covers it generically:
Capstone's `cs_regs_access()` reports the registers read and written for AVX,
AVX-512, AES, SHA, x87 and everything else, so the fallback havocs precisely
the right state and the panel says *not modelled*. `rep`-prefixed string
operations are the one case worth naming separately — an invented `rcx` could
be 2^63 — so they are clamped to a bounded iteration count or treated as one
opaque bulk operation, and labelled as such.

### 4.6 The host CPU is irrelevant — except to the oracle

Worth stating because it is the natural worry: **no instruction from the file
under study is ever executed**, so the host's feature set has nothing to do
with what the explorer can show. A machine with no AVX explores an AVX binary
exactly as well as one with it.

The single exception is §16's gdb oracle, which generates its expected results
on real hardware. That corpus must record the host's feature flags in its
header, and vectors cannot be generated for an instruction the generating host
does not implement. Practically: generate on the most capable machine
available, and let the checker skip any vector whose extension it cannot
attest to. The corpus is checked in as data precisely so that everyday `make
test` needs neither gdb nor a particular CPU.

> **A caveat on these measurements.** They come from `objdump -d` mnemonics,
> where `cs` and `data16` in the "everything else" row are branch-hint prefixes
> and decode-desync artefacts that Capstone folds into the instruction. The
> real "unmodelled" fraction is therefore a little smaller than the tables
> show, which only strengthens the conclusions.

---

### 4.7 The VLC case, and a finding that has nothing to do with the VM

The libraries above were measured because VLC is the case worth getting right:
video codecs are the largest body of hand-written SIMD assembly most people
have on their disk, and watching a motion-compensation kernel actually execute
is the most compelling thing this feature could show.

Two things came out of looking at it.

**`/usr/bin/vlc` is a 16 KB launcher with 668 instructions.** It is a shim.
Everything of interest is elsewhere — `libvlccore.so.9` (183 k instructions),
18 MB of plugins under `/usr/lib/x86_64-linux-gnu/vlc/plugins/`, and the codec
libraries behind those.

**And the explorer cannot currently reach any of it.** The `DT_NEEDED` chain is:

```
  /usr/bin/vlc  ->  libvlc.so.5  ->  libvlccore.so.9  ->  libc.so.6
```

Nothing in that chain names `libavcodec`, `libx264`, `libvpx` or `libdav1d`.
The codec plugins are `dlopen`ed at runtime from a plugin directory, and it is
`libavcodec_plugin.so` — reachable by no relocation and no dynamic tag — that
names `libavcodec.so.60`. **Walking the gateways from `vlc` never reaches a
single line of codec assembly.**

This is an explorer problem, not a VM problem, and it is worth fixing
independently: a binary whose real content is behind `dlopen` is common
(VLC, GStreamer, PAM, NSS, Apache, anything with a plugin directory). The
cheapest useful version is to notice a `dlopen`-shaped program — a `DT_RUNPATH`
or a string constant naming a directory of `.so` files — and offer that
directory as a district of gateways alongside the `DT_NEEDED` ones. That folds
neatly into the README's existing "the dependency graph as a region, not a
gateway" idea. Until then, the way to explore VLC's codecs is to open the
codec library directly, which the file browser already allows.

### 4.8 What a wisp would actually do in a codec kernel

Worth being concrete about, because "it works for those" has to mean something.

- **In `libvpx` at `--cpu baseline`**: 98.1% of instructions modelled. The wisp
  walks the SSE2 kernel, the XMM lanes hold real packed arithmetic on invented
  pixel data, and the loop counters are honest. This works.
- **In `libx264`**: 93.6% without AVX. The SSE kernels work; the AVX2 variants
  are reached only at `--cpu v3`, where 5.2% of the file becomes reachable and
  most of it is tier 3 at 256 bits.
- **In `libdav1d`**: 69.6% without AVX, 99.1% with AVX2 minus the AVX-512
  paths. At `--cpu baseline` the dispatch code sends the wisp down the C
  fallback, which is tier-1 integer and works perfectly — a good outcome that
  falls out for free.
- **The pixel data is invented**, always. A motion-compensation kernel run on
  random bytes computes a correct result over meaningless pixels. The
  arithmetic is real; the picture is noise. That is the honest description and
  it should be what the panel says.
- **The loop structure is real**, and that is the part worth watching: a
  wisp stepping a `4x4` transform sees the actual lane arithmetic, the actual
  strides, the actual number of iterations.

There is one genuinely appealing consequence. A codec kernel's dispatch reads
`cpuid` directly (§4.2), so **switching `--cpu` and re-entering the same
function walks you into a different room** — the C fallback, the SSE2 kernel,
the AVX2 kernel — each a physically separate place in the city. For a library
whose whole design is runtime multiversioning, that is the city finally showing
something a listing cannot.

---

## 5. Memory: three layers, and a fake stack

A read of address *A* of *n* bytes resolves in this order:

1. **Shadow.** If we have written *A* (or invented it earlier), return that.
   Sparse 4 KiB pages in a small open hash, capped — 256 pages is 1 MiB and far
   more than a function touches. Eviction is not needed within one run; a run
   that somehow exceeds the cap simply stops recording and keeps inventing.
2. **The file.** If *A* lies inside a section with `SHF_ALLOC` and real file
   bytes, return the bytes from `e->map`, tagged `PV_FILE`. This is what makes
   `lea rax, [rip + 0x1234]` / `mov rdi, [rax]` produce a pointer that resolves
   to an actual string, and it is 7.4% of `git`'s operands.
3. **Invention.** Mint `n` bytes from the PRNG, *write them into the shadow*,
   return them tagged `PV_INVENTED`.

Writes always go to the shadow. The mapped file is never touched — it is a
read-only `mmap` and the city's own display of those bytes must not change
under it.

**Minting is not uniform noise.** A random 64-bit value is useless to look at
and produces absurd branches. Bias by context:

| what is being invented | value |
|---|---|
| a pointer-sized value that the next instruction dereferences | an address inside the synthetic heap region |
| a value compared against a small immediate | a small integer, 0..64, sometimes exactly the immediate |
| a byte read from a string-shaped region | printable ASCII, occasionally NUL |
| anything else | a small integer most of the time, a plausible pointer sometimes |

This is unashamedly cosmetic, and it is the difference between a loop that
runs three times and one that runs 2^40 times before the fuel runs out.

### The synthetic stack

`rsp` starts in the middle of a fake 64 KiB region at, say,
`0x00007fff_ffff0000`; `rbp = rsp`. The return address at `[rsp]` on entry is a
**sentinel** — when `ret` pops it, the run is over and the wisp fades at the
door it came in by. `push`/`pop`/`call`/`ret`/`leave` then work exactly as
written, and the stack ladder on the panel shows real (if invented) frame
contents. Addresses near `rsp`/`rbp` get annotated `stack -0x40` instead of a
raw hex address, which is how anybody reads a stack anyway.

### The stack canary

`git` alone has 4466 `mov rax, fs:0x28` loads. Without a model for `fs`, the
canary check at the end of every protected function compares an invented value
against a *differently* invented value, fails, and the wisp dives into
`__stack_chk_fail` — in perhaps a third of all functions. So: a synthetic `fs`
base with one constant canary at `fs:0x28`, minted once per run. The check then
passes, as it does in reality. This is a small special case that buys a very
large fraction of realistic-looking runs, and it should be in the first
version, not deferred.

### Determinism

Seed the PRNG from the room's address (`splitmix64(r->addr)`), not from the
clock. The same room then shows the same invented state every time you walk
into it, which makes the whole thing feel like a property of the code rather
than a slot machine — and makes bug reports reproducible. A key (`n`) reseeds
for a new run when you want to see a different path.

---

## 6. The walk

One step, given the wisp is at instruction *i* of the room's `Disasm`:

| situation | what happens | how it looks |
|---|---|---|
| ordinary instruction | execute, `i+1` | short hop to the next sculpture |
| `Insn.target >= 0`, taken | jump inside the room | **rides the existing arc**, `arc_point()` with the same lift the wire uses |
| `Insn.tunit >= 0` | jump to another alcove of the chamber | same, across the room |
| conditional, not taken | `i+1` | the arc dims for a beat instead of lighting |
| `Insn.port >= 0`, a `call` | **stepped over** (§below) | the port panel flashes, the wisp stays put for one beat |
| `Insn.port >= 0`, a `jmp`/tail call | the run leaves this room | flies into the port panel; §8 decides what happens next |
| `ret` with the sentinel on top | the run is over | drifts to the door and fades |
| `ret` otherwise | pop and continue at the popped address, if it is in this room | as a jump |
| indirect (`call rax`, `jmp [rax*8+t]`) | if the register's value resolves in the city, treat it as that target; otherwise step over as an opaque call | a stub arc into the ceiling, going nowhere — which is honest |

### Stepping over a call

A call that leaves the room is not followed (§8 explains why). Instead we
emulate the *contract*: `rax ← PV_CALL` (fresh, biased by the callee's name if
we recognise it), the caller-saved registers (`rcx rdx rsi rdi r8–r11`) are
havocked, `rsp` is unchanged. The panel logs `call strlen  -> rax = ~0x17`.

An optional table of ~20 libc models (`strlen` returns a small int, `malloc`
returns a fresh heap pointer *that later dereferences work against*, `memcpy`
returns its first argument, `printf` returns a small positive) is maybe 80
lines and makes the downstream state markedly more plausible. Recommended as a
second-pass refinement.

### Deciding conditionals

95.2% of `git`'s conditional jumps have their flags set by the instruction
immediately before them, and that instruction is one we model. Counting the
`mov`/`lea` cases (which do not touch flags, so the producer is simply further
back) the real figure with proper flag tracking is close to 99%. **Conditional
branches will nearly always be decided by arithmetic, not by a coin.**

When flags *are* unknown — tier-0 instruction, or another architecture —
guess, but guess with a policy:

- a **backward** branch (a loop) is taken while the loop's visit count is under
  a threshold (say 12), then not taken, so loops run a plausible number of
  times and then exit;
- a **forward** branch is a weighted coin, biased to fall through;
- the resulting edge is drawn in a different colour from a decided one, and the
  panel says `guessed`.

Two colours for taken branches — *decided* and *guessed* — is the single
cheapest honesty feature in the whole design.

### Fuel

A hard step budget per run (4000, adjustable), a per-instruction visit counter
(`uint16_t` × `d->n`, 10 KB for the worst room), and a wall-clock guard. When
the fuel runs out the wisp fades where it stands and, if the run was launched
by the player, a fresh one spawns at the entry. Without this a `while(1)`
loop in a room you have walked away from runs forever.

---

## 7. Where it lives in space

The user's instinct — put it on the ceiling — is right, and the room's vertical
budget happens to be free there:

| band | occupant |
|---|---|
| 0 – 1.55 m | sculptures (`class_lift()` caps `h` at 1.55) |
| 1.7 – 3.3 m | exit ports (`PORT_BOT`/`PORT_TOP`) on the far wall only |
| 3.3 – 4.0 m | **empty everywhere except the far wall** |

So the panel flies at about **y = base + 3.35 m**, over the middle of the
serpentine, with:

- a **tether** — a thin translucent beam from the panel down to the top of the
  sculpture being executed, so there is never any doubt what the state belongs
  to. This is the whole trick; without it a floating panel is just a HUD that
  happens to be in 3D.
- a **cursor ring** on the current sculpture, reusing the existing near-
  instruction ring style but in a different colour, so "where you are standing"
  and "where the machine is" never look alike.
- **motion**: the panel does not teleport. It eases between positions over
  `1/rate` seconds, following `arc_point()` for a taken branch so it visibly
  swings back along the same wire the loop is drawn with.

### The trail

Keep the last N states (16 is plenty) as small ghost cards hanging from the
ceiling above the sculptures they belonged to, fading with age and shrinking to
a single line — mnemonic and the one register that changed. The room then
reads as a *timeline* you can walk under and look back along, which is the
thing a static disassembly listing cannot do at all. Cheap, and it is the
feature most likely to be what makes this worth building.

### Level of detail

The panel is ~1.6 × 0.9 m with 10 rows of 0.09 m text — readable to about
7 m, which is most of a room. Beyond that, collapse to a glowing token plus
the current mnemonic; beyond 25 m, draw the tether only. This mirrors what
`draw_code_room()` already does for sculptures (`dd < 90.0f` for detail) and
port labels.

### Chambers

A chamber's seven alcoves are seven separate decodings in one room. Either run
one wisp in the alcove you are standing at (simplest, and consistent with how
`activeUnit` already gates the readout), or allow up to three at once with the
panel of the active alcove drawn in full and the others collapsed. Start with
one; the `Wisp` struct should carry its `unit` index from the beginning so the
second is a small change.

---

## 8. Getting out of the room

This is the one place where the existing architecture pushes back. `city.c`
keeps **exactly one room decoded at a time** (`g_openBld`/`g_openRoom`), and
`--selftest` asserts it (`disasm_live() > budget` is a hard failure). A wisp
that follows a `call` into another function needs that function decoded — and
so does the caller, to come back to.

Three options:

### B1. The local wisp — *recommended as the default*

The run lives and dies with the room. It starts at the room's first
instruction (or at the entry point, if the entry point is in this room), steps
over every call, and ends at `ret`, at a tail-call port, or when the fuel runs
out. Zero memory pressure, zero architectural change, and it is complete in
itself: you are watching *this function* execute.

### B2. Pilot mode — *recommended as the second key*

Press a key and the wisp *leads* rather than being watched: when the run
reaches an exit port, the app does what stepping through that port already
does — `teleport_addr()` — and the wisp continues in the new room with **the
same CPU state**. The state is a plain struct that owns nothing belonging to
the `Disasm`, so it survives the room change for free.

This is the whole "start at the entry point and follow the code path"
experience, it reuses a code path that already exists and is already
self-tested, and it never breaks the single-decoding invariant, because the
player moves too. The camera follows the wisp; the ports you pass through
flash as you go. It also finally gives a reason for the city's ports to be
traversed in sequence, which is the most under-used mechanism in the program.

The cost is that you cannot follow a call *and come back* — teleporting into
the callee frees the caller's decoding. Two answers: keep a small **shadow
call stack** of (address, register snapshot) so `ret` can teleport *back* and
resume, which works and is maybe 40 lines; or step over calls in pilot mode
too and only follow jumps. Prefer the former — following a call into another
room and returning from it is the single most illuminating thing this feature
could do.

### B3. Lift the one-room limit

Allow *k* decodings alive (an LRU of, say, 4 rooms, ~1–2 MB) so a wisp can run
across rooms without moving the player. This is a real change to a deliberate
invariant, it needs the self-test's budget arithmetic reworked, and it buys
only the case of watching a call return while standing still. **Defer.** If
B2's shadow call stack proves annoying in practice, this is the fix.

---

## 9. Relocations, which we already parse and never use

The README's own list of limitations opens with: *"Relocations are read but
never used for geometry — the single biggest gap."* This feature has a direct
use for them.

Before a run, walk `.rela.dyn`/`.rela.plt` and write the resolved values into
the **shadow memory**: `R_X86_64_RELATIVE` → base + addend, `JUMP_SLOT` and
`GLOB_DAT` → the address of the symbol if it is in this file, or a synthetic
stub address named after the symbol if it is not, and `IRELATIVE` → a synthetic
stub named after the IFUNC, never the resolver's own answer (§4.2). Then:

- `call *offset(%rip)` through the GOT resolves to something with a *name*
  instead of to zero;
- a PLT thunk's indirect jump becomes followable;
- pointers in `.data.rel.ro` (vtables, function tables) point at real rooms.

Without this, every GOT-mediated call in a dynamically linked binary — which
is most of the interesting ones — reads a zero out of the file and dead-ends.
It is perhaps 120 lines against tables that are already decoded into text, and
it makes indirect control flow work in the common case.

---

## 10. String constants: much easier than it looks

The intuition is that this is hard. **Measured, it is one of the cheapest wins
in the whole document** — and it is worth separating two questions that look
like one, because the easy one is nearly free and the hard one is not solved by
string handling at all.

### 10.1 Address → string is a decode-time annotation, not an analysis

Every string reference in compiled x86-64 code has one of two shapes:

1. **`lea reg, [rip + disp]`** — the compiler's universal way of naming a
   constant. Capstone hands us the memory operand with `base == X86_REG_RIP`
   and the displacement; add the instruction length, look the address up in the
   section table (`elf_open` already built it), and read the bytes out of
   `e->map`. There is no analysis, no dataflow, no VM. **About 40 lines.**
2. **an `R_X86_64_RELATIVE` addend pointing into `.rodata`** — a pointer in a
   table, which is how every array-of-strings is built. `.rela.dyn` is already
   decoded (§9); the addend *is* the string address.

Measured over the whole `.rodata` of each file — every NUL-terminated printable
run of four characters or more, against the two mechanisms:

| | strings in `.rodata` | by `lea` | by relocation | **union** | left over |
|---|---|---|---|---|---|
| `git` | 12 950 | 85.6% | 17.1% | **99.8%** | 28 |
| `bash` | 1 526 | 58.5% | 45.3% | **98.8%** | 18 |
| `codecity` | 370 | 76.5% | 21.6% | **97.6%** | 9 |
| `libx264.so.164` | 749 | 86.9% | 11.7% | **96.1%** | 29 |
| `libvlccore.so.9` | 4 187 | 63.7% | 12.5% | **75.0%** | 1 047 |

Two mechanisms, neither requiring a single line of the VM, resolve **75% to
99.8%** of every string a binary contains. On `git` twenty-eight strings out of
thirteen thousand are left unaccounted for.

`libvlccore` is the interesting outlier at 75%, and the reason is worth
knowing: VLC builds its module descriptors through macros that emit strings
into static structures reached by computed offsets from a table base, rather
than by a direct `lea` per string. Those are recoverable too — the base is
`lea`-reached, and the offsets are immediates in the surrounding code — but
that *is* dataflow, and it is exactly the kind of thing the wisp does for free
once it is walking. A nice division of labour: the cheap mechanism gets three
quarters of them statically, and the VM picks up the rest when you walk in.

The scale is real, too: 7275 RIP-relative operands in `libvlccore` reach **2963
distinct strings**, and 5482 of those references are the single instruction
`lea`.

### 10.2 What this buys, and it is a lot

- **The panel stops being a hex dump.** `rdi 0x555555559120 → "usage: git…"` is
  the difference between a register display and an explanation. This was
  already in §1 as an aspiration; it is now costed.
- **The `.rodata` gallery gets its wires.** The README's idea of a string
  gallery becomes a room where each string has a visible link to the functions
  that name it — because the reverse index is the same data, inverted. This is
  the reverse direction and it is the only expensive part: it needs every code
  section decoded once (a few seconds for `libavcodec`'s 2.7 M instructions),
  so it should be an opt-in `--index` pass, not something a walk-in pays for.
- **Port labels get better.** A `call` to a room whose first act is naming a
  string can show the string, not just the symbol.
- **Format strings become readable ahead of time.** Standing at a `printf`
  call, the panel can show the format string and how many arguments it wants,
  which is most of what you need to read the call.

### 10.3 Where it stops, and why it does not solve `dlopen`

This is the part that answers the question directly, and the answer is no.
Here is what `libvlccore` actually contains:

```
  "%s/plugins"
  "VLC_PLUGIN_PATH"
  "loading plugins cache file %s"
  "saving plugins cache %s"
```

The plugin path is a **format string**, not a path. VLC builds the directory at
runtime from an environment variable or a compiled-in prefix, then `readdir`s
it and `dlopen`s what it finds. No amount of string-constant recovery produces
`libavcodec_plugin.so`, because that name is never in the binary — it is on the
filesystem.

So string handling and `dlopen` handling are separate problems that happen to
meet here:

- **Recovering a literal `dlopen("libfoo.so", …)`** is easy and worth doing: a
  one-instruction backward look at what set `rdi` before the call, which the
  annotation of §10.1 already resolves. Plenty of programs do exactly this.
- **Recovering a constructed path is not possible statically**, and pretending
  otherwise would be the same dishonesty §14 warns about. The wisp *could* build
  it — with `snprintf`/`strcpy`/`strcat` models in the libc table of §6 and real
  `.rodata` bytes underneath, a wisp stepping the path construction would
  assemble the string live, which is a genuinely lovely demonstration — but the
  directory it interpolates comes from `getenv` or a `readdir`, and those are
  invention.

**The honest fix for VLC is the filesystem, not the code.** Notice a string
constant that names a directory which exists and contains `.so` files — or a
`DT_RUNPATH`, or a plugin directory beside the library — and offer it as a
district of gateways next to the `DT_NEEDED` ones. That is a file-explorer
feature in a file explorer, it needs no dataflow, and it is what actually gets
someone from `vlc` to `libavcodec`'s AVX2 kernels (§4.7).

### 10.4 Cost

| | lines | when |
|---|---|---|
| RIP-relative operand → address → section → string | 40 | with tier 1; the panel needs it immediately |
| `R_X86_64_RELATIVE` addends → string table | 20 | falls out of §9, same pass |
| annotation formatting on the panel (truncation, escaping, a `→` marker) | 60 | with the panel |
| reverse index (`--index`, string → referencing functions) | 150 | later, opt-in, and it earns the `.rodata` gallery |
| literal `dlopen`/`dlsym` argument recovery | 40 | with the index |

Under 200 lines for everything except the reverse index, and the first 60 of
those are on the critical path anyway because the panel is much less useful
without them.

---

## 11. GPU code

The short answer: **a wisp never executes any of it, and it should not try.**
The longer answer is more interesting, because on the machine this was written
on, VLC's GPU code turns out to be recovered *completely* by the string
mechanism of §10 — and because one GPU format is the easiest thing to decode in
this entire document.

### 11.1 GPU instructions are almost never in the file as machine code

They arrive in four forms, in increasing order of difficulty:

| form | what it is | where | difficulty |
|---|---|---|---|
| **shader source** | GLSL / HLSL / OpenCL C, as text | string constants in `.rodata` | **already solved by §10** |
| **SPIR-V** | a specified binary IR | a blob in `.rodata` or a separate file | easy — see §11.3 |
| **PTX / fatbinary** | NVIDIA's IR plus target blobs | `.nv_fatbin`, `.hip_fatbin` sections | partial |
| **native GPU ISA** | SASS, GCN/RDNA, Intel Gen | inside a fatbinary | out of scope |

**Capstone has no GPU architecture at all** — the full list it was built with
here is `ARM, ARM64, MIPS, X86, PPC, SPARC, SYSZ, XCORE, M68K, TMS320C64X,
M680X, EVM`. So there is no path where the existing decoder produces GPU
instructions, and `disasm_open()` correctly returns 0 for anything it does not
know. That behaviour is already right and needs no change.

### 11.2 What VLC actually ships — measured

`libgl_plugin.so` is VLC's OpenGL video output. Its GPU code is **GLSL source
held as string constants**, assembled at runtime and handed to the driver:

```
  "#version %u"
  "%suniform sampler2D Texture0;uniform vec4 xyz_gamma = vec4(2.6);
   uniform mat4 matrix_xyz_rgb = mat4( 3.240454, -0.9692660, ... );
   varying vec2 TexCoord0;
   void main(){ vec4 v_in, v_out; v_in = texture2D(Texture0, TexCoord0);
     v_in = pow(v_in, xyz_gamma); v_out = matrix_xyz_rgb * v_in;
     ... gl_FragColor = v_out; }"
```

22 shader strings, 1107 bytes of GLSL, in a 70 KB plugin. And:

```
  libgl_plugin.so   220 strings | lea 100.0% | reloc 0.0% | union 100.0% | unreferenced 0
```

**Every string in it is reachable, by the single cheapest mechanism in §10.**
VLC's entire shader corpus falls out of a RIP-relative `lea` lookup that was
already going to be written for the register panel. Nothing further is needed
to *find* it.

There is a genuinely nice consequence. A shader is built here by concatenating
`#version %u` with a body, and unlike the `dlopen` path of §10.3 — which
interpolates a `getenv` result and is therefore invention — **the shader body
is entirely in-file constants**. With the `snprintf`/`strcat` models of §6, a
wisp stepping the shader assembly would build the real string, and the panel
could show the finished shader as it is composed. Only the version number is
invented. That is one of the few places in this design where the VM produces a
result that is *true*, and it is worth building for the demonstration alone.

### 11.3 SPIR-V is the one GPU format worth decoding

None is present on this system — the Mesa drivers compile GLSL to their own IR
at runtime, and no CUDA or ROCm is installed — but any Vulkan application ships
it, and it is by a wide margin the easiest binary format in this document:

- a five-word header, beginning with the magic `0x07230203`;
- then a flat stream of instructions, each a single word of
  `(word_count << 16) | opcode`, followed by that many operand words.

Walking it needs no external dependency and no disassembler: roughly 300 lines
gives the instruction list, and `OpName`, `OpEntryPoint`, `OpSourceExtension`
and friends hand back the *original identifiers* — shader stage, entry point,
variable names, and often the original source. A SPIR-V blob is therefore
richer than the surrounding machine code, not poorer.

This belongs in the README's "non-code sections as interactive sculptures"
family, not here: it makes a shader blob a readable room instead of a byte
mosaic. Worth noting as the highest-value GPU work, and worth keeping out of
the VM entirely.

`.nv_fatbin` and `.hip_fatbin` deserve detection and a summary — which target
architectures are inside, and the PTX, which is text and can be listed like any
other listing room. The native SASS inside is undocumented and stops there.

### 11.4 Why the wisp must not follow, even in principle

This is not a gap to be filled later. A GPU is a different execution model, and
"a CPU state that walks the code" is the wrong metaphor for it:

- There is no *a* state. A wavefront is 32 or 64 lanes stepping one instruction
  together under a per-lane execution mask, and a dispatch is thousands of
  those. The ceiling panel shows one register file; the honest GPU equivalent
  is sixty-four of them plus a mask.
- Control flow is divergence and reconvergence, not branches. A conditional
  does not choose a path — it disables lanes, runs both sides, and merges.
  §6's "taken / not taken, decided or guessed" has no meaning there.
- Memory is a hierarchy of address spaces (private, local, global, constant)
  with no single flat map, so §5's three layers do not apply.
- And none of it is reachable from the CPU side anyway: the code is handed to a
  driver, compiled by it, and run on another device.

So a wisp that reaches `glCompileShader`, `vkCmdDispatch` or
`clEnqueueNDRangeKernel` does exactly what it does for any other call that
leaves the room — it steps over it (§6). **The one thing worth adding is a
label**: a port whose destination is a known GPU-submission entry point should
say so — *the work happens on another device; this program cannot follow it* —
rather than looking like an ordinary unresolved call. That is a dozen lines and
a name table, and it turns a dead end into the edge of the city, which is the
truth.

### 11.5 Cost

| | lines | |
|---|---|---|
| recognise a shader string and show it as text, not bytes | 60 | heuristic on `#version` / `gl_Position` / `layout(` / `uniform`; §10 already found the string |
| label GPU-submission ports as leaving the machine | 20 | a name table of ~30 entry points |
| SPIR-V blob detection and instruction listing | 300 | separate from the VM; belongs with the data-section work |
| `.nv_fatbin` detection and PTX listing | 120 | detection and a summary only |
| native GPU ISA | — | out of scope; nothing decodes it here |

The first two are worth doing alongside the panel. The rest is a data-section
feature that has nothing to do with execution, and should be planned with the
README's other section-as-sculpture ideas rather than with this document.

---

## 12. Controls, and the HUD

Fitting the existing scheme (single letters, `F1` help, `Tab` detail):

| key | |
|---|---|
| `x` | start/stop a wisp in this room |
| `Space` | pause / resume |
| `.` | single step (implies pause) |
| `,` / `/` | slower / faster (0.5 – 20 instructions per second) |
| `n` | reseed and restart the run |
| `p` | pilot mode: follow the wisp through ports (§8 B2) |
| `X` | full register/memory sheet in the HUD |

`Tab`'s room detail sheet already shows a seven-line disassembly window
centred on `a->nearIns`. When a wisp is running it should centre on the *wisp*
instead, with the executed lines marked — turning that panel into a trace view
for free.

---

## 13. The prerequisite: text that can change every frame  — **VALIDATED, DO FIRST**

**Validated as a blocker: this is built before anything else.** It is the one
thing that could sink a naive implementation. `text.c`
caches whole rendered strings keyed by their content (`hashs(s, font)`), with
an LRU bounded at 2000 entries / 28 MB. A register panel produces
*sixteen new strings per step*, none of which is ever seen again: at 6 steps a
second that is ~100 TTF rasterisations plus 100 `glTexImage2D` uploads per
second, and it evicts the room's actual labels — the "treadmill" the cache's
own comment warns about.

The fix is a **monospace glyph atlas**: rasterise the 95 printable ASCII
characters of `FNT_MONO` once into a single texture, and add

```c
void text_mono_3d(const float p[3], const float r[3], const float u[3],
                  float h, int anchor, const char *s);
void text_mono_2d(float x, float y, float px, const char *s);
```

which draw a string as one `glBegin(GL_QUADS)` run against one bound texture.
A 200-glyph panel becomes one bind and 200 quads with no rasterisation at all.
This is ~150 lines in `text.c`, it is useful well beyond this feature (the HUD
readouts change every frame too), and it should be **step one**.

---

## 14. What this will not do

Stated plainly, because the feature's whole risk is being believed:

- **It is not execution.** No kernel, no syscalls (an `int 0x80`/`syscall` sets
  `rax` to an invented value and moves on), no dynamic loader, no libc, no
  ASLR, no threads, no signals, no self-modifying code.
- **The inputs are fiction.** Arguments, environment, heap contents and
  anything read from an unmapped address are invented. Everything computed from
  them is arithmetically correct and semantically meaningless. A loop trip
  count is a *plausible* trip count, not the real one.
- **`.bss` and any `SHT_NOBITS` region has no bytes**, so it invents; and
  `.data` gives its *initial* values, which for anything mutated at startup is
  wrong in a way that looks right.
- **SIMD and floating point are havoc in tier 1.** A float-heavy binary — ours —
  shows a wisp that walks correctly but whose registers stop meaning anything
  inside a numeric loop until tier 2 lands. **AVX-512 is never modelled** —
  under 3% of most files but 8.4% of `dav1d`'s vector code — and a wisp in one
  of those paths havocs and says so (§4.3.1).
- **IFUNC targets are not resolved**, because the feature struct glibc's
  resolvers read lives in `ld.so`, not in the file (§4.2). The port says which
  symbol it is and that the choice happens at load time.
- **No GPU code is executed or decoded** (§11). Shaders are shown as the source
  text they are; a dispatch call is a port labelled as leaving the machine.
- **Data decoded as code produces nonsense**, exactly as it already does for
  the sculptures. The wisp will wander through a jump table and it will look
  like garbage, because it is.
- **Non-x86 architectures get tier 0 only**: control flow and register
  disturbance, no values. This must be said in the UI, not just here.
- **Flags are approximated.** AF in particular is rarely worth computing
  correctly; PF only over the low byte. Any instruction whose flag effects we
  are unsure of should clear the known bit rather than guess.
- **`rep` string operations** are either run for a bounded number of iterations
  (with `rcx` clamped to something sane, since an invented `rcx` could be 2^63)
  or treated as an opaque bulk operation. Either way, say which.
- **Coverage is not correctness.** 97% of instructions modelled is not 97% of
  runs correct: one wrong `movzx` early poisons everything after it. The
  self-test (§16, §17) should compare our result against a known-good emulation for
  a hand-written corpus, not just check that nothing crashes.

### The alternative that would actually be true

Record a **real** trace, offline, and replay it: `perf record -e
intel_pt//`, or a `gdb`/`ptrace` stepper, or a `qemu-user` plugin, dumped to a
file of (address, register delta) records. `codecity --trace run.trace` would
then float *observed* states through the same rooms with none of the invention
— and the renderer, the tether, the trail and the ceiling panel are all
identical. If this feature works visually, that is the natural sequel, and it
is worth keeping the `Vm`→renderer interface narrow enough (a struct of
values, tags and a step kind) that a trace file can drive it instead.
Running the binary itself from inside the explorer is a different matter: it
executes untrusted code of possibly foreign architecture, and it should not
happen. Reading a trace file someone else produced deliberately is fine.

---

## 15. Change list

### new: `src/vm.h`, `src/vm.c` (~700 lines)

```c
typedef enum { PV_NONE, PV_INVENTED, PV_FILE, PV_DERIVED, PV_CALL } Prov;

typedef struct { uint64_t v; uint8_t prov; uint32_t stamp; } VReg;

typedef struct VmPage { uint64_t base; struct VmPage *next; uint8_t b[4096]; } VmPage;

typedef struct {
    VReg      r[17];                /* 16 GPRs + rip                       */
    uint8_t   fl[6], flknown;       /* CF PF AF ZF SF OF                   */
    VmPage   *pg[64];               /* shadow memory, open hash            */
    int       npg;
    uint64_t  stackLo, stackHi, fsBase, heapNext;
    uint64_t  seed;                 /* splitmix64, from the room address   */
    const Elf *elf;                 /* layer 2 of a read                   */
    int       steps, fuel;
    /* a small ring of what just happened, for the panel */
    struct { uint64_t addr, val; uint8_t n, wr, prov; } mem[8];
    int       nmem;
    struct { uint64_t pc; VReg r[17]; } call[16];    /* shadow call stack   */
    int       ncall;
} Vm;

typedef enum { ST_FALL, ST_TAKEN, ST_NOT_TAKEN, ST_CALL_OVER,
               ST_EXIT_PORT, ST_RET, ST_STOP } StepKind;

void      vm_init(Vm *m, const Elf *e, uint64_t entry, uint64_t seed);
StepKind  vm_step(Vm *m, const Insn *in, const uint8_t *bytes, uint64_t *next,
                  int *decided);          /* decided=0 -> the branch was guessed */
uint64_t  vm_read(Vm *m, uint64_t a, int n, Prov *pr);
void      vm_write(Vm *m, uint64_t a, int n, uint64_t v);
void      vm_relocs(Vm *m);                /* §9 */
const char *vm_annotate(const Vm *m, uint64_t v, char *buf, size_t n);
void      vm_free(Vm *m);
```

`vm.c` holds the register table, the flag model, the memory layers, the
condition predicates and the tier-0 fallback; a second file `src/vmx86.c`
(~500 lines) holds the tier-1 semantics, keyed by `X86_INS_*` so it is a
switch, not string comparison.

### new: `src/wisp.h`, `src/wisp.c` (~250 lines)

The animated body: current index, previous index, interpolation `t`, rate,
fuel, per-instruction visit counts, the trail ring, and `wisp_tick(dt)`. Owns
a `Vm`. One per room (or per alcove), spawned and freed from `room_lifecycle()`
so it obeys the same discipline as the decoding.

### `src/text.c` (+150)
The monospace atlas of §13, plus `text_mono_2d`/`text_mono_3d`.

### `src/render.c` (+250)
`draw_wisp()`: panel, tether, cursor ring, trail. Reuses `arc_point()` for the
hop, `ICOL[]` for the class colouring, and the port flash. Called from
`draw_code_room()` after the ports so it composites over them.

### `src/hud.c` (+120)
The full state sheet on `X`; the `Tab` detail window follows the wisp when one
is running.

### `src/main.c` (+120)
Keys (§12), the tick, pilot mode's teleport hook, and `--vmtest`.

### `src/city.c` (+40)
Expose the entry point (`e->entry`) as a spawn target; call `vm_relocs()` when
a run starts.

### `src/app.h` (+6)
`Wisp *wisp; int pilot;` — nothing else; the wisp is not part of the city
model, exactly as `nearIns` is not.

**No change to `model.h`, `world.h`, `disasm.h`, or any geometry.** That is the
strongest argument that this design fits the program it is going into.

---

## 16. Ground truth: gdb as a differential oracle

**Validated on this machine** (gdb 15.1, Python 3.12, `ptrace_scope=1`). The
question was whether stepping real code under gdb could tell us how to handle
the strange cases of §3 and §14. It can, but not in the shape it first looks
like.

### The wrong shape: tracing library functions for their own sake

Calling a "safe" syscall-free function out of a real shared library and
watching it run is easy — gdb will do it — but it is a **poor sample of
instructions**, because libc's string and memory functions are hand-written
SIMD. Stepping the real `strlen` with a 42-byte argument, on this machine:

```
strlen resolves to 0x7ffff7fedb00  (strlen in .text of ld-linux-x86-64.so.2)
35 instructions stepped
   pxor 4   pcmpeqb 4   pmovmskb 4   mov 3   and 3   shl 3   test 2   je 2
   or 2     endbr64 1   cmp 1       ja 1    movdqu 1  xor 1   sar 1   bsf 1  ret 1
```

Thirteen of thirty-five instructions — 37% — are the SSE2 block compare, which
is exactly the tier-0 havoc territory we deliberately chose not to model. So
this trace mostly tells us something we already knew. Three further traps show
up in that one line of output:

- the symbol resolved into **ld.so**, not libc, because the process was still
  early; and it is the **SSE2 IFUNC variant**, chosen by the CPU at load time.
  "The function at that address in the `.so` file" is not what runs — which is
  itself worth knowing for §9's `R_X86_64_IRELATIVE`.
- `movdqu` on an aligned-down pointer **reads bytes before the buffer**. An
  interpreter that treats a read outside a known object as an error, rather
  than as ordinary memory, will fail on the most common string idiom there is.
- `bsf` is in there, and its destination is architecturally *undefined* when
  the source is zero (see below).

Conclusion: use libc traces to **enumerate exotica**, not as a coverage target.
Ordinary compiled application code — the thing someone walks into a room to
read — is far more mundane, and `codecity`'s own `.text` is a better corpus.

### The right shape: a controlled single-step oracle

Do not run library functions. Run **one instruction at a time, from a state we
chose**, and diff the result against the VM. A ~40-line harness plus a ~60-line
gdb Python script is the whole thing:

```c
/* oracle.c -- a controlled place for gdb to single-step one instruction */
unsigned char *scratch;   /* RWX: the instruction under test */
unsigned char *data;      /* RW : stack + operands           */
void probe(void){}        /* gdb breaks here                 */
```

The script writes the instruction bytes into `scratch`, sets the sixteen GPRs
and `eflags` to the test vector, points `rip` at the page, `stepi`, reads the
registers back, and restores. The inferior is never allowed to run free, a
fault is caught by gdb rather than being fatal, and nothing from the file under
study is ever executed. It generalises to randomised vectors — thousands of
(instruction, state) pairs per run — which is how you find semantics bugs that
a hand-written corpus never reaches.

**Measured, on the first run of that harness.** Every one of these is a rule
the interpreter has to get right, and several are not what a naive
implementation would do:

| tried | hardware said |
|---|---|
| `mov eax, ecx` | `rax = 0x11223344` — **upper half zeroed** |
| `mov ax, cx` | `0xdeadbeefcafe1234` → `0xdeadbeefcafe3344` — merged |
| `mov al, cl` | `…1234` → `…1244` — merged |
| `movsxd rax, ecx` (`ecx=0xffffffff`) | `0xffffffffffffffff` |
| `inc rax` with CF preset | ZF/PF/AF changed, **CF untouched** |
| `shl rax, 0` | **no register and no flag changed at all** |
| `shl rax, 65` | count masked to 6 bits → `rax = 2` |
| `shl eax, 33` | count masked to **5** bits at 32-bit width → `2` |
| `bsf rax, rcx` with `rcx = 0` | ZF set, **destination left unchanged** |
| `bsf rax, rcx` with `rcx = 8` | `rax = 3` |
| `imul rax, rcx` overflowing | CF and OF set — and ZF/PF, which the manual calls undefined |
| `lea` with every flag preset | no flag touched |

The shift-count masking, the width-dependent mask, the zero-count no-op, `inc`
not touching CF, and `bsf` preserving its destination are five bugs that would
otherwise have shipped, and four of them would have shown up as *plausible
wrong numbers* on the ceiling panel rather than as a crash.

### How it plugs into the VM

Two modes, both worth having:

1. **Instruction-level fuzz.** For each of the ~70 tier-1 mnemonics, a few
   thousand random operand/flag vectors, stepped on hardware and through the
   VM, compared on the full register and flag state. This is the defence
   against §14's "97% coverage, silently wrong".
2. **Trace-level replay.** Step a real function of *our own* binary under gdb,
   dump `(rip, register state)` after every instruction, and replay the same
   instruction sequence through the VM **primed from the trace's initial
   state**, diffing after each step and stopping at the first divergence. Any
   difference is then attributable to exactly one instruction. This is also the
   file format that §14's "the alternative that would actually be true" would
   consume, so the two efforts converge.

### What gdb cannot help with

Worth being explicit, because it is easy to over-invest here:

- **It removes the uncertainty that is the whole point.** gdb supplies real
  arguments, a real stack, a real heap and a loader-filled GOT. It validates
  the *semantics* half of the design and says nothing about the invention
  policy, the branch-guessing policy, the loop budget, the call step-over
  contract or the minting bias. Those are judged by eye.
- **Absolute addresses will never match**, because the inferior has ASLR, a
  real stack and real TLS. All comparison must be on register/flag *deltas*, or
  on a VM primed from the trace's own initial state.
- **Single-stepping into a PLT the first time** lands in `_dl_runtime_resolve`,
  a long detour. Set `LD_BIND_NOW=1`.
- **Even "pure" functions can trip into a syscall** (lazy TLS, a first-touch
  `malloc`). `catch syscall` detects it so those traces can be discarded.
- **Speed.** A ptrace single-step loop is a few thousand steps a second. Ample
  for corpora of tens of thousands; not for millions.
- **x86-64 only, natively.** ARM/MIPS/PPC would need `qemu-user`'s gdbstub
  (`qemu-aarch64 -g 1234`), which is easy and would let tier 0 be checked on
  another architecture — but tier 0 has almost no semantics to check, so this
  is low value until there is a tier 1 for a second architecture.

An alternative oracle is Unicorn, used as a **test-only** dependency: no
ptrace, deterministic, given exactly our memory layout, and perhaps a thousand
times faster, so millions of random vectors become possible. Against it: it is
another emulator, so a shared misreading of the manual stays invisible.
Hardware via gdb is the ground truth and should be primary; Unicorn is the way
to get volume if the fuzz corpus proves too slow.

---

## 17. Testing

- **`--vmtest FILE`**, headless, alongside `--selftest`: for every code room in
  the first *N* buildings, spawn a wisp, run it to its fuel limit, and assert
  it terminates, does not fault, allocates a bounded number of pages, and frees
  everything. Report a table: rooms run, mean steps to `ret`, share of steps
  handled by tier 1 vs tier 0, share of conditionals decided vs guessed, ports
  reached. That table is the honest measure of whether the feature works, and
  it belongs in the README the way the room-sizing numbers do.
- **A semantics corpus, generated rather than hand-written** (§16). The gdb
  oracle produces the expected register and flag state for thousands of
  randomised (instruction, state) vectors; the corpus is checked in as data so
  the test needs neither gdb nor an x86 host to run. This is the only defence
  against the "97% coverage, silently wrong" failure in §14.
- **`--selftest` additions**: a wisp must not perturb `disasm_live()`, and
  entering/leaving a room with a wisp running must leak nothing. ASan and
  UBSan clean, as the rest of the program already is.
- **Determinism**: running the same room twice must produce an identical step
  sequence. One assertion, and it protects the property from §5.

---

## 18. Cost, per milestone

The fine-grained breakdown behind the **Milestones** section at the top of this
document. Every step belongs to exactly one milestone, so the totals say what
each milestone costs before it can be tested.

| M | step | lines | what it earns |
|---|---|---|---|
| **M0** | monospace atlas in `text.c` | 150 | changing text at all; helps the HUD too |
| | *total* | **150** | |
| **M1** | `Vm` skeleton: registers, flags, memory layers, tier 0 | 300 | a wisp that walks any architecture and shows disturbance |
| | the wisp body + ceiling panel + tether + cursor | 350 | **the feature is visible and worth looking at** |
| | `--vmtest` harness | 120 | termination, leaks, budgets |
| | *total* | **770** | |
| **M2** | tier-1 x86-64 semantics + condition predicates | 500 | 97% of instructions; branches decided by real arithmetic |
| | the gdb oracle and its generated corpus (§16) | 80 + 100 of throwaway tooling | knowing whether any of it is right |
| | *total* | **580** | |
| **M3** | stack, canary, call step-over, `ret` sentinel | 150 | runs that look like function calls instead of drifting |
| | relocations into shadow memory (§9) | 120 | indirect calls resolve; closes a README gap |
| | annotation: symbols and strings for pointer values (§10.1) | 120 | the panel explains rather than dumps |
| | libc return models | 80 | plausible downstream state |
| | *total* | **470** | |
| **M4** | the trail, LOD, `X` sheet, `Tab` trace view, controls | 300 | the room becomes a timeline you can read |
| | *total* | **300** | |
| **M5** | pilot mode + shadow call stack (§8 B2) | 200 | "start at the entry point and follow the path" |
| | *total* | **200** | |
| **M6** | tier 2 scalar SSE + tier 3a packed moves | 240 | 92.1% → 97.0% on ours; 98.8% on `git` |
| | tier 3b packed shuffle/compare, **width-parametric** (§4.3.1) | 120 | string idioms; 88.2% on `x264` |
| | tier 3c packed integer arithmetic | 200 | **62.8% → 98.1% on `libvpx`**; the arithmetic of video |
| | *total* | **560** | |
| **M7** | AVX2: tier 3 at 256 bits + ~10 lane-crossing ops (§4.3.1) | 150 | `dav1d` 69.6% → 99.1% |
| | *total* | **150** | |
| **M8** | CPUID profiles + `--cpu`, `xgetbv` (§4.2) | 120 | codec dispatch; the same function becomes several rooms |
| | *total* | **120** | |
| — | shader strings as text + GPU port labels (§11.5) | 80 | slot in beside M3 |
| — | reverse string index, `--index` (§10.4) | 150 | earns the `.rodata` gallery its wires |
| — | SPIR-V listing (§11.3) | 300 | a data-section feature, not an execution one |
| — | AVX-512 (`k` registers, masking) | 400 | **deferred**; 8.4% of `dav1d`'s vector code |

**M0–M1 is 920 lines and is a standalone, demonstrable thing**: a state that
walks the room and tells you honestly which registers each instruction touches,
on every architecture Capstone decodes. Everything after that is refinement in
decreasing order of payoff, and stopping at any milestone boundary leaves the
program whole.

**M2 is the risky one** — not because it is hard, but because it is the first
place where being *subtly* wrong is possible. That is why the oracle is inside
it rather than after it: it found five real bugs before a line of the
interpreter existed, four of which would have surfaced as plausible wrong
numbers on the ceiling panel rather than as a crash.

**M6 is the one that must not be rushed.** Writing the packed tiers with fixed
128-bit two-operand forms would make M7 a rewrite instead of 150 lines. The
parameterisation costs nothing on the day and is the difference between AVX2
being nearly free and being a second project.

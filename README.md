# Code City — a 3D file explorer for binaries

<https://github.com/xavierdpt/codecity>

Walk around the inside of an ELF file.

An executable or shared library is rendered as a city you can walk through. Its
sections are towers, zoned into districts by what they hold. Its symbols are the
rooms inside those towers, reached from a corridor on each floor, with the
symbol's name on a plate over the door. You get in through the front door and up
the stairs like anywhere else.

Step into a room in an executable section and its bytes are disassembled on the
spot: every instruction becomes a sculpture, and the branches between them are
wires with a pulse running along them. Step back out and the decoding is freed.

Written in C against OpenGL + GLU (fixed-function, compatibility profile), with
SDL2 for the window and input, SDL2_ttf for the signage, and Capstone for the
disassembly. No engine, no scene graph, no external ELF library — the ELF reader
is `src/elfload.c`.

## Build and run

```sh
make
./codecity /usr/lib/x86_64-linux-gnu/libc.so.6
./codecity                 # defaults to /bin/ls
```

Needs `libsdl2-dev`, `libsdl2-ttf-dev`, `libgl-dev`, `libglu1-mesa-dev`,
`libcapstone-dev`, and a DejaVu or Liberation font.

Capstone is optional at runtime, not at build time: on an architecture it does
not cover, code rooms fall back to showing raw bytes and the program says so on
stderr.

---

# What it does

## The city

| in the file | in the city |
|---|---|
| the file | a city, approached along an avenue of gateways, with a monument carrying the ELF header |
| a section's flags and type | a district — CODE, RODATA, DATA, LINKAGE, DEBUG, MISC |
| a section | a tower |
| a symbol | a room, sized by the symbol's size |
| a slice of a table, or of bytes no symbol covers | also a room |
| a symbol's name, binding, type, address and size | the plate over its door |
| two symbols contiguous in the file | a door in the party wall between their rooms |
| `DT_NEEDED` | a gateway on the approach — press **E** to travel into that library's city |

A tower's shape is not decoration. The number of floors is the number of rooms it
needs; the corridor length is how wide those rooms have to be. A fat `.dynstr`
really is a tall building. Every tower has a stair core at its west end with a
spiral staircase joining every floor, and the front door is at the foot of it.

## Inside a room

What you see is the actual bytes:

| room | what it shows |
|---|---|
| a room in an executable section | disassembled the moment you walk in — see below |
| a function you are not standing in | its machine code as a floor of columns, one per byte, coloured by value |
| a data object | its bytes as crates, one per 16 bytes, taller where they are non-zero |
| a table — `.dynamic`, `.symtab`, `.dynsym`, `.rela.*`, `.note.*` | the real decoded entries printed on a board on the wall |
| a string table | the actual strings, on the wall |
| anything else | a byte-per-cell mosaic |
| `.bss` | an empty room with a ghost outline, because there is nothing there yet |

## Code rooms

Cross the threshold of a room in a section carrying `SHF_EXECINSTR` and its bytes
go to Capstone. The instructions are laid out as a boustrophedon ribbon across the
floor — one row left to right, the next right to left — so consecutive
instructions stand next to each other and the thread of execution is a path you
can walk along.

Each instruction is a sculpture. Its height is its length in bytes; its form and
colour are what it does:

| form | instruction |
|---|---|
| spire | an unconditional jump |
| tapered spire | a conditional branch |
| beacon — a shaft with a sphere on top | a call |
| drum | a return |
| stacked tiers | a vector/SIMD instruction |
| flat slab | padding — `nop`, `endbr64`, `ud2` |
| plain block | everything else, coloured by class: move, stack, arithmetic, logic, compare, system |

Branches are wires between the sculptures they connect, with a bead running along
each so you can see which way control flows:

- **cyan** — a branch forward
- **orange** — a branch backwards, which is to say a loop
- **magenta** — a call landing inside the same room
- **green** — a call into a neighbouring alcove of the same chamber; still the
  same room, so it wires straight to the sculpture rather than out through a port
- **yellow** — a branch that leaves the room, running out to its own **port**

### Ports

Every distinct destination the room's code leaves for gets one port: a lit
signboard on the far wall, named for the symbol it lands in (`malloc`,
`_IO_puts+18`) or the raw address, coloured by the kind of transfer that reaches
it. They are ranked across the wall in the clear band above the sculptures, so
the ways out of a function are legible from the doorway, and every yellow wire
in the room ends at the one port for its own destination.

Look at a port and it lights up; press `E` or click and you are taken to the
room that holds that address — another function in this tower, a stub in `.plt`,
an alcove of a chamber three floors up. That is the shortest path there is from
`call foo` to standing inside `foo`.

Walk up to a sculpture and it is ringed, with its address and the full
instruction — operands and all — in the readout at the bottom of the screen.
The ring is the only thing drawn in the world for it: a label floating in the
room can hold a mnemonic and nothing more, and the readout has room for the
whole line. `Tab` lists its neighbours with their addresses and classes.

The control-flow mnemonics (`call`, `jne`, `ret`) are billboarded over their own
sculptures, but only for the dozen nearest — they are signposts for where you
already are, not an overlay on the whole room.

**The decoding lives only while you are in the room.** Crossing the threshold
decodes it; stepping back out frees it. At most one room's worth of instructions
is ever allocated, which `--selftest` asserts. A chamber counts as one room: all
seven of its alcoves are decoded together, so the whole room is lit the moment
you step in rather than one sculpture at a time as you walk between them.

## Controls

| | |
|---|---|
| `W A S D` | walk, `Shift` to run, `Space` to jump |
| mouse | look; `Esc` releases the pointer, `Esc` again quits |
| — | walk into the lit door on a tower's west face to go in |
| spiral stair | climbs every floor; `[` and `]` jump a floor |
| `Tab` | detail sheet for the room — the instructions around you, or a hex dump |
| `E` or click | step through the port you are looking at, or travel through a dependency gateway |
| `F` | file browser — arrows, type to filter, `Enter` to open, `Backspace` to edit |
| `M` `G` `V` `R` | minimap, wireframe, free-fly, back to the plaza |
| `F1` | controls and legend |
| `F2` | text stress test: 200 never-repeated strings a frame, first out of the monospace atlas, then through the string cache |
| `X` | start (or stop) a **wisp** in the code room you are standing in — a CPU state that walks it |
| `K` `-` `=` `N` | run / pause · slower · faster · a different run |
| `T` / `I` | **step over** one instruction (a call is run whole) / **step into** the call. `F10` / `F11` do the same |
| `Shift+X` | the whole state sheet — every register with what it points at, the flags, the stack, the last memory references |
| `Tab` (with a wisp running) | the room detail sheet follows the *machine* instead of your feet, with a visit count against every line |
| `P` | **pilot mode** — start at the entry point and let the wisp lead: it walks through the ports and you go with it. It starts *paused*: step it with `T` and `I`, or press `K` to let it run |
| `Ctrl+Q` | quit |

## Source layout

| file | what it does |
|---|---|
| `src/elfload.c` | ELF32/64, little- and big-endian reader over an `mmap`; sections, segments, symbols, `.dynamic`, address→symbol lookup |
| `src/disasm.c` | Capstone wrapper: one handle per architecture, a room decoded into classified instructions with their branch targets resolved |
| `src/city.c` | the mapping above — rooms, floors, tower proportions, district and city packing, lazily decoded table text, and the enter/leave lifecycle of a code room |
| `src/world.c` | wall and slab geometry, the spiral-stair height field, collision, player physics |
| `src/render.c` | the drawing — exteriors baked into display lists, interiors drawn per floor |
| `src/text.c` | strings rendered by SDL2_ttf into cached GL textures, for signs, plates and the HUD; the cache is bounded in bytes as well as entries, and never evicts a string the frame in progress has already drawn. Beside it, a monospace glyph atlas — the 95 printable ASCII of `FNT_MONO` rasterized once — for text whose *content* changes every frame, which the cache cannot hold |
| `src/vm.c` | the machine a wisp carries: registers keyed by Capstone id with x86's sub-register write rule, flags with a known bit each, three memory layers (shadow, the mapped file, invention), and the tier-0 fallback that models no instruction semantics at all |
| `src/vmx86.c` | tier 1: what the ~70 integer instructions actually do, and the sixteen condition predicates. Every rule in it was checked against hardware |
| `src/vmcpu.c` | a virtual CPU for `cpuid` and `xgetbv` to answer: three profiles from the x86-64 psABI levels, plus the host's own. A codec dispatches on this, so it decides which kernel a wisp walks into |
| `src/vmsimd.c` | tiers 2 and 3: scalar float, packed moves, packed shuffle/compare and packed integer arithmetic — written as a loop over *(element width, lane count, operand count)*, never as 128-bit two-operand functions, so AVX2 is the same code at another lane count |
| `src/vmcheck.c` | replays `tests/vm-corpus.txt` — states a real CPU produced — through `vmx86.c` and reports any divergence, and counts how much of a file's text is reachable with no analysis at all |
| `tools/oracle.c`, `tools/oracle.py` | the generator for that corpus: a gdb script that single-steps one instruction at a time from a chosen register state. Needs gdb and an x86 host; the everyday test does not |
| `src/wisp.c` | the body: which instruction the state is standing on, how it eases to the next one, the per-instruction visit counts the loop policy needs |
| `src/hud.c` | readouts, minimap, detail sheet, help, file browser |
| `src/main.c` | window, event loop, file loading, and the four headless modes |

## Headless modes

```sh
./codecity FILE --selftest   # walks in the front door, up the spiral to floor 3, back
                            # down one, along the corridor and through a room door, for
                            # the first 8 towers; asserts that entering a room
                            # allocates one room's worth of decoding and leaving
                            # frees it, and that every port in a code room can be
                            # aimed at and leads to a room that holds its address.
                            # Non-zero exit on failure.
./codecity FILE --shot DIR   # renders eighteen canned viewpoints to DIR/*.ppm
./codecity FILE --bench      # frame time in the busiest code room it can find
./codecity FILE --textbench  # 200 never-repeated strings a frame, drawn out of the
                            # monospace atlas and then through the string cache
./codecity FILE --vmtest     # replay the hardware corpus through the interpreter,
                            # then run a wisp in every code room of the first eight
                            # code towers; asserts zero divergence from hardware, that
                            # every run terminates, allocates a bounded number of
                            # shadow pages, frees everything, does not move
                            # disasm_live(), and takes the same path twice.  -v shows
                            # the first twenty divergences
./tools/gen-corpus.sh        # regenerate tests/vm-corpus.txt from this machine's CPU
                            # (needs gdb and an x86-64 host; --vmtest does not)
```

`--cpu baseline|v2|v3|host` picks the CPU a wisp's `cpuid` answers as —
`baseline` by default, which is x86-64-v1 and what the compiler assumed.

### The same call site, two different rooms

`git`, `bash` and this program never execute a `cpuid` at all: for ordinary
application code it is a loader phenomenon, done once inside `ld.so` and
written into a struct. Codec libraries invert that — `libvpx` has fifteen of
them, in its own code, with no IFUNC in sight. A resolver that dereferences
`ld.so`'s data cannot be run honestly; a `cpuid` can, because it is a pure
function of `(eax, ecx)` and a small table.

So `--cpu` is not a curiosity for those libraries. It is the control that
decides which kernel a wisp walks into, and `--selftest` demonstrates it:
it finds a `cpuid`, walks the function it sits in under each profile, and
reports where the paths part company.

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

`0x9d88` is `je 0x9da0`, immediately after `and ecx, 0x18000000` — the test for
OSXSAVE and AVX together. Under `baseline` and `v2` the wisp falls through and
returns; under `v3` it takes the branch and walks into the code that checks
`xcr0` and then asks for leaf 7. The same instruction, two answers, two rooms.

**AVX-512 is absent from every profile on purpose.** It is not modelled, and a
profile that advertised it would send a wisp into code the interpreter could
only havoc.

`--selftest` passes on 60 binaries and libraries from `/usr/bin` and
`/usr/lib/x86_64-linux-gnu`, and on the kernel's 32- and 64-bit vDSOs, which is
what exercises Capstone's `CS_MODE_32` path. Non-ELF files are refused with a
message.

`--bench` in a 640-instruction room with 251 wires: ~405 fps, 2.5 ms/frame on
Mesa/Iris Xe. The live app is vsync-capped to 60.

### A wisp: a CPU state that walks the room

Press `X` inside a code room and a state appears on the first instruction: a
panel of registers and flags hanging over the serpentine, a tether down to the
sculpture being executed, and a ring around it. It steps along the path the
code actually takes — a taken branch rides the same arc the wire is drawn with
— and stops at a `ret`, at a port out of the room, or when its fuel runs out.

**It is not execution, and the panel says so.** There is no kernel, no loader,
no libc; the inputs are invented. What it *is* is consistent: the first read of
anything with no value mints one from a PRNG seeded off the room's address and
writes it back, so the same room shows the same run every time you walk into
it. A `~` before a value means it was invented; nothing here was observed.

The arithmetic is real. **Tier 1** interprets about seventy integer
instructions — the moves and their sub-register rules, add/sub/adc/sbb, the
logic three, the shifts, multiply and divide, the bit scans, the stack, and
`setcc`/`cmovcc`/`jcc` across all sixteen conditions — so a branch is decided by
the flags rather than by a coin. **Tier 0** is underneath it for everything else
and for every other architecture: Capstone says which registers an instruction
reads and writes and which flags it disturbs, so the fallback materialises the
reads, havocs the writes, and clears the known bit on the flags — `CF? ZF?`
rather than a number it cannot justify. That is why an AArch64 or MIPS binary
still gets a walker, from the same code, with no per-architecture work.

A run starts in a frame of its own: `rsp` in the middle of a synthetic
64 KiB stack with a **sentinel return address** already on it, `rbp` equal to
it, one constant **stack canary** at `fs:0x28`, and the file's
**relocations applied into the shadow** so the GOT resolves. A `call` is not
followed — the city keeps exactly one room decoded at a time — but its
*contract* is: `rax` is whatever it is pretending to have returned, the
caller-saved registers are gone, `rsp` is untouched. About thirty libc
callees have a return model, so `strcmp` can say "equal" and `getenv` can say
"nothing", which is the difference between a search loop that ends and one
that runs until its fuel does. When `ret` pops the sentinel, the function has
returned to its caller and the run is over.

Behind it trails the last sixteen states, as ghost cards hanging over the
sculptures they belonged to and fading with age — newest in full, older ones
collapsed to their mnemonic, orange where the branch was guessed rather than
worked out from the flags. The room then reads as a **timeline you can walk
under and look back along**, which is the one thing a static listing cannot do
at all. Close up you get the full panel; past eight metres it collapses to the
token and its mnemonic; past twenty-five, to the tether alone.

### Following the code through the city

`P` starts the walk the whole thing exists for. It puts you at the file's
entry point, starts a wisp there, and hands it the wheel: when a run reaches a
way out of the room, the app does exactly what stepping through that port by
hand does — you *walk through the door* — and the run continues in the new room
with the same CPU state. That state owns nothing belonging to a decoding, so it
survives the move for free, and because the player moves too, the city's rule
that exactly one room is decoded at a time is never in question.

In pilot mode a call is really **made** rather than stepped over: the return
address goes on the synthetic stack, so `ret` pops it and brings you back to
the room you called from, with whatever the callee actually left in the
registers. Three things it declines to follow, because there is nothing there
to walk: a symbol this file does not define, a jump into one (a tail call —
the contract runs and the wisp returns instead), and an `R_X86_64_IRELATIVE`
resolver. The one exception is `__libc_start_main`, whose first argument is
`main` — the single hand-off every dynamically linked C program makes, and
without it a walk from the entry point ends after thirteen instructions.

### Step by step

`P` leaves the wisp **paused** on the entry point: nothing moves until you say
so. Then `T` steps and `I` steps in, and the difference is what they do at a
call. (`F10` and `F11` are bound to the same two, out of debugger habit. The
keys are letters on purpose: a keycode for a letter is the same key on every
layout, and punctuation is not — `.` on a French keyboard is Shift+`;`, which
arrives as a different keysym altogether.)

`I` makes the call and stops on the callee's first instruction, taking you
with it — one instruction, into another room.

`T` steps *over*: one instruction, except that a call which is really made is
not one instruction, and pretending otherwise is what a step-over contract
does. So the callee is actually run. You stay at the call site — the call keeps
its mark on the sculpture you are standing at — and the wisp goes off through
the doors on its own, at its own rate, with the readout saying which room it
has reached. `[J]` at any point takes you to it, and puts the run back under
your hand where it stands. Otherwise it comes back by itself: the moment the
call depth returns to what it was when you pressed the key, the wisp stops,
knocks at your door, and `[E]` picks it up again — paused, on the instruction
after the call, one address further on than where you left it.

That last sentence is the contract `--selftest` checks, on both keys, pressing
them through `app_key()` exactly as the event loop does:

```
ok   stepping      I lands inside the callee;  T runs the call whole and comes back one on
```

`--selftest` walks forty hops from `e_entry` on every binary it is given and
asserts at each one that the room it landed in really covers the address it
asked for and that exactly one room's worth of decoding is alive:

```
ls         40 hops from the entry point, 207 steps, 42 rooms, 2 calls deep
git        40 hops from the entry point, 207 steps, 42 rooms, 3 calls deep
bash       40 hops from the entry point, 155 steps, 42 rooms, 2 calls deep
vim        40 hops from the entry point, 326 steps, 42 rooms, 5 calls deep
python3    40 hops from the entry point, 254 steps, 42 rooms, 6 calls deep
```

`--vmtest` runs one in every code room and reports where they got to:

| | `ls` | `git` | `bash` | `codecity` | AArch64 |
|---|---|---|---|---|---|
| code rooms | 80 | 600 | 600 | 118 | 1 |
| handled by **tier 1** | 99.8% | **97.5%** | 99.7% | 80.1% | 0% |
| conditionals **decided** | 100% | 99.9% | 99.9% | 96% | 0% |
| ended at `ret` | 62% | 48% | 74% | 71% | 100% |
| left the room | 16% | 17% | 10% | 10% | 0% |
| ran out of fuel | 20% | 35% | 14% | 18% | 0% |
| calls named | 80% | 28% | 89% | 100% | — |

(that table is a **local** wisp — one room, calls stepped over, which is what
`X` gives you; pilot mode is the other half.)

`codecity` is the worst case in the table because it is float-heavy: scalar SSE
is tier 2 and has not landed. The AArch64 column is tier 0 doing exactly what it
is for. A running wisp costs **no measurable frame time**. `--bench` in a
1848-instruction room: 3.16 ms without one, 3.12 ms with one running, its
trail drawn and stepping an instruction every frame. It comes out marginally
*cheaper*, and for a reason worth knowing: a wisp's trail labels the same
sculptures the room's own control-flow mnemonics do, so those stand down while
one is walking — and the trail draws through the monospace atlas below, where
the labels it replaced went through the string cache.

### What a pointer points at

Two mechanisms find most of the text in a binary without a line of analysis:
a RIP-relative operand naming an address in `.rodata` — the compiler's
universal way of referring to a constant — and a relocation addend pointing
there, which is how every array-of-strings is built. `--vmtest` measures both:

| | strings in `.rodata` | by a `lea` | by a relocation | **by either** | left over |
|---|---|---|---|---|---|
| `git` | 12 950 | 85.6% | 17.1% | **99.8%** | 30 |
| `bash` | 1 526 | 58.2% | 45.3% | **98.5%** | 23 |
| `codecity` | 543 | 73.1% | 25.2% | **98.0%** | 11 |
| `libx264.so.164` | 749 | 86.9% | 11.7% | **96.1%** | 29 |
| `ls` | 479 | 36.1% | 14.8% | 50.7% | 236 |

So when a register holds a pointer into a mapped section, the panel can print
the string it names instead of sixteen hex digits. `ls` is the low outlier
because it is small and most of its `.rodata` is not strings at all — the
scan counts any printable NUL-terminated run of four bytes or more, and a
table of small integers can look like one.

### How much of a binary can be modelled

Four tiers. Tier 1 is the integer base; tier 2 is scalar SSE float; tier 3 is
the packed operations — bulk moves, shuffle/compare/logic, and the lane-wise
arithmetic that is the actual work of a video codec. Below them tier 0 havocs
precisely what Capstone says an instruction touches, on any architecture.

`--vmtest` classifies every instruction in a file's executable sections by
offering it to the real dispatchers on a throwaway machine — the classifier
*is* the interpreter, so the table cannot drift from the code:

| | instructions | tier 1 | tier 2 | tier 3 | **modelled** |
|---|---|---|---|---|---|
| `git` | 727 991 | 97.1% | 0.0% | 2.8% | **100.0%** |
| `bash` | 241 699 | 99.3% | 0.0% | 0.7% | **100.0%** |
| `libvlccore.so.9` | 182 502 | 97.2% | 0.5% | 2.2% | **100.0%** |
| `libavcodec.so.60` | 2 689 541 | 91.4% | 1.3% | 7.2% | **99.9%** |
| `libvpx.so.7` | 633 990 | 62.4% | 0.8% | 36.7% | **99.9%** |
| `codecity` | 47 606 | 81.6% | 13.4% | 4.9% | **99.9%** |
| `libswscale.so.7` | 135 012 | 85.3% | 0.8% | 13.2% | **99.3%** |
| `libx264.so.164` | 368 904 | 78.9% | 1.6% | 18.9% | **99.3%** |
| `libdav1d.so.7` | 293 494 | 52.6% | 0.0% | 42.6% | 95.1% |

`libvpx` is the encouraging case: 62% of it is integer and it still comes out at
99.9%, because an SSE2-era codec is almost entirely lane-wise arithmetic — add,
subtract, multiply-accumulate, average, sum-of-absolute-differences over packed
8- and 16-bit lanes. `libdav1d` is the hard one: AV1's SIMD is written for
AVX2 and AVX-512, and **AVX-512 is deliberately not modelled** — it needs a
`k` register file and predication threaded through every operation. What is
left unmodelled in `dav1d` is almost entirely that: `vmovdqa32` and `vmovdqu32`
are its two largest misses, and a further 13 526 bytes of it Capstone declines
to decode at all.

The same tier-3 loops run at 128 and at 256 bits — writing them over
*(element width, lane count, operand count)* is what makes AVX2 the same code —
and the hardware corpus is recorded 256 bits wide, so both are checked rather
than assumed. Legacy SSE and VEX differ in one respect that only a
256-bit-wide corpus can see: a VEX write zeroes everything above the
destination and a legacy one leaves it exactly as it was.

### Hardware as the oracle

Coverage is not correctness: 97% of instructions modelled is not 97% of runs
right, because one wrong `movzx` early poisons everything after it. So none of
tier 1's rules were written from the manual and left there.
`tools/gen-corpus.sh` builds a small RWX scratch page in a process under gdb,
writes one instruction into it, sets the sixteen registers and the flags to a
chosen vector, single-steps, and records what the CPU did.
`tests/vm-corpus.txt` is 10 584 such vectors over 441 instruction forms and 24
states — sixteen general registers, the flags, eight 256-bit vector registers
and a 512-byte window of memory — checked in as data; `--vmtest` replays it
through the interpreter and fails on any divergence. It needs neither gdb nor an x86 host to
run, which is the whole reason the corpus is a file.

Five rules came out of that and would otherwise have shipped as *plausible wrong
numbers* rather than crashes:

- a shift count is masked to **5** bits at width 4 and **6** at width 8, so
  `shl eax, 33` shifts by one;
- a shift of zero changes no flag — but still writes the destination, so
  `shl eax, 0` zero-extends the top half of `rax` anyway;
- `inc` and `dec` leave `CF` exactly as they found it;
- `bsf`/`bsr` leave their destination **unchanged** when the source is zero;
- a register write at width 4 zero-extends the whole 64-bit slot, at 1 and 2 it
  merges — get that backwards and every pointer grows junk in its high bits;
- `minss` returns one of its operands **bit for bit**, not the smaller number:
  recomputing it through a `double` quiets a signalling NaN and changes the
  answer;
- `cvttss2si` of a NaN or an out-of-range float gives the *integer indefinite*
  value, not whatever a C cast happens to do — which was returning zero;
- a float operation on a NaN returns **that NaN's bits**, quieted; computing it
  through a `double` and converting back invents a different NaN;
- `ror eax, 0` rotates by nothing and sets no flag — and still writes its
  destination, so it zero-extends `rax`, exactly as `shl eax, 0` does.

Where the manual says a flag is *undefined*, the interpreter says so too rather
than copying whatever this CPU happened to leave: asked 4872 times, hardware's
`OF` after a shift by more than one is not a function of the inputs (no rule
explains more than 65 of 87 `shl` vectors), so that flag comes back unknown and
the `?` on the panel is the honest answer. `--vmtest` prints how many such
comparisons were declined and for which instructions, so a missing rule cannot
hide behind one.

The generator itself needed a tripwire. Roughly one `stepi` in a hundred on this
gdb resumes from the previous stop instead of the program counter just set,
which silently records the wrong answer — it first showed up as a `pop` that
appeared to *push*. The scratch page is therefore padded with `cld`, `DF` is set
before every step, and any vector that comes back with `DF` clear has run a pad
byte and is redone.

### Text that changes every frame

The string cache keys on content, so a readout that is different every frame is
a TTF rasterization and a `glTexImage2D` upload every frame — and, worse, it
evicts the room's own labels on the way past. Anything of that shape draws out
of a monospace atlas instead: one texture, one quad per glyph, no rasterization
at all. `--textbench` draws the same 200 never-repeated strings both ways:

| path | ms/frame | vs. no panel | cache entries | cache MiB | strings rasterized |
|---|---|---|---|---|---|
| no stress panel | 0.32 | — | 38 | 2.4 | 0 |
| monospace atlas | 0.51 | +0.19 | 38 | 2.4 | **0** |
| string cache | 6.84 | +6.52 | 520 | 28.0 (its ceiling) | **43 200** |

Two hundred strings is ~4800 glyphs; through the atlas that is one bind and one
`glBegin(GL_QUADS)` run per string, and the cache does not move. Through the
cache it is 216 rasterizations and as many uploads a frame, 20× the frame cost,
and the cache is pushed to its byte ceiling — which is where the room's signs
and nameplates used to be. `F2` shows the same thing live, and adds a
world-space panel in front of the player for the `text_mono_3d` half.

### Focus, and the alt-tab freeze

SDL picks the **x11** driver here even in a Wayland session, so the window is an
XWayland client and `SDL_SetRelativeMouseMode(SDL_TRUE)` — mouse-look — is an
`XGrabPointer`. The event loop used to handle only `SDL_QUIT` and resize, so the
grab was held across a focus change: alt-tab handed the desktop to GNOME Shell
while this window still had the pointer grabbed, which is a well-worn way to
lock a whole session up for a few seconds, or for good.

The loop now handles focus and visibility:

- `FOCUS_LOST` drops relative mouse mode, so the grab is never held by a window
  the compositor is switching away from. `FOCUS_GAINED` takes it back, and
  swallows the first motion event so the camera does not jump by however far the
  pointer travelled while we were away.
- `MINIMIZED`/`HIDDEN` stops drawing altogether and waits on the event queue
  instead. A window nobody can see gets no frame callbacks, so swapping into one
  can block for as long as it stays hidden — a wait we cannot be woken from.
  Measured in a nested X server: 350% CPU visible, **0% hidden**, straight back
  to 350% on restore.
- Unfocused but visible throttles to about 40 fps. There is nothing to animate
  for behind another window.

`SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR` is also turned off before
`SDL_Init`. It defaults to on, which asks the compositor to unredirect the
window; that is for fullscreen games, and it makes Mutter redirect and
unredirect on every focus change.

Every frame now times `SDL_GL_SwapWindow`. Anything over 250 ms prints

```
stall: 3180 ms inside SDL_GL_SwapWindow -- the frame was drawn, the compositor
or driver held it
```

and the worst handover so far shows under the fps readout. That is the line that
tells the two cases apart: a stall inside the swap is not this program's frame
time, it is the frame being drawn and then not shown.

`--gpudebug` puts `MESA_DEBUG=1` and `GALLIUM_HUD=fps,VRAM-usage` in the
environment before SDL brings the driver up, which is the only moment Mesa
reads them. It is for telling a stall in this program from a stall in the
driver: if the HUD's fps keeps climbing while the window is frozen, the
frames are being drawn and something downstream is holding them.

`VRAM-usage` is a radeonsi/nouveau query — the Iris driver does not export it
and prints `gallium_hud: unknown driver query 'VRAM-usage'`, leaving just the
fps graph. An existing `GALLIUM_HUD` in the environment is left alone, so on
Intel the useful form is:

```sh
GALLIUM_HUD=fps,frametime,cpu ./codecity --gpudebug FILE
```

### The glyph cache

Text is the one part of a frame that can allocate, and it used to be bounded
only by a count of 1400 entries. A 190-character listing line rasterizes to
about half a megabyte of texture, so standing in a `.dynstr` or `.symtab` tower
could hold a quarter of a gigabyte of GL textures and churn tens of thousands of
`glTexImage2D`/`glDeleteTextures` calls while walking. Worse, the sweep evicted
anything not touched in the last 700 lookups — so once a frame needed more than
700 strings, it evicted the strings it was still drawing, and every frame
re-rasterized hundreds of them: measured 0.7 ms/frame before that threshold and
20.7 ms/frame after, for the same view, and it does not recover until you walk
away. The cache now has a byte budget as well as an entry budget, evicts by true
recency, refuses to evict anything the frame in progress has already drawn, and
steps a very long string down to a half-size face rather than ever rasterizing a
texture wider than 1280 texels. A listing tower's whole working set measures
about 21 MB.

---

# Current limitations

## What is not modelled

- **Relocations are read but never used for geometry.** `.rela.dyn` and
  `.rela.plt` are decoded into readable text on a wall, and that is all. Nothing
  in the city is connected because of a relocation, which is the single biggest
  gap — see the ideas below.
- **Doors between rooms mean adjacency, not reference.** A door in a party wall
  is drawn when two symbols are within 16 bytes of each other in the file. It
  says "these are neighbours", not "this one calls that one".
- **No cross-tower links at all.** A `call` into another section ends at the
  terminal over the doorway. Nothing physically joins two towers.
- **`.interp` is parsed but is not a gateway,** even though the dynamic loader it
  names is a file on disk that the explorer could load.
- **`DT_RPATH`/`DT_RUNPATH` are parsed but not used to resolve gateways.**
  Library lookup is seven hardcoded directories (`/lib/x86_64-linux-gnu`,
  `/usr/lib/x86_64-linux-gnu`, `/lib64`, `/usr/lib64`, `/lib`, `/usr/lib`,
  `/usr/local/lib`); no `ld.so.conf`, no `$ORIGIN`, no multiarch beyond x86-64.
- **No DWARF.** The DEBUG district is zoned and sized correctly, but its contents
  are shown as raw bytes.

## Caps that silently truncate

| cap | value | effect |
|---|---|---|
| rooms per tower | 1400 | a larger section simply stops getting rooms |
| floors per tower | 34 | rooms are packed more densely per floor instead |
| instructions per code room | 640 | the plate says the room is deeper than what is shown |
| lines per table room | 22 | each board shows a slice; the rest are in other rooms |
| sections | 400 | later section headers are ignored |
| program headers | 64 | ditto |
| `DT_NEEDED` gateways | 128 | ditto |
| collision boxes in range | 4096 | walls beyond that would not stop you |
| cached text textures | 1400 | evicted round-robin, so a very busy frame re-rasterises |

## Disassembly

- Only **direct** branches get a wire. Branch targets are recovered by reading the
  printed operand, so an indirect `call rax`, a jump table, or a PLT thunk's
  indirect jump shows no link.
- A room is decoded as a straight run from its first byte. Data embedded in a code
  section (jump tables, alignment islands) is decoded as if it were instructions,
  and Capstone will happily produce nonsense for it.
- Capstone 4.0.2 covers x86, x86-64, ARM, AArch64, MIPS, PowerPC, SPARC and S390.
  Only the two x86 modes have been exercised on this machine. RISC-V needs
  Capstone 5.
- Big-endian ELF parsing is implemented but has never been run against a real
  big-endian file.

## Rendering and interaction

- **No frustum or occlusion culling.** Visibility is plain radius checks, so
  standing inside a tower still pays for every building within 1200 m and every
  sign within 500 m.
- **No way to search for a symbol.** If you want to find `malloc` you have to
  know which tower and floor it is on and walk there.
- **Nothing is selectable.** You can read what you stand next to, but you cannot
  pick a thing and keep it highlighted while you walk elsewhere.
- **No saved position.** Every load starts you back on the plaza.
- Screenshots are written as PPM, so they need converting before most viewers
  will open them.
- `Esc` releases the mouse on the first press and quits on the second, which is
  easy to trip over.

---

# Ideas for improvement

## Non-code sections as interactive sculptures

This is where the program is weakest. Code rooms come alive when you enter them;
everything else is a board of text or a mosaic of coloured squares. But most of
these sections *are* data structures — tables, arrays, graphs — and a data
structure built at walking scale is a better explanation than any diagram.

The ordering below is roughly by value for effort.

### `.got`, `.got.plt`, `.data.rel.ro` — a patch bay

The most opaque part of a binary, and the one that would gain most. Every
pointer-sized slot becomes a socket in a floor-mounted panel. The relocation
tables say exactly which slots get written at load time and with what, so any
slot covered by a relocation grows a cable that rises out of the socket and runs
off toward its target, coloured by relocation type:

- `JUMP_SLOT` — lazy PLT binding, resolved on first call
- `GLOB_DAT` — a data symbol from another object
- `RELATIVE` — self-relative, no symbol; the cable loops back into this same city
- `IRELATIVE` — an ifunc resolver; draw the far end as a call beacon
- `COPY` — a symbol copied in from a library at load

Stand at a socket and the HUD shows the relocation and the symbol it resolves to.
All the data needed for this is already parsed.

### `.rela.dyn`, `.rela.plt` — the same switchboard from the other end

These are currently text boards listing the same relocations. They should be the
other terminal of the patch-bay cables: one socket per relocation, wired to the
place being patched. Walk into `.rela.plt` and you are standing at the switchboard
that wires `.got.plt` to the outside world.

### `.dynsym`, `.symtab` — a registry hall with working links

One pillar per entry: height from the symbol's size, colour from its type
(function, object, TLS, ifunc), finish from its binding (global polished, local
matte, weak translucent). The interesting part is the interaction — press the
follow key on a pillar and travel to the room where that symbol actually lives.
The symbol table becomes what it already is: a working directory of the city.

### `.gnu.hash`, `.hash` — a machine you can operate

The best candidate for a genuinely *interactive* sculpture, because it is a
lookup algorithm rather than a list. Build it physically: the Bloom filter as a
wall of lamps, one per bit, lit where the bit is set; the bucket array as a row of
numbered pipes; the chain array as beads inside them.

Then let the visitor run a query. Point at a symbol name — or type one — and watch
the lookup happen: the two Bloom bits light, the bucket lights, the chain walks
bead by bead until it hits or falls off the end. Nobody understands `.gnu.hash`
from reading the spec; everybody would understand it after operating this once.

### `.init_array`, `.fini_array`, `.preinit_array` — a starting grid

An array of function pointers run before and after `main`. Numbered launch pads in
a row, in execution order, each with a cable to the constructor it points at (from
the `RELATIVE` relocation that fills it in, for a PIE). Walking the row is walking
the program's startup sequence.

### `.dynstr`, `.strtab` — library stacks that show the sharing

Shelves of spines, one per string, width from its length. The detail worth
building is that ELF string tables use **suffix merging** — `printf` may be
nothing more than the tail of `sprintf`. Draw shared suffixes as telescoping
segments of one spine and the sharing becomes visible, which it never is in a
hex dump. Point at a spine to highlight every symbol whose name offset falls
inside it.

### `.rodata` string literals — a gallery

Scan for printable runs, and each literal becomes a lit banner: height from
length, colour by class (plain text, a format string containing `%`, something
that looks like a path, UTF-8). With the reverse index below, each banner also
carries inbound cables from the functions that reference it.

### `.dynamic` — a control room

Already the best of the non-code rooms, but it could stop being a board. Each tag
becomes a labelled instrument: string-valued tags (`NEEDED`, `SONAME`, `RPATH`)
as signs, and address-valued tags (`STRTAB`, `SYMTAB`, `HASH`, `JMPREL`) as cables
leaving the room toward the towers they point at.

### `.eh_frame`, `.eh_frame_hdr` — scaffolding

Every FDE describes how to unwind one function's stack frame. Draw a scaffold per
FDE, its height the frame size and its rungs the CFA rules, with a beam back to
the function it covers. `.eh_frame_hdr` is a sorted binary-search table, so it
becomes a card catalogue standing in front of the scaffolding. Needs a DWARF CFI
reader, which is the most work of anything in this list.

### `.bss` — a room full of correctly sized ghosts

Currently a single wireframe cube. It should be one hollow volume per symbol, at
true relative size, labelled and faintly shimmering: *this much memory appears at
load time and every byte of it is zero*. The emptiness is the content.

### `.note.*`, `.comment` — engraved tablets

Cheap and charming. One stone tablet per note with its name and payload:
`.note.gnu.build-id` as a monolith carrying the ID, `.note.gnu.property` as a row
of switches for the IBT and shadow-stack flags, `.comment` as a plaque naming the
compiler that built the file.

### `.gnu.version`, `.gnu.version_r` — banners over the pillars

Version tags hung over the symbol pillars they apply to, and a plaque per needed
library listing the versions it demands.

## The mechanics that would make all of that work

Four pieces of plumbing, none of them large, that most of the ideas above depend
on:

1. **An address→room index.** A sorted map from virtual address to
   `(building, room)`. Everything else is built on this.
2. **A follow key.** One verb — *go to what this points at* — that works on a
   branch wire, a `.got` socket, a symbol pillar, a relocation, an `init_array`
   pad. It turns the city from a place you wander into a graph you navigate.
3. **A reverse index.** Built once at load from the relocations plus every direct
   branch immediate: for any address, who points at it. Then any room can grow
   inbound cables. Standing in a function and seeing every one of its call sites
   arriving as a wire would be the most striking thing in the program.
4. **Selection.** Pick a thing and keep it lit while you walk away — highlighted
   in its room, on its tower, and on the minimap.

Plus the obvious companion: **search**. Type a name, get taken to it.

## Code rooms

- **Basic blocks instead of a flat ribbon.** Split each function at its branch
  targets and lay the blocks out as a real control-flow graph, so a room's floor
  plan *is* the CFG rather than a serpentine that happens to have wires over it.
  Loops would become visibly circular.
- **Follow the linear sweep with a smarter one.** Decoding from the first byte
  onward turns embedded jump tables into garbage instructions. Starting from
  known symbol entry points and following branches would decode only what is
  really code, and mark the rest as data.
- **Indirect branches.** At least detect them and draw a stub wire that says
  "somewhere I cannot see", rather than nothing at all.

## Across the city

- **Skybridges.** A call from one section into another becomes a bridge between
  the two towers. The city's street layout would then reflect the call graph, and
  you could walk from a caller to a callee without going outside.
- **Relocation doors.** A `JUMP_SLOT` relocation physically connects a `.plt`
  room to the `.got.plt` slot it patches — a door between two towers that exists
  because the linker said so.
- **The dependency graph as a region, not a gateway.** Rather than loading one
  library at a time, lay the whole `DT_NEEDED` closure out as neighbouring cities
  with roads between them.
- **`.interp` as a gateway**, since the loader it names is a file that can be
  loaded.
- **Honest library resolution** — `DT_RUNPATH`, `$ORIGIN`, `ld.so.conf`, other
  architectures — so gateways stop being marked "not found on this system" when
  the library is plainly installed.

## DWARF

- **`.debug_line`** gives a source file and line for every address. Painting that
  onto the code-room sculptures would let you stand in the machine code and read
  which line of C you are standing in.
- **`.debug_info`** is a tree of DIEs. Draw it as an actual tree you can walk
  under, with types as the leaves — the DEBUG district stops being a warehouse of
  bytes and becomes the most information-dense part of the city.

## Engineering

- Frustum and portal culling; the radius checks are leaving a lot of frame time
  on the floor and will not survive skybridges or inbound cables.
- Streaming past the room and instruction caps instead of truncating, now that
  the enter/leave lifecycle proves per-room allocation works.
- Remember where you were, per file.
- Write screenshots as PNG.
- Make `Esc` only ever release the mouse, and leave quitting to `Ctrl+Q`.
- Test the other Capstone architectures and a real big-endian file.

---

# Contributing

**Please fork it, change it, and open a pull request.**

The list above is a wish list, not a plan — nothing in it is claimed, and none of
it has to be done in the order given. If one of those sections catches your eye,
take it. If you have a better idea for what a `.got` table or a hash bucket should
look like when you can walk around it, that is exactly the kind of pull request
this project wants.

Ideas are as welcome as code. Open an issue, or open a pull request that only
edits the improvements section of this README — a well-argued idea for turning
some corner of an ELF file into something you can walk through is worth as much
here as an implementation.

Some things that make a change easy to take:

- `make` should stay warning-free with `-Wall -Wextra`.
- `./codecity FILE --selftest` should still pass. It is cheap to run over a lot of
  files at once, and it catches broken geometry and leaked room decodings that
  are invisible from a screenshot.
- If you add something visual, `--shot` is the quickest way to show what it looks
  like in a pull request.
- Keep the metaphor honest. The rule the whole thing rests on is that the shape
  of a room reflects what is really in the file — a tower is tall because the
  section needs that many floors, not because tall looks good.

# License

[MIT](LICENSE). Do what you like with it — use it, change it, ship it, sell it —
as long as the copyright notice comes along. No warranty.

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
| `Ctrl+Q` | quit |

## Source layout

| file | what it does |
|---|---|
| `src/elfload.c` | ELF32/64, little- and big-endian reader over an `mmap`; sections, segments, symbols, `.dynamic`, address→symbol lookup |
| `src/disasm.c` | Capstone wrapper: one handle per architecture, a room decoded into classified instructions with their branch targets resolved |
| `src/city.c` | the mapping above — rooms, floors, tower proportions, district and city packing, lazily decoded table text, and the enter/leave lifecycle of a code room |
| `src/world.c` | wall and slab geometry, the spiral-stair height field, collision, player physics |
| `src/render.c` | the drawing — exteriors baked into display lists, interiors drawn per floor |
| `src/text.c` | strings rendered by SDL2_ttf into cached GL textures, for signs, plates and the HUD; the cache is bounded in bytes as well as entries, and never evicts a string the frame in progress has already drawn |
| `src/hud.c` | readouts, minimap, detail sheet, help, file browser |
| `src/main.c` | window, event loop, file loading, and the three headless modes |

## Headless modes

```sh
./codecity FILE --selftest   # walks in the front door, up the spiral to floor 3, back
                            # down one, along the corridor and through a room door, for
                            # the first 8 towers; asserts that entering a room
                            # allocates one room's worth of decoding and leaving
                            # frees it, and that every port in a code room can be
                            # aimed at and leads to a room that holds its address.
                            # Non-zero exit on failure.
./codecity FILE --shot DIR   # renders fourteen canned viewpoints to DIR/*.ppm
./codecity FILE --bench      # frame time in the busiest code room it can find
```

`--selftest` passes on 60 binaries and libraries from `/usr/bin` and
`/usr/lib/x86_64-linux-gnu`, and on the kernel's 32- and 64-bit vDSOs, which is
what exercises Capstone's `CS_MODE_32` path. Non-ELF files are refused with a
message.

`--bench` in a 640-instruction room with 251 wires: ~405 fps, 2.5 ms/frame on
Mesa/Iris Xe. The live app is vsync-capped to 60.

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

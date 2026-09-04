# Sizing rooms from content, and packing them into floors

> **Implemented.** All of it, in `src/ehframe.[ch]` (new) and
> `model.h`, `city.c`, `world.c`, `render.c`, `main.c`, `hud.c`.
> `./elfcity --stats FILE` prints the layout the packer produced.
>
> | | rooms | chambers (units) | recovered | tallest | far plane |
> |---|---|---|---|---|---|
> | `elfcity` | 291 | 138 (934) | 3 | 26 m / 6 fl | 1904 m |
> | `git` (stripped) | 3156 | 568 (3959) | 2335 | 1058 m / 264 fl | 2189 m |
> | `ssh` (stripped) | 643 | 116 (798) | 402 | 182 m / 45 fl | 1854 m |
> | `busybox` (static) | 1948 | 525 (3647) | 1401 | 726 m / 181 fl | 1949 m |
>
> No floor falls outside 1..10 rooms, `--selftest` passes on all of them plus
> a Go binary, and the build is clean under ASan and UBSan. Layout is 27 ms
> for `git`.
>
> **One correction to §3 and §4, found by running it.** Sizing the tile from
> the *largest* room (to cap it at `PLATE_MAX_SIDE`) squeezed every other room
> down to the minimum: `git`'s `.text` came out with a 26 m plate, rooms
> averaging 6.3 m, 6.2 rooms per floor and an aspect ratio of **1:67** — a
> needle. Since a floor holds at most `MAXPF` rooms either way, shrinking the
> rooms buys nothing but slenderness. Two changes fix it:
>
> - **Size the tile from the median room, not the largest** (`ROOM_TYPICAL`,
>   7 m across), and treat `PLATE_MAX_SIDE` as a safety valve against one
>   monster function rather than a design driver — it went from 18 m to 40 m.
>   A per-building minimum room size, set at about half the typical room,
>   stops a section of uniformly tiny units becoming a stack of closets.
> - **Grow and repack until the floor count stops falling.** The area bounds
>   of §4 can still leave a plate that does not *geometrically* hold ten
>   rooms — one wide room blocks a whole row — which is why uniform sections
>   like `.bss` and `.rela.dyn` were stuck at 4.0 rooms per floor. A single
>   12% step often fails to cross a threshold, so the loop keeps going for up
>   to ten steps and keeps the smallest plate that achieved the best count.
>
> | `git` section | before | after |
> |---|---|---|
> | `.text` | 26 m plate, 428 fl, 1714 m, 1:67, 6.2/floor | **91 m, 264 fl, 1058 m, 1:12, 10.0/floor** |
> | `.rela.dyn` | 19 m, 1:11, 4.0/floor | **29 m, 1:3, 9.8/floor** |
> | `.bss` | 17 m, 1:3, 4.0/floor | **24 m, 1:1, 8.8/floor** |
>
> Across the whole `git` city that is 548 floors down to 336, with 298 of them
> holding the full ten rooms.
>
> **A pre-existing bug fell out of this.** The spiral stair generated a full
> extra turn *above* the top landing (`stair_height()`, `src/world.c`), so in
> any building you could climb past the top floor onto treads that led
> nowhere and be stranded. It was invisible only because the self-test
> skipped buildings under three floors and stopped climbing at floor 3. The
> stair now stops at `(nfloors-1) * FLOOR_H`, and the test climbs to each
> building's own top.

---

## 0. Where the code is today

`build_one()` in `src/city.c:388` does all of the layout. Its current model:

| Thing | Today | Where |
|---|---|---|
| Rooms per floor | fixed `per = 8`, or up to 44 for large sections | `src/city.c:429-443` |
| Room width | `clamp(2.8 + 1.15·log2(size), 2.8, 15)`, then stretched to fill the corridor | `src/city.c:453`, `:466-477` |
| Room depth | `ROOM_D`, a `#define` — every room, every floor | `src/model.h:97` |
| Room position | `x0, x1` along the corridor plus `side` (0 = −Z, 1 = +Z) | `src/model.h:126-127` |
| Building box | `w = CORE_W + len`, `d = CORR_W + 2·ROOM_D` | `src/city.c:491-493` |
| Floor assignment | sequential slicing: rooms `f·per .. (f+1)·per` | `src/city.c:439-442` |

So there is no packing at all today, and room *area* carries no information: the depth is
constant and the width is logarithmic and then re-scaled to justify against the corridor.
A 64-byte function and a 13 KiB function differ by a factor of about 2 in floor area,
when they differ by a factor of 212 in size.

The far-wall mosaic is `case RT_BYTES` at `src/render.c:639`: 96 columns × 20 rows of
quads on the plane `z = zFar`. In a stripped `.text` *every* room is `RT_BYTES` — no
symbols means `rooms_from_bytes()` — so today a stripped binary's code district is
entirely wall mosaics. That is the thing to move down.

---

## 1. One tile, one unit of content

Everything below hangs off a single number per room: **`ntiles`, how many units of
content the room holds**. Define a unit per room kind so that one tile is one thing a
person would point at:

| Room kind | One tile is | Estimate before decoding |
|---|---|---|
| `RT_FUNC` | one instruction | `ceil(size / avg_insn_len)`; 4 for x86-64, 4 for fixed-width RISC, 2 for Thumb |
| `RT_OBJECT` | one 16-byte line | `ceil(size / 16)` — this is already what the crate loop at `src/render.c:608` does |
| `RT_BYTES` | one 16-byte line | `ceil(size / 16)` |
| `RT_LIST` | one printed line | `hi - lo`, already known |
| `RT_EMPTY` | — | 1 |

`RT_FUNC` has to be *estimated*, because the room is sized in `city_build()` and the
disassembly only happens on entry (`city_enter_room()`, `src/city.c:343`). That is fine:
the estimate only has to be good enough to size the room, and the serpentine layout in
`layout_code()` (`src/render.c:295`) already copes with the real count being off — it
shrinks the pitch until the instructions fit. Better: once the room is sized from a tile
grid, `layout_code()` should *use* that grid instead of re-deriving one, so the sculptures
land on the same squares as the mosaic.

Clamp: `ntiles = clamp(estimate, 1, TILE_MAX)`. See §3 for how `TILE_MAX` is chosen.

---

## 2. The most-filled rectangle

Given `n` tiles, find the smallest rectangle `w × h` with `w·h ≥ n` that is close to square.

```c
/* near-square rectangle holding at least n tiles; w is the long side */
static void tile_rect(int n, int *w, int *h){
    int a = (int)sqrt((double)n);
    if (a < 2) a = 2;                  /* no 1-wide slivers: you must fit through it */
    int b = (n + a - 1) / a;           /* ceil(n / a) */
    if (b < a){ int t = a; a = b; b = t; }
    *w = b; *h = a;                    /* long side along the corridor */
}
```

That is the rule the brief describes, and it reproduces both worked examples:

| n | `floor(sqrt(n))` | rectangle | cells | waste |
|---|---|---|---|---|
| 15 | 3 | 3 × 5 | 15 | 0 |
| 16 | 4 | 4 × 4 | 16 | 0 |
| **17** | **4** | **4 × 5** | **20** | **3** |
| 23 | 4 | 4 × 6 | 24 | 1 |
| 214 | 14 | 14 × 16 | 224 | 10 |
| 3394 | 58 | 58 × 59 | 3422 | 28 |

Checked exhaustively for n = 1…4096:

- worst relative waste above n = 30 is **under 13%**, and it falls as n grows;
- the aspect ratio never exceeds **2.0**, and is under 1.2 for n > 100;
- no exhaustive search is needed — trying `a-2 … a+2` and keeping the minimum-waste
  option changes essentially nothing, so the closed form is the right choice.

**Long side along the corridor.** `w ≥ h` matters: the corridor direction is cheap
(each floor extends it independently and the packer can absorb it), while depth is
expensive (the building shell is one box, so the *deepest room in the whole building*
sets the depth of every floor). Putting the long side along X keeps buildings shallower.

---

## 3. Tiles to metres

```
TILE      0.55 m        one tile, comfortably readable while walking past
MARGIN    0.70 m        clear strip between the grid and the side/back walls
SETBACK   1.20 m        clear strip inside the door, so you can step in and turn

room width  w_m = w·TILE + 2·MARGIN + 2·WALL_T
room depth  d_m = h·TILE + MARGIN + SETBACK
```

Both are clamped below at a minimum walkable room, about 2.6 × 2.4 m.

### How big may the biggest room be?

Strictly linear area over the real range of function sizes does not survive contact with
a walking camera. In `elfcity`'s own `.text`: median function 214 bytes, 95th percentile
5895, maximum 13577 — a 63:1 span within one section, and across a whole binary it is
worse. At one tile per instruction and 0.55 m per tile, the largest function alone would
be a 29 × 30 m room.

Two ways out, and the recommendation is the second:

- **Global cap** — `TILE_MAX` a constant, say 1600. Simple, but the cap is hit by
  different functions in different binaries, so "biggest room" stops meaning anything.
- **Per-building normalisation** — pick `TILE` per building so the largest room in it
  lands on a target footprint (say 18 × 18 m), and keep `ntiles` uncapped:

  ```c
  int nmax = max ntiles over the building's rooms;
  b->tile = clampf(sqrtf(TARGET_AREA / (float)nmax), 0.22f, 0.55f);
  ```

  Area stays *exactly* proportional to content within a building — which is the
  comparison a visitor actually makes, since they are inside one building at a time —
  and the building stays a sane size. The floor plate stops being comparable between
  buildings, which is a fair trade and is already true of the current log scale.

Either way `b->tile` becomes a per-building float and every consumer reads it, rather
than `0.46f` being hardcoded at `src/render.c:581`.

This choice and the plate rule of §4 are the same knob seen from two ends: `b->tile` fixes
how many metres the largest room needs, and the largest room fixes the plate, and the plate
fixes the whole building's footprint. Set `TARGET_AREA` here and everything downstream
follows without further tuning.

---

## 4. Packing rooms into floors

### The insight: the largest room already paid for the floor plate

The building shell is one box, so **every floor has the same footprint, and that footprint
must contain the largest room in the building**. Once you have paid for that area, every
other floor gets it for free. So do not treat the floor size as a free variable to be
searched — derive it from the largest unit, and then spend the resulting space by packing
the smaller functions in around the big ones as filler.

This is better than growing a corridor until the floor count comes down, which is what an
earlier draft of this note proposed. Measurements are in [The trade-off, measured](#the-trade-off-measured);
the short version is that plate-from-largest builds the same city in **a third of the
volume** at **four times the fill**.

It also has a property worth having for its own sake: function sizes are heavily skewed —
in `elfcity`'s `.text`, the median function is 214 bytes and the largest is 13577, and the
largest *room* is 32× the median room by area. A distribution with a few huge items and a
long tail of small ones is the best case for first-fit-decreasing: the big items define the
shape, and there is always something small enough to drop into the leftover. Uniform item
sizes are the bad case, and §"Two bounds" below is what stops that case degenerating.

### Two bounds on the plate, not one

The naive reading — plate = the largest room's rectangle — has a failure mode that shows up
immediately on real input. A stripped section produces rooms that are all *the same size*
(`rooms_from_bytes()` emits equal chunks, `src/city.c:159`). Then the largest room is the
typical room, the plate fits exactly one of them, and you get **one room per floor**:
simulated on 120 uniform 4 KiB chunk-rooms, that is 120 floors instead of 12.

So the plate needs a second bound — enough area to actually hold a full quota of rooms:

```c
float A = fmaxf( wmax * dmax,              /* must contain the largest room     */
                 MAXPF * mean_room_area ); /* must hold a full floor of typical */
A /= FILL_TARGET;                          /* ~0.62: corridors and packing loss */
float side = sqrtf(A);
b->plateW = fmaxf(wmax + CORR_W + 2*WALL_T, side);
b->plateD = fmaxf(dmax + CORR_W + 2*WALL_T, A / b->plateW);
```

Both bounds are load-bearing, each in a different regime:

| Section | largest-room bound | 10-room quota bound | binds |
|---|---|---|---|
| `elfcity .text`, skewed | 1166 m² | 370 m² | largest room |
| stripped `.text`, uniform | 382 m² | 3820 m² | quota |

### The algorithm — shelf rows, big first

The packing must be **shelf (guillotine) packing, not free-form rectangle packing.** Not
for speed — because of corridors. A maxrects packer will happily place a room in the middle
of the plate with no way to reach it. Shelf rows give every room corridor frontage by
construction: the floor is a comb, with rows of rooms separated by straight corridor runs.

```
+---------------------------------------------+
|  corridor                                   |   <- row 0 corridor
|  +--------------+ +-------+ +-----+ +----+  |
|  |              | |       | |     | |    |  |   <- row 0: big room, then
|  |   .text_big  | | helper| |sub  | |aux |  |      smaller ones as filler
|  |              | +-------+ +-----+ +----+  |
|  +--------------+                           |
|  corridor                                   |   <- row 1 corridor
|  +-------+ +-------+ +--------+             |
|  |       | |       | |        |             |   <- row 1
+---------------------------------------------+
```

```c
sort rooms by area descending
for each room r:
    for each open floor f:
        if f.count == MAXPF: continue
        for each row in f:
            if row.used + r.w + WALL_T > plateW: continue
            if r.d <= row.depth:                    /* fits under the row's ceiling */
                place in row; goto placed
            if f.usedD - row.depth + r.d + CORR_W <= plateD:
                row.depth = r.d;                    /* deepen the row to take it */
                place in row; goto placed
        if f.usedD + r.d + CORR_W <= plateD:
            open a new row in f; place; goto placed
    open a new floor
```

Two details that matter more than they look:

- **Rows may grow.** Fixing a row's depth from its first item wastes the tail of every row.
  Letting a row deepen while the plate still allows it is four lines and worth several
  percent of fill.
- **Sort by area, not width.** Because §2 makes both sides scale as `√n`, sorting by area,
  by width, or by depth-then-width produced *identical* packings on `elfcity`'s 98
  functions. Area is the one that also happens to be the right tiebreak for the row-growth
  rule, so use it and stop thinking about it.

**Address-adjacency tiebreak.** When two rooms have near-equal area, prefer the one whose
address is closest to a room already on the floor. Static helpers usually sit next to their
caller in the file, so this makes "the big function and the little functions it calls"
land on the same floor for free. It costs one comparison and it is the difference between
a floor being a bag of rooms and a floor being a neighbourhood.

### Grouped rooms: small units share a chamber

A room holding four instructions is not worth walking into, and there are a lot of them.
The distribution is lopsided in a useful way — measured on FDE-recovered functions:

| | `git` | `ssh` | `elfcity` |
|---|---|---|---|
| functions | 4472 | 704 | 93 |
| median size | 271 B | 302 B | 218 B |
| ≤ 128 B — share of **functions** | 22.1% | 18.9% | 35.5% |
| ≤ 128 B — share of **bytes** | **2.1%** | **1.7%** | **2.3%** |
| ≤ 256 B — share of **functions** | 47.9% | 43.2% | 54.8% |
| ≤ 256 B — share of **bytes** | **9.0%** | **7.8%** | **6.5%** |

**Half the functions carry a tenth of the code.** So merging the small half costs almost no
represented area and halves the room count.

Give them a chamber each holding several units, laid out as **3 × 3 cells with the centre
left open**:

```
        +-------+-------+-------+
        | unit1 | unit2 | unit3 |
        +-------+-------+-------+
        | unit8 |  ///  | unit4 |     /// = circulation, you stand here
        +-------+-------+-------+
        | unit7 | DOOR  | unit5 |
        +-------+-------+-------+
```

Eight perimeter cells; the door takes the middle cell of the corridor-facing wall, leaving
**7 units**. When the room also carries an enfilade door to its neighbour (the
`linkNext` contiguity door, `src/city.c:478`), that takes the opposite middle cell and the
capacity drops to **6**. Fewer still if a unit is big enough to want two cells.

Details that follow from the shape:

- **Every cell is the same size**, set by the largest unit in the group, so group units of
  similar size together — sort the smalls descending and take them seven at a time, which
  is one line and keeps waste low.
- **A grouped room does not use `tile_rect()`.** Its grid is `3·cellW × 3·cellH` tiles with
  a hole in the middle, which is deliberately not the near-square rectangle of the total.
  §2 applies to the *cell*, not to the room.
- **`Room` gains a unit list.** A single-function room is just the `nunits == 1` case, which
  unifies the two paths rather than adding a room kind. `city_enter_room()`
  (`src/city.c:343`) then decodes the nearest alcove rather than "the room", extending the
  `a->nearIns` proximity logic that already exists.
- **Labels move inside.** `draw_room()` writes one nameplate over the door
  (`src/render.c:545`); a grouped room needs a small plaque per alcove instead.

Measured on `git`'s `.text`, sweeping the "small" threshold:

| threshold | rooms | grouped rooms | plate | floors | height |
|---|---|---|---|---|---|
| none | 4472 | 0 | 50 × 50 m | 476 | 1904 m |
| ≤ 64 B | 4049 | 71 | 50 × 50 m | 436 | 1744 m |
| ≤ 128 B | 3626 | 141 | 50 × 50 m | 394 | 1576 m |
| **≤ 256 B** | **2638** | **306** | **50 × 50 m** | **298** | **1192 m** |
| ≤ 512 B | 1784 | 448 | 55 × 55 m | 205 | 820 m |

**≤ 256 B (~64 instructions) is the sweet spot**: it removes 41% of the rooms and 37% of the
floors while leaving the plate untouched. At 512 B the plate starts growing, because a 3 × 3
chamber of 512-byte units is now bigger than the largest single function — that is the point
where grouping starts costing footprint instead of saving it.

### The trade-off, measured

`elfcity`'s `.text`, 98 functions, floor-count lower bound `ceil(98/10) = 10`, largest room
34.5 × 33.8 m. `slack` is the multiplier on the plate area `A` above:

| slack | plate | floors | room fill | corridor | volume |
|---|---|---|---|---|---|
| **1.0** | **39.1 × 38.4 m** | **13** | **50.2%** | 19.2% | **19 500 m³** |
| 1.6 | 43.2 × 43.2 m | 13 | 40.4% | 17.1% | 24 200 m³ |
| 2.0 | 48.3 × 48.3 m | 12 | 35.0% | 14.5% | 27 900 m³ |
| 2.5 | 54.0 × 54.0 m | 11 | 30.5% | 13.5% | 32 000 m³ |
| 4.0 | 68.2 × 68.2 m | 11 | 19.1% | 9.1% | 51 200 m³ |

Against the corridor-spine model of the earlier draft, on the same input:

| | plate | floors | room fill | volume |
|---|---|---|---|---|
| plate-from-largest, slack 1.0 | 39 × 38 m | 13 | **50.2%** | **19 500 m³** |
| corridor spine, L grown to chase the bound | 103 × 64 m | 11 | 13.5% | 72 700 m³ |

Read it honestly:

- **Slack 1.0 is the right setting.** Growing the plate to chase the floor-count bound costs
  65% more volume, halves the fill, and *never gets there* — it plateaus at 11 floors.
- **It does not reach the floor lower bound**, 13 against 10. The claim "plate-from-largest
  hits the bound by construction" is only true when the largest room dwarfs everything, and
  that is precisely the case where fill collapses (see the outlier below). Two extra floors
  is the correct price.
- **50% room fill plus 19% corridor ≈ 69% of the plate doing something.** The remaining 31%
  is packing loss, and it is what the padding of §5 has to absorb.
- The corridor spine loses on every axis except floor count. It is dominated.

### The one pathological case

A single enormous unit — a generated parser, a table-driven state machine — drags the plate
with it. Simulated: adding one 500 KiB function to `elfcity`'s 98 blows the plate to
250 × 250 m and drops room fill to 7.8%, because one room needs a floor that 97 others
rattle around in.

Guard with a plate cap. If `wmax × dmax` exceeds, say, `4 × MAXPF × mean_room_area`, the
largest room is an outlier rather than a design driver: cap the plate at that, and let the
oversize rooms **spill across floors** — a room whose grid does not fit gets `th` split into
per-floor bands and occupies the same footprint on two or three consecutive floors, joined
by its own internal stair. That is the honest depiction anyway; a 500 KiB function *is* a
multi-storey building. Deferrable — implement the cap first and simply truncate the grid,
and add the spill later.

### Cost

Sorting is `O(n log n)`. The pack is `O(n · F · R)` where `F` is floors and `R` is rows per
floor (bounded by `plateD / minimum room depth`, so ~8). At the current ceiling of
`MAX_ROOMS_PER_BLD = 1400` (`src/city.c:28`), `F ≥ n/10 = 140`, giving ~1.6 M probes — a few
milliseconds, once, at load. And unlike the corridor-spine model there is **no outer
iteration**: the plate is computed in closed form from two maxima and a mean, so the packer
runs exactly once.

If the room ceiling is ever raised past ~10 000, bucket floors by largest remaining row gap
and search from the matching bucket upward, which restores near-`O(n)`. Not needed yet.

---

## 5. Corridors and padding

With a comb plan the corridors are not an afterthought — they are the packing's spare
space, given a job. Three kinds of leftover, in the order the packer creates them.

**Row corridors.** Every row gets a `CORR_W` strip along its open edge. That is the 19% in
the fill table and it is not waste. Rooms in a row face their strip; the strips are joined
at the plate edge by a spine running back to the stair core, so the plan is a comb, not a
set of disconnected galleries.

**Row tail** — the last room in a row does not reach `plateW`. Rather than one dead stub,
distribute the remainder into the `count + 1` gaps between rooms in that row. A metre of
slack spread over five gaps reads as pilasters and door reveals; five metres in one lump
at the end of the row reads as a bug.

**Row underrun** — a room shallower than its row's depth. Put the room against its
corridor, since the door has to be reachable, and fill from its back wall to the next
corridor with a solid `M_PART` block. Reads as a plant room. One extra box per shallow
room in `emit_floor()`.

**Plate underrun** — the last row does not reach `plateD`. This is the big one at 31% and
it is where the floor-to-floor variation lives: floor 3 might use 90% of the depth and
floor 11 only 40%. Options, cheapest first:

1. **Widen the last row's corridor** to absorb it. Free, and a floor with one generous
   gallery is a pleasant floor.
2. **Grow the rooms.** Once packing is done, scale up the tile pitch for that floor's
   rooms until the plate is used. Tempting, but it breaks the whole point — tile area must
   mean content size, and it would no longer be comparable between floors of the same
   building. **Do not do this.**
3. **A terrace.** Leave it open to the sky where the floor above is smaller. Only works
   with a stepped shell, which the single-box `emit_shell()` (`src/world.c:28`) does not
   do today.

Take option 1 and revisit if the buildings look hollow.

**Building box** then becomes simply

```c
b->w = CORE_W + b->plateW;
b->d = b->plateD;
b->h = b->nfloors * FLOOR_H + 1.6f;
```

and `bld_z0()` / `bld_z1()` (`src/model.h:194-195`) read `b->bz ∓ b->plateD/2` instead of
`CORR_W/2 + ROOM_D`. Two lines, and no change to their ~20 call sites — the win of having
had those helpers all along.
## 6. Squares on the floor

Once a room owns a `w × h` tile grid, the mosaic is the grid, and the three content
renderers collapse into one:

```c
/* replaces RT_FUNC (render.c:579), RT_OBJECT (:608) and RT_BYTES (:639) */
static void draw_tiles(const Building *b, const Room *r, float base){
    float t = b->tile;
    float gx = room_grid_x0(b, r), gz = room_grid_z0(b, r);   /* set back from the door */
    float dz = (r->side == 0) ? -1.0f : 1.0f;
    glBegin(GL_QUADS);
    for (int k = 0; k < r->ntiles; k++){
        int ci = k % r->tw, ri = k / r->tw;
        float x = gx + ci * t, z = gz + dz * ri * t;
        float cr, cg, cb; tile_color(r, k, &cr, &cg, &cb);
        glColor3f(cr, cg, cb);
        float h = tile_height(r, k);                          /* 0 for a flat mosaic */
        /* one quad at base + h, or an extruded column if h > 0 */
    }
    glEnd();
}
```

with `tile_color` / `tile_height` switching on `r->kind`: `byte_color()` of the
representative byte for `RT_BYTES`, the instruction-class colour `ICOL[]`
(`src/render.c:267`) for `RT_FUNC`, non-zero density for `RT_OBJECT`. The far-wall plane
`z = zFar + nz·0.03` disappears entirely from `RT_BYTES`.

Three things get fixed for free:

- The wall mosaic currently shows only `r->data[0 .. 1920]` and silently drops the rest
  (`src/render.c:648-653`); `RT_FUNC` subsamples with `off = k/want · avail`
  (`src/render.c:592`). With one tile per 16-byte line, the grid covers the *whole* room,
  because the grid is what sized the room.
- The room's size becomes legible from the doorway: a big function is a big floor of
  squares, not a taller bar on an identically sized wall.
- `layout_code()` (`src/render.c:295`) can drop its pitch-shrinking search loop and use
  `b->tile`, `r->tw`, `r->th` directly, so the sculptures stand on the mosaic squares —
  the instruction and its bytes in the same place.

Keep the extrusion. Flat squares on a floor read as carpet; the existing byte-value
height (`0.12 + v/255 · 1.10`, `src/render.c:598`) is what makes it a city block seen from
above. Just cap the height at about `0.9 · TILE` so you can still see across the room.

---

## 7. The change list

### `src/model.h`

The room stops being an interval on a corridor and becomes a rectangle on a plate.

```c
typedef struct Room {
    ...
    int   floor;
    int   row;           /* NEW: which comb row; replaces `side`          */
    float x0, x1;        /* now plate-local, not corridor-local           */
    float z0, z1;        /* NEW: the other two edges                      */
    int   doorSide;      /* NEW: which edge faces this row's corridor     */
    int   ntiles, tw, th;/* NEW: tile count and grid                      */
    ...
} Room;

typedef struct Building {
    ...
    float plateW, plateD;/* NEW: the floor plate, from §4                 */
    float tile;          /* NEW: metres per tile                          */
    ...
} Building;

static inline float bld_z0(const Building *b) { return b->bz - b->plateD * 0.5f; }
static inline float bld_z1(const Building *b) { return b->bz + b->plateD * 0.5f; }
```

`side` cannot simply be renamed: it is a two-valued flag consulted for wall orientation,
door normal and camera facing at seven sites. `row` plus `doorSide` carries the same
information for an arbitrary number of rows, so those sites become "which way does this
room's door face" rather than "north or south of the spine".

### `src/city.c`

- Add `room_tiles()` (§1) and `tile_rect()` (§2), called once per room after the
  `rooms_from_*()` calls populate the `RoomVec`.
- Add `plate_of()` (§4) — two maxima and a mean over the room list, closed form, no
  iteration.
- Replace `src/city.c:429-443` (the `per = 8` slicing) with the shelf packer. It must
  still produce `floorStart[]` sorted by floor, since `floor_realize()`
  (`src/city.c:368`) and `room_at()` (`src/world.c:270`) both iterate
  `floorStart[f] .. floorStart[f+1]` — so after packing, **stable-sort the room array by
  `(floor, row, x0)`** and rebuild `floorStart[]`. The array is owned outright, so this is
  a `qsort` plus a prefix scan.
- Replace `src/city.c:445-489` (natural width, corridor scaling, `x0`/`x1` assignment)
  with the packer's placements.
- The `linkPrev`/`linkNext` contiguity doors at `src/city.c:478-485` get *better* here,
  and need the test tightened. Two rooms may now be adjacent along either axis, so join
  them only if they are contiguous in the file **and** share a wall segment longer than
  `DOOR_W`. Combined with the address-adjacency tiebreak of §4, a run of consecutive
  static functions becomes a genuine enfilade.
- `src/city.c:491-493`: `b->w = CORE_W + b->plateW; b->d = b->plateD;`.
- `src/city.c:610`: `b->bz += b->plateD * 0.5f` instead of `CORR_W*0.5f + ROOM_D`.

### `src/world.c`

- `emit_floor()` (`:49-108`) is the largest single edit, maybe 70 lines. It stops walking
  one room list along one corridor and instead walks rows: per row, emit the corridor
  strip, then per room its four walls with a doorway on `doorSide`, then the underrun
  filler behind shallow rooms (§5).
- `room_at()` (`:270`): today it tests `side` and an x-range. It becomes a point-in-rect
  test over the floor's rooms. Same cost, and it must be a real rect test — with filler
  blocks between rooms, an x-only test reports you as inside a room you are standing
  beside.
- `MAX_SOLIDS 4096` (`:142`) is per three floors. Ten rooms × ~6 boxes, plus fillers and
  corridor strips, × 3 floors ≈ 300. No pressure.

### `src/render.c`

- `room_bounds()` (`:245`): return the room's own rect, four values straight off the
  `Room`, rather than deriving `zFar` from `ROOM_D`. It gets simpler.
- `draw_room()` (`:534`): the three content cases collapse into `draw_tiles()` (§6).
- `layout_code()` (`:295`): take the grid from the room instead of searching for a pitch.
- `bake_exterior()` (`:94`) puts window grids on four faces using `bld_x0/x1/z0/z1`. Those
  helpers still work, so windows need no change — but the plate is now up to 40 m across
  where it was 17, so the `pitch = 3.4f` window spacing will want a look.

### `src/main.c`

Camera and teleport helpers assume a room's z is `b->bz ± (CORR_W/2 + k)` — `:285`,
`:348`, `:442`. They become "stand in the room's corridor, facing its door", which the
`Room` rect and `doorSide` give directly. Cosmetic, but a tour that walks into a wall is
worse than no tour.

---

## 8. Stripped binaries: recovering the units

Everything above assumes a building's rooms mean something. For a stripped binary they do
not: with no symbols, `rooms_from_bytes()` (`src/city.c:159`) cuts `.text` into equal
slabs, and `/usr/bin/git` becomes 770 identical 4 KiB rooms. Sizing rooms by content is
pointless when the content has been cut on arbitrary boundaries.

The good news is that **function boundaries almost always survive stripping**, in a section
that has nothing to do with debugging.

### `.eh_frame` is a function table

C++ exceptions and `backtrace()` need to unwind the stack at runtime, so the unwind tables
are *loaded*, not debug data — `strip` cannot remove them, and on x86-64
`-fasynchronous-unwind-tables` is the default even for C. Every FDE (Frame Description
Entry) in `.eh_frame` carries an exact `(pc_begin, pc_range)` pair: a function's start and
length. `.eh_frame_hdr` additionally holds a sorted binary-search table of entry points,
which is the cheapest thing to parse in the whole file.

Measured across **`/usr/bin`** — 1120 ELF64 executables with a `.text` over 4 KiB, of which
**1118 are stripped**:

| | count | share |
|---|---|---|
| usable `.eh_frame_hdr` index | 1109 | **99%** |
| stripped **and** indexed | 1107 | 99.0% of stripped |
| no usable index | 11 | 1% |

That is **898 813 function boundaries recovered for free** across the directory, median 40
functions per binary, largest 50 401.

And the boundaries are not approximate. Cross-checked against `elfcity`'s own symbol table:

```
real FUNC symbols in .text : 93
FDE starts in .text        : 93
exact matches              : 93   (100.0%)
FDE start with no symbol   : 0
symbol with no FDE         : 0
```

**Exact, both directions.** Walking the FDE extents covers 98–99% of `.text` in every
binary tested; the shortfall is entirely explained:

- every gap of ≤ 15 bytes is 16-byte inter-function alignment padding;
- there is exactly **one** gap over 64 bytes in every gcc-linked binary, always 202 bytes,
  always the same four CRT stubs from `crtbegin.o` — `deregister_tm_clones`,
  `register_tm_clones`, `__do_global_dtors_aux`, `frame_dummy`. Hard-code it or leave it as
  one room.

So `.eh_frame` covers **100% of actual compiled functions**. This is a ~120-line parser
(CIE augmentation string for the pointer encoding, then a linear walk) with no
disassembly, and it should be tried before anything else.

Note that `.eh_frame_hdr` is only an *index*. Two of the eleven exceptions above —
`busybox` and `docker-init`, both statically linked — have no `.eh_frame_hdr` section but
do have `.eh_frame`; a linear walk recovers 1117 FDEs covering 98.4% of `docker-init`.
**Always fall back to walking `.eh_frame` directly.**

### The other sources, in order of value

1. **`.eh_frame`** — above. Exact boundaries, 99% of binaries.
2. **`.gopclntab`** — Go binaries have no `.eh_frame` but carry their own table, which
   gives boundaries **and names**. Nine of the eleven exceptions are Go (`docker`, `snap`,
   `ctr`, `containerd-*`). `docker` carries a 9 MB `.data.rel.ro.gopclntab`. Different
   parser, well documented, and it turns the worst-looking binaries into the best-labelled
   ones.
3. **`.dynsym` and `.rela.plt`** — exported and imported functions keep their names through
   stripping. This is the only source that yields *names*, so it is worth reading even when
   `.eh_frame` already gave you the boundaries: `elfload.c` parses both already.
4. **`.init_array` / `.fini_array` / `e_entry`** — a handful of guaranteed entry points.

### Where the jump graph comes in

After all of the above, one real case remains. `busybox` is statically linked against a
glibc full of hand-written assembly (`memcpy`, `strlen`, …) that carries no CFI, so its
FDEs cover only **61.8%** of a 1.73 MB `.text` — leaving one contiguous 646 KB span with no
structure at all.

This is where building the graph of internal constant jumps earns its place. But the
measurements say something slightly different from the obvious plan, and it is worth being
precise about it.

**Finding entries does not need the graph.** Two byte-level scans do most of the work, with
no disassembler at all:

- **direct calls** — opcode `0xe8` plus a `rel32`; every target that lands in `.text` is a
  function entry;
- **`endbr64`** — the four bytes `f3 0f 1e fa`. On any CET build (all current distro
  binaries) this marks every indirect-branch target, which includes every address-taken
  function.

Scored against `elfcity`'s 93 known functions:

| seed source | found | recall | precision |
|---|---|---|---|
| `.eh_frame` FDEs | 93 | **100%** | **100%** |
| `call rel32` targets | 90 | 90.3% | 93.3% |
| `call` targets, 2+ callers | 64 | 68.8% | 100% |
| `endbr64` scan | 64 | 66.7% | 96.9% |
| 16-byte aligned after padding | 96 | 24.7% | 24.0% |
| union of the cheap seeds | 174 | 100% | 53.4% |

Two readings worth keeping. The **alignment heuristic is junk** — 24% precision, it finds
padding, drop it. And the **union over-segments badly**: 100% recall but half the "entries"
are basic blocks inside functions.

**The graph's job is rejection, not discovery.** A real function entry is *called*; a basic
block is *jumped to*. So scan the conditional and short jumps (`0f 80`–`0f 8f`, `70`–`7f`,
`eb`) to build the intra-`.text` jump-target set, and use it as **negative** evidence —
discard any `endbr64` candidate that is the target of a jump, since that is a loop header
or a switch case, not a function:

| | found | recall | precision |
|---|---|---|---|
| union of cheap seeds | 174 | 100% | 53.4% |
| generic jump-graph rejection | 151 | 100% | 61.6% |
| **call-targets ∪ (endbr64 not jumped-to)** | **101** | **100%** | **92.1%** |

That last line is the recipe: **100% recall at 92% precision, from three byte scans and a
set difference.** No disassembler, no basic-block reconstruction, no connected components.

**And you do not need connected components at all.** The instinctive plan — build the CFG,
take weakly connected components, call each one a function — has to fight tail calls
(indistinguishable from internal jumps), jump tables (which silently truncate a function),
and hot/cold splitting (one function, two components). Skip all of it: once you have the
entry set, sort it and define unit `i` as `[entry_i, entry_{i+1})`. That tiles the span
completely, needs no edge classification, and is exactly the structure `.eh_frame` hands
you anyway — so both paths produce the same shape of result and feed the same room builder.

Applied to `busybox`'s uncovered 646 KB, the cheap seeds find **2408 candidate entries**
with a plausible function-size distribution — median 119 B, p90 758 B, max 13.5 KB. Against
158 identical 4 KiB slabs, that is the difference between a district and a filing cabinet.

### What this changes upstream

This mostly *removes* work rather than adding it, and it retires a recommendation made
earlier in this note.

- **`rooms_from_syms()` gets a second supplier.** Today rooms come from
  `s->symCount > 0` or nothing (`src/city.c:396`). Add a `unit[]` list per code section,
  populated by `.eh_frame` → `.gopclntab` → seed-scan in that order, and have
  `rooms_from_syms()` consume symbols *or* units. Names from `.dynsym` attach where they
  exist; the rest are `sub_401f60`, which is what every disassembler shows anyway.
- **"Raise the chunk size for unsymbolised spans" (option 3 under
  [What actually breaks](#what-actually-breaks)) is superseded.** It was damage control for
  not knowing where the functions were. `git` stops being 770 arbitrary slabs and becomes
  4472 real functions, median 272 bytes.
- **This makes the room-sizing work worth doing.** Content-proportional rooms only mean
  something if the content is a unit. On a stripped binary today they would be 770 rooms of
  identical size — a perfectly packed building conveying nothing.
- **The floor count goes up a lot, and that is fine.** 4472 functions at 10 per floor is 448
  floors and a 1792 m tower. Floors are lazy (see [What actually breaks](#what-actually-breaks)),
  so the cost is a far plane and a room-count cap, not framerate. What it does need is
  `MAX_ROOMS_PER_BLD` raised from 1400 — otherwise 69% of the functions you just recovered
  are dropped on the floor.

### Cost

`.eh_frame` parsing is microseconds and pure integer work. The seed scan is three linear
passes over `.text`; on `busybox`'s 1.7 MB that is a few milliseconds. Both run once at
load, both are optional — if either fails, fall through to today's `rooms_from_bytes()`.

The byte-level scans do misframe occasionally: a `0xe8` inside data-in-`.text` or in the
middle of a longer instruction yields a bogus target. That is already priced into the 92%
precision above. Since the project links capstone, the same scan over a real decoded
instruction stream would be cleaner — but it costs a full linear sweep of `.text` at load,
and 92% from three `memchr`-speed passes is the better trade.

---

## What actually breaks

### Not the floor count

An earlier draft of this note treated `MAX_FLOORS = 34` (`src/city.c:29`) as the blocking
constraint. It is not. **Floors are nearly free, because almost nothing about a floor is
realized until you stand on it.** Checked against the code:

| per-floor work | when it happens | scales with `nfloors`? |
|---|---|---|
| listing text (`floor_realize`, `src/city.c:368`) | one floor at a time, `b->realized` | no |
| disassembly (`city_enter_room`, `src/city.c:343`) | one room at a time, globally | no |
| interior geometry (`draw_interior`, `src/render.c:973`) | player floor ±1 | no |
| collision solids (`gather`, `src/world.c:186`) | player floor ±1 | no |
| spiral stair (`draw_stair`, `src/render.c:731`) | player floor ±1; floors 0–2 at ground | no |
| `ground_at` / `stair_height` loops | every frame | yes, but 448 iterations of integer work |
| exterior windows (`bake_exterior`, `src/render.c:113`) | once, into a display list | **yes** |
| `Room` structs | all allocated up front | **yes** |

Both of the real ones are trivial. `sizeof(Room)` is 504 bytes, so `git`'s 4472 recovered
functions cost **2.1 MB** — noise. And a 448-floor tower on a 40 m plate bakes about 20 000
window quads into its display list, ~80 k vertices, submitted once per frame when visible.
Neither is a reason to cap anything.

So: **raise `MAX_FLOORS` to whatever, or delete it.** A stripped 3 MB `.text` *should* be a
1792 m tower. That is an honest depiction of a binary that is mostly one enormous code
section, and it is the most informative thing the skyline can do.

### What does break, in order

The two caps are simply deleted — `MAX_FLOORS` (`src/city.c:29`) and
`MAX_ROOMS_PER_BLD` (`src/city.c:28`). `git`'s `.text` becomes 2638 rooms and 298 floors
(4472 functions, grouped per §4), a 1192 m tower, and that is the correct picture of a
binary that is mostly one enormous code section. Climbing it is fine.

That leaves one thing that must actually be fixed, and it is not what it looks like.

**The far plane must be computed, and the ground plane is what drives it.**
`gluPerspective(70.0, aspect, 0.14, 1600.0)` (`src/render.c:877`) is a constant, and the
obvious reading is that tall towers overrun it. They do — but the dominant term is the
**ground quad**, drawn 400 m beyond the city bounding box on every side
(`float m = 400.0f`, `src/render.c:158`). That skirt alone puts the far corner of the world
1281 m from the near corner *for `elfcity`*, a binary with 13-floor buildings. The current
1600 m has been marginal all along; towers only make it obvious.

```c
/* city_build(), once the bbox at src/city.c:621-622 is known */
float gw = (c->maxx - c->minx) + 2*GROUND_MARGIN;      /* GROUND_MARGIN = 400 */
float gd = (c->maxz - c->minz) + 2*GROUND_MARGIN;
float gh = 0;
for (int i = 0; i < c->nbld; i++)
    if (c->bld[i].h > gh) gh = c->bld[i].h;
c->farPlane = sqrtf(gw*gw + gd*gd + gh*gh) * 1.05f + 50.0f;
```

and `render_scene()` (`src/render.c:867`) passes `c->farPlane` to `gluPerspective` instead of the literal. It
already has the `App`, so the city is in reach — this is a one-line change at the call site:

| | ground W | ground D | height | diagonal | far plane |
|---|---|---|---|---|---|
| `elfcity` | 920 m | 890 m | 52 m | 1281 m | 1395 m |
| `git`, grouped | 1178 m | 1029 m | 1192 m | 1967 m | 2115 m |
| `git`, ungrouped | 1178 m | 1029 m | 1904 m | 2464 m | 2637 m |
| a 50 MB binary | 1700 m | 1400 m | 6000 m | 6391 m | 6761 m |

**Raise the near plane too.** A 0.14 m near plane against a 2 km far plane is a 15 000:1
ratio, and a 24-bit depth buffer cannot hold it. Depth resolution goes as `z²/(near·2²⁴)`:

| near | at 50 m | at 200 m | at 500 m | at 2000 m |
|---|---|---|---|---|
| 0.14 m (today) | 1.1 mm | 17 mm | 106 mm | 1703 mm |
| **0.25 m** | 0.6 mm | 9.5 mm | 60 mm | 954 mm |

`PLAYER_R` is 0.34 m (`src/world.h`) and `hits()` (`src/world.c:203`) keeps the player's
centre that far off every solid, so **the near plane can be raised to just under 0.34 with
no risk of clipping into a wall** — 0.25 m nearly halves the depth error for free. That is
enough: at 500 m a 6 cm error is invisible on a 40 m-wide building.

If z-fighting still shows up on the far skyline, the ordered fixes are: stop letting the
ground skirt set the scale (translate the base quad to follow the camera each frame, keeping
the district plots and plaza in the fixed display list — the skirt is one untextured quad,
so moving it is free), and only then a two-pass depth-range split.

**Truncation should be surfaced, even with the caps gone.** `build_one()` sets
`b->truncated` (`src/city.c:426`) and nothing ever reads it — `hud.c:169` reports only
*room-level* truncation from the disassembler. With no room cap this should never fire, so
show it in the building panel: if it ever does, it is a bug rather than a policy.

### The rest

**Buildings get squatter, and the city grows.** Today a building is `CORE_W + len` by
`CORR_W + 2·ROOM_D` = up to 98 × 17 m — a long thin slab. A plate is near-square and, for
`elfcity`'s `.text`, 39 × 38 m. Deeper, but *much* shorter, and the volume goes down, not
up. The district packer `pack()` (`src/city.c:510`) already shelf-packs on `b->w`/`b->d`,
so the city layout absorbs this with no change — but it was tuned against slabs, and
near-square blocks will shelf differently. Look at `gapx = 10, gapz = 16` afterwards.

The one thing that genuinely gets worse is the **walk across a floor**. A 39 × 38 m plate
with a comb of corridors is a real building to cross, where today's 17 m-deep slab puts
every door within sight of the spine. The stair core is at one corner (`stair_center()`,
`src/world.c:12`), so the far corner of a plate is ~50 m of corridor away. If that grates,
the fix is not a smaller plate — it is a second stair.

**Rooms with zero content.** `elfcity`'s own symbol table contains functions with
`size = 0`. `ntiles` clamps to 1, `tile_rect(1)` gives 2 × 2, and the minimum-room clamp
gives a 2.6 × 2.4 m closet. Correct behaviour — just make sure the clamp is there,
because a `0 × 0` room would divide by zero in the row-frontage sum.

---

## Suggested order

1. Delete both caps (`MAX_FLOORS`, `MAX_ROOMS_PER_BLD`, `src/city.c:28-29`), compute the
   far plane from the city bbox, and raise the near plane to 0.25. Do these first — they
   cost nothing and they stop later steps being misdiagnosed as layout bugs when they are
   really truncation and clipping.
2. `tile_rect()` + `room_tiles()`, storing `ntiles/tw/th` on the `Room` but leaving the
   layout alone. Verifiable immediately: print the grid in the room HUD panel
   (`src/hud.c:252`) and walk around.
3. Move the mosaic to the floor using the new grid, still inside the old fixed-depth
   rooms. This is change 4 of the brief, standalone, and it is the one you can see.
4. Give `Room` its `z0/z1` and `doorSide`, wire them through `room_bounds()`,
   `emit_floor()`, `room_at()` and `main.c`, and then **set them to exactly what `side`
   and `ROOM_D` produce today**. No visible change — a pure refactor, and the point at
   which you find out whether anything else was reading `ROOM_D` behind your back. This is
   the step that de-risks the whole thing, so do not skip it because it renders identically.
5. `plate_of()` and the shelf packer. By now it only has to fill in
   `x0/x1/z0/z1/floor/row/doorSide`, and everything downstream already reads them.
6. Only then the extras: the address-adjacency tiebreak, the enfilade doors, the plate cap
   and the oversize spill. Each is independent and each can be judged by eye.

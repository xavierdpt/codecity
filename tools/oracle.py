# oracle.py -- the gdb half of the differential oracle of §16.
#
#   gdb -q -batch -x tools/oracle.py --args <oracle-binary>
#
# For each of a few dozen chosen register states, every instruction in
# tools/oracle-forms.s is written into the inferior's RWX scratch page, run
# for exactly one step from that state, and the resulting register, flag and
# memory deltas printed as one corpus line.  The inferior never runs free and
# nothing from any file under study is executed.
#
# The corpus it prints is checked in, so the everyday test needs neither gdb
# nor an x86 host.

import os, re, subprocess, sys, tempfile

FORMS  = os.environ.get("ORACLE_FORMS", "tools/oracle-forms.s")
BLOCKS = int(os.environ.get("ORACLE_BLOCKS", "24"))
WINDOW = 512                      # bytes of `data` the corpus records
MEMBASE_OFF = 128                 # r15 points here
STACK_OFF   = 448                 # rsp points here

GPR = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
       "r8","r9","r10","r11","r12","r13","r14","r15"]
NXMM = 8                          # xmm0..7 -- recorded 256 bits wide,
                                  # because a VEX form zeroes the upper half
                                  # and only a ymm-wide corpus can see it
FLMASK = 0x8d5                    # CF PF AF ZF SF OF
DF     = 0x400                    # the mis-step tripwire; see oracle.c
M64 = 0xFFFFFFFFFFFFFFFF

# ---------------------------------------------------------------- forms

def strip_block_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)

def read_forms(path):
    """(pre-dict) per instruction, in source order."""
    src = strip_block_comments(open(path).read())
    out = []
    for line in src.splitlines():
        body, _, comment = line.partition("#")
        body = body.strip()
        if not body or body.startswith(".") or body.endswith(":"):
            continue
        pre = {}
        m = re.search(r"\bpre\b(.*)", comment)
        if m:
            for k, v in re.findall(r"(\w+)=([@\w+x]+)", m.group(1)):
                pre[k] = v
        out.append(pre)
    return out

def assemble(path):
    """(bytes, text) per instruction, in the same order."""
    d = tempfile.mkdtemp()
    obj = os.path.join(d, "forms.o")
    subprocess.check_call(["as", "-o", obj, path])
    dump = subprocess.check_output(["objdump", "-d", "-M", "intel", obj]).decode()
    out, cur = [], None
    for line in dump.splitlines():
        m = re.match(r"^\s+[0-9a-f]+:\t((?:[0-9a-f]{2} )+)\s*(.*)$", line)
        if not m:
            continue
        raw, text = m.group(1).replace(" ", ""), m.group(2).strip()
        if text:                                  # a new instruction
            out.append([raw, re.sub(r"\s+", " ", text)])
        elif out:                                 # continuation of its bytes
            out[-1][0] += raw
    os.unlink(obj); os.rmdir(d)
    return [(b, t) for b, t in out]

# ---------------------------------------------------------------- state

class Rng:
    """splitmix64, so a corpus can be regenerated exactly."""
    def __init__(self, seed): self.s = seed & M64
    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & M64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return (z ^ (z >> 31)) & M64

EDGE = [0, 1, 2, 0xffffffffffffffff, 0x7fffffffffffffff, 0x8000000000000000,
        0xffffffff, 0x7fffffff, 0x80000000, 0x100000000, 0xff, 0x80, 0x7f,
        0xdeadbeefcafe1234, 0x11223344, 8, 63, 64, 65]

def pick(rng):
    x = rng.next()
    k = x % 10
    if k < 4:  return EDGE[(x >> 8) % len(EDGE)]
    if k < 6:  return (x >> 8) & 0xffff
    if k < 8:  return (x >> 8) & 0xffffffff
    return x

# ---------------------------------------------------------------- gdb

def reg(name):
    return int(gdb.parse_and_eval("$" + name)) & M64

def setreg(name, val):
    gdb.execute("set $%s = 0x%x" % (name, val & M64), to_string=True)

def xmm_get(i):
    v = gdb.parse_and_eval("$ymm%d.v4_int64" % i)
    return sum((int(v[k]) & M64) << (64 * k) for k in range(4))

def xmm_set(i, v):
    for k in range(4):
        gdb.execute("set $ymm%d.v4_int64[%d] = 0x%x" % (i, k, (v >> (64 * k)) & M64),
                    to_string=True)

def read_mem(addr, n):
    return bytes(gdb.selected_inferior().read_memory(addr, n))

def write_mem(addr, b):
    gdb.selected_inferior().write_memory(addr, b)

def main():
    # gdb chatters on stdout (breakpoint notices, the inferior's own output),
    # so the corpus goes to a file of its own rather than being piped.
    out = open(os.environ.get("ORACLE_OUT", "vm-corpus.txt"), "w")
    def emit(s): out.write(s + "\n")

    gdb.execute("set confirm off")
    gdb.execute("set pagination off")
    gdb.execute("set debuginfod enabled off")
    gdb.execute("set disable-randomization on")
    gdb.execute("break probe")
    gdb.execute("run")

    scratch = int(gdb.parse_and_eval("scratch")) & M64
    data    = int(gdb.parse_and_eval("data")) & M64
    membase = data + MEMBASE_OFF
    stack   = data + STACK_OFF

    pres  = read_forms(FORMS)
    forms = assemble(FORMS)
    if len(pres) != len(forms):
        sys.stderr.write("forms parse mismatch: %d annotations, %d instructions\n"
                         % (len(pres), len(forms)))
        return 1
    sys.stderr.write("%d forms, %d state blocks\n" % (len(forms), BLOCKS))

    emit("# codecity vm oracle corpus -- generated by tools/gen-corpus.sh, do not edit")
    emit("# every line was produced by single-stepping the instruction on this")
    emit("# machine's CPU from the state above it.  Flags Capstone reports as")
    emit("# architecturally undefined for an instruction are not compared.")
    emit("#   s <16 GPRs in rax rcx rdx rbx rsp rbp rsi rdi r8..r15 order> <eflags>")
    emit("#   v <%d ymm registers, 64 hex digits each, high half first>" % NXMM)
    emit("#   w <address of the memory window> <%d bytes, hex>" % WINDOW)
    emit("#   t <instruction bytes> <deltas...> ; <disassembly>")
    emit("# a delta is reg=value, f=eflags, m<offset>=<bytes>, or rip=<value>")
    emit("v 1")
    # rip-relative operands resolve against the address the instruction ran at
    emit("c %x" % scratch)

    rng = Rng(0x0DDBA11)
    ntest = 0
    nredo = 0
    for blk in range(BLOCKS):
        want = {}
        for r in GPR:
            want[r] = pick(rng)
        want["rsp"] = stack
        want["r15"] = membase
        wflags = rng.next() & FLMASK

        win = bytes((rng.next() >> 13) & 0xff for _ in range(WINDOW))
        # packed lanes want values that are interesting per element, not one
        # 128-bit blob of noise: mix edges into every byte
        wantx = []
        for i in range(NXMM):
            x = 0
            for k in range(32):
                r = rng.next()
                b = (0, 1, 0x7f, 0x80, 0xff)[(r >> 3) % 5] if (r % 3) == 0 else ((r >> 8) & 0xff)
                x |= b << (k * 8)
            wantx.append(x)

        # apply, then read back what the hardware actually holds
        for r in GPR: setreg(r, want[r])
        for i in range(NXMM): xmm_set(i, wantx[i])
        gdb.execute("set $eflags = 0x%x" % ((reg("eflags") & ~FLMASK) | wflags), to_string=True)
        write_mem(data, win)
        base = {r: reg(r) for r in GPR}
        basex = [xmm_get(i) for i in range(NXMM)]
        bflags = reg("eflags")

        emit("s " + " ".join("%016x" % base[r] for r in GPR) + " %08x" % bflags)
        emit("v " + " ".join("%064x" % x for x in basex))
        emit("w %x %s" % (data, win.hex()))

        for (raw, text), pre in zip(forms, pres):
            b = bytes.fromhex(raw)
            fault, attempt = None, 0
            for attempt in range(5):
                # Everything is restored on every attempt: a mis-step can
                # have written to the stack before the tripwire caught it.
                write_mem(scratch, b + b"\xfc" * 32)     # cld padding, see oracle.c
                write_mem(data, win)
                for r in GPR: setreg(r, base[r])
                for i in range(NXMM): xmm_set(i, basex[i])
                setreg("eflags", bflags | DF)
                for k, v in pre.items():
                    if v.startswith("@d"):
                        off = int(v[2:], 0) if len(v) > 2 else 0
                        setreg(k, data + off)
                    else:
                        setreg(k, int(v, 0))
                pbase = {r: reg(r) for r in GPR}
                pbasex = [xmm_get(i) for i in range(NXMM)]
                pflags = reg("eflags")
                setreg("rip", scratch)
                try:
                    res = gdb.execute("stepi", to_string=True)
                except gdb.error as e:
                    fault = str(e).replace(";", ",")
                    break
                if "SIG" in res:
                    fault = "fault"
                    gdb.execute("signal 0", to_string=True)
                    break
                if reg("eflags") & DF:
                    fault = None
                    break        # the tripwire survived, so our instruction ran
                fault = "gdb did not step the instruction"
            nredo += attempt
            if fault:
                emit("x %s ; %s ; %s" % (raw, fault, text))
                continue

            d = []
            for r in GPR:
                v = reg(r)
                if v != pbase[r]:
                    d.append("%s=%x" % (r, v))
            for i in range(NXMM):
                v = xmm_get(i)
                if v != pbasex[i]:
                    d.append("xmm%d=%064x" % (i, v))
            f = reg("eflags")
            if (f & FLMASK) != (pflags & FLMASK):
                d.append("f=%x" % (f & FLMASK))
            rip = reg("rip")
            if rip != scratch + len(b):
                d.append("rip=+%d" % (rip - scratch))
            now = read_mem(data, WINDOW)
            if now != win:
                i = 0
                while i < WINDOW:
                    if now[i] != win[i]:
                        j = i
                        while j < WINDOW and now[j] != win[j]: j += 1
                        d.append("m%d=%s" % (i, now[i:j].hex()))
                        i = j
                    else:
                        i += 1
            # the state the test actually ran from, when `pre` changed it
            p = ["%s=%x" % (k, pbase[k]) for k in sorted(pre)]
            emit("t %s%s %s; %s" % (raw,
                                     (" p:" + ",".join(p)) if p else "",
                                     " ".join(d) + (" " if d else ""),
                                     text))
            ntest += 1

    out.close()
    sys.stderr.write("%d vectors, %d steps redone\n" % (ntest, nredo))
    gdb.execute("kill")
    return 0

try:
    import gdb
except ImportError:
    sys.stderr.write("run me under gdb: gdb -q -batch -x tools/oracle.py --args ORACLE\n")
    sys.exit(2)

main()

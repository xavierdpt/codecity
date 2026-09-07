/* oracle-forms.s -- one tier-1 instruction per line, assembled and then
   single-stepped on hardware by tools/oracle.py.  Order does not matter and
   nothing here is ever executed as a program: each line is lifted out on its
   own, run from a chosen register state, and the result recorded.

   r15 is the memory base (data+128) and rsp is data+448 in every vector, so
   any [r15+n] or stack reference lands inside the 512-byte window the corpus
   records.  A "# pre" comment constrains the state for that line only --
   used where an unconstrained vector would fault.                       */
.intel_syntax noprefix
.text

/* ---- mov, and the sub-register trap -------------------------------- */
    mov     rax, rcx
    mov     eax, ecx
    mov     ax, cx
    mov     al, cl
    mov     ah, ch
    mov     r8, r9
    mov     r8d, r9d
    mov     r8b, r9b
    movabs  rax, 0x1122334455667788
    mov     rax, 0x7fffffff
    mov     rax, -1
    mov     eax, 0xdeadbeef
    mov     cx, 0x1234
    mov     cl, 0x5a

/* ---- zero and sign extension --------------------------------------- */
    movzx   rax, cl
    movzx   rax, cx
    movzx   eax, cl
    movzx   eax, cx
    movsx   rax, cl
    movsx   rax, cx
    movsx   eax, cl
    movsx   eax, cx
    movsxd  rax, ecx
    cdq
    cqo
    cdqe
    cwde

/* ---- lea: no flags, and 32-bit address size truncates -------------- */
    lea     rax, [rbx+8]
    lea     rax, [rbx+rcx*4+16]
    lea     rax, [rcx*8]
    lea     eax, [ebx+ecx*2+4]
    lea     rax, [rip+0x20]

/* ---- add / sub / logic, every width -------------------------------- */
    add     rax, rcx
    add     eax, ecx
    add     ax, cx
    add     al, cl
    add     rax, 0x7f
    add     rax, 0x12345678
    add     eax, 0x12345678
    add     al, 0x7f
    sub     rax, rcx
    sub     eax, ecx
    sub     al, cl
    sub     rax, 0x7f
    and     rax, rcx
    and     eax, ecx
    and     al, cl
    and     rax, 0x0f
    or      rax, rcx
    or      eax, ecx
    or      rax, 0x0f
    xor     rax, rcx
    xor     eax, ecx
    xor     rax, rax
    xor     eax, eax
    cmp     rax, rcx
    cmp     eax, ecx
    cmp     al, cl
    cmp     rax, 0x7f
    test    rax, rcx
    test    eax, ecx
    test    al, cl
    test    rax, 0x0f
    adc     rax, rcx
    adc     eax, ecx
    sbb     rax, rcx
    sbb     eax, ecx

/* ---- inc / dec / neg / not: inc must leave CF alone ----------------- */
    inc     rax
    inc     eax
    inc     cl
    dec     rax
    dec     eax
    dec     cl
    neg     rax
    neg     eax
    neg     cl
    not     rax
    not     eax

/* ---- shifts: the count mask is width-dependent, and 0 is a no-op ---- */
    shl     rax, 0
    shl     rax, 1
    shl     rax, 5
    shl     rax, 65
    shl     eax, 33
    shl     eax, 1
    shl     cl, 3
    shr     rax, 1
    shr     rax, 5
    shr     eax, 3
    shr     cl, 2
    sar     rax, 1
    sar     rax, 5
    sar     eax, 3
    sar     cl, 2
    shl     rax, cl
    shr     rax, cl
    sar     rax, cl
    shl     eax, cl
    shr     eax, cl

/* ---- multiply and divide ------------------------------------------- */
    imul    rax, rcx
    imul    eax, ecx
    imul    rax, rcx, 7
    imul    rax, rcx, 0x1234
    imul    eax, ecx, 7
    imul    rcx
    imul    ecx
    mul     rcx
    mul     ecx
    div     rcx                     # pre rdx=0 rcx=0x1f
    div     ecx                     # pre rdx=0 rcx=0x1f
    idiv    rcx                     # pre rdx=0 rax=0x123456789 rcx=0x1f
    idiv    ecx                     # pre rdx=0 rax=0x12345678 rcx=0x1f

/* ---- bit scan: the destination survives a zero source -------------- */
    bsf     rax, rcx
    bsr     rax, rcx
    bsf     rax, rcx                # pre rcx=0
    bsr     rax, rcx                # pre rcx=0
    bsf     eax, ecx

/* ---- exchange ------------------------------------------------------- */
    xchg    rax, rcx
    xchg    eax, ecx
    xchg    al, cl

/* ---- the stack ------------------------------------------------------ */
    push    rcx
    push    0x41
    pop     rcx
    pop     rax
    leave                           # pre rbp=@d+64

/* ---- memory: the effective address, and every access width --------- */
    mov     rax, [r15+8]
    mov     eax, [r15+4]
    mov     ax, [r15+2]
    mov     al, [r15+1]
    mov     rax, [r15+rcx*8]        # pre rcx=3
    mov     [r15+16], rcx
    mov     [r15+24], ecx
    mov     [r15+28], cx
    mov     [r15+30], cl
    movzx   eax, byte ptr [r15+3]
    movzx   eax, word ptr [r15+2]
    movsx   rax, byte ptr [r15+3]
    movsx   rax, word ptr [r15+2]
    movsxd  rax, dword ptr [r15+4]
    add     rax, [r15+8]
    add     [r15+32], rcx
    sub     rax, [r15+8]
    and     [r15+40], rcx
    cmp     rax, [r15+8]
    cmp     [r15+8], rax
    test    [r15+8], rcx
    inc     qword ptr [r15+48]
    dec     dword ptr [r15+52]
    neg     qword ptr [r15+56]
    shl     qword ptr [r15+64], 3
    xchg    [r15+72], rcx
    lea     rax, [r15+rcx*2+9]

/* ---- setcc: the sixteen conditions, through a register ------------- */
    seto    al
    setno   al
    setb    al
    setae   al
    sete    al
    setne   al
    setbe   al
    seta    al
    sets    al
    setns   al
    setp    al
    setnp   al
    setl    al
    setge   al
    setle   al
    setg    al

/* ---- cmovcc: the same sixteen, moving a whole register ------------- */
    cmovo   rax, rcx
    cmovno  rax, rcx
    cmovb   rax, rcx
    cmovae  rax, rcx
    cmove   rax, rcx
    cmovne  rax, rcx
    cmovbe  rax, rcx
    cmova   rax, rcx
    cmovs   rax, rcx
    cmovns  rax, rcx
    cmovp   rax, rcx
    cmovnp  rax, rcx
    cmovl   rax, rcx
    cmovge  rax, rcx
    cmovle  rax, rcx
    cmovg   rax, rcx
    cmove   eax, ecx

/* ---- jcc: the same sixteen again, read off rip --------------------- */
    jo      .+16
    jno     .+16
    jb      .+16
    jae     .+16
    je      .+16
    jne     .+16
    jbe     .+16
    ja      .+16
    js      .+16
    jns     .+16
    jp      .+16
    jnp     .+16
    jl      .+16
    jge     .+16
    jle     .+16
    jg      .+16

/* ---- the ones that must do nothing --------------------------------- */
    nop
    endbr64
    xchg    ax, ax

/* ---- tier 3c: packed integer arithmetic ---------------------------- */
    paddb   xmm0, xmm1
    paddw   xmm0, xmm1
    paddd   xmm0, xmm1
    paddq   xmm0, xmm1
    psubb   xmm0, xmm1
    psubw   xmm0, xmm1
    psubd   xmm0, xmm1
    psubq   xmm0, xmm1
    paddsb  xmm0, xmm1
    paddsw  xmm0, xmm1
    psubsb  xmm0, xmm1
    psubsw  xmm0, xmm1
    paddusb xmm0, xmm1
    paddusw xmm0, xmm1
    psubusb xmm0, xmm1
    psubusw xmm0, xmm1
    pavgb   xmm0, xmm1
    pavgw   xmm0, xmm1
    pmullw  xmm0, xmm1
    pmulhw  xmm0, xmm1
    pmulhuw xmm0, xmm1
    pmaddwd xmm0, xmm1
    psadbw  xmm0, xmm1
    pminsw  xmm0, xmm1
    pmaxsw  xmm0, xmm1
    pminub  xmm0, xmm1
    pmaxub  xmm0, xmm1
    pabsb   xmm0, xmm1
    pabsw   xmm0, xmm1
    pabsd   xmm0, xmm1

/* ---- tier 3b: shuffle, compare, logic, shifts ---------------------- */
    pcmpeqb xmm0, xmm1
    pcmpeqw xmm0, xmm1
    pcmpeqd xmm0, xmm1
    pcmpgtb xmm0, xmm1
    pcmpgtw xmm0, xmm1
    pcmpgtd xmm0, xmm1
    pand    xmm0, xmm1
    pandn   xmm0, xmm1
    por     xmm0, xmm1
    pxor    xmm0, xmm1
    pxor    xmm2, xmm2
    psllw   xmm0, 3
    pslld   xmm0, 5
    psllq   xmm0, 9
    psrlw   xmm0, 3
    psrld   xmm0, 5
    psrlq   xmm0, 9
    psraw   xmm0, 3
    psrad   xmm0, 5
    psllw   xmm0, xmm3
    psrld   xmm0, xmm3
    pslldq  xmm0, 3
    psrldq  xmm0, 5
    punpcklbw xmm0, xmm1
    punpcklwd xmm0, xmm1
    punpckldq xmm0, xmm1
    punpcklqdq xmm0, xmm1
    punpckhbw xmm0, xmm1
    punpckhwd xmm0, xmm1
    punpckhdq xmm0, xmm1
    punpckhqdq xmm0, xmm1
    pshufd  xmm0, xmm1, 0x1b
    pshufd  xmm0, xmm1, 0xe4
    pshufb  xmm0, xmm1
    packsswb xmm0, xmm1
    packuswb xmm0, xmm1
    packssdw xmm0, xmm1
    pmovmskb eax, xmm1
    movmskps eax, xmm1
    movmskpd eax, xmm1

/* ---- tier 3a: the bulk moves --------------------------------------- */
    movdqa  xmm0, xmm1
    movdqu  xmm0, xmm1
    movaps  xmm0, xmm1
    movups  xmm0, xmm1
    movdqa  xmm0, xmmword ptr [r15+16]
    movdqu  xmm0, xmmword ptr [r15+19]
    movdqa  xmmword ptr [r15+80], xmm1
    movd    xmm0, ecx
    movq    xmm0, rcx
    movd    ecx, xmm1
    movq    rcx, xmm1
    movq    xmm0, qword ptr [r15+8]
    movlps  xmm0, qword ptr [r15+8]
    movhps  xmm0, qword ptr [r15+8]

/* ---- tier 2: scalar float ------------------------------------------ */
    movss   xmm0, xmm1
    movsd   xmm0, xmm1
    movss   xmm0, dword ptr [r15+4]
    movsd   xmm0, qword ptr [r15+8]
    addss   xmm0, xmm1
    addsd   xmm0, xmm1
    subss   xmm0, xmm1
    subsd   xmm0, xmm1
    mulss   xmm0, xmm1
    mulsd   xmm0, xmm1
    divss   xmm0, xmm1
    divsd   xmm0, xmm1
    minss   xmm0, xmm1
    maxsd   xmm0, xmm1
    sqrtsd  xmm0, xmm4
    comiss  xmm0, xmm1
    comisd  xmm0, xmm1
    ucomiss xmm0, xmm1
    ucomisd xmm0, xmm1
    cvtsi2sd xmm0, rcx
    cvtsi2ss xmm0, ecx
    cvttss2si eax, xmm1
    cvttsd2si rax, xmm1
    cvtss2sd xmm0, xmm1
    cvtsd2ss xmm0, xmm1
    andps   xmm0, xmm1
    andnps  xmm0, xmm1
    orps    xmm0, xmm1
    xorps   xmm0, xmm1

/* ---- the rest of tier 1: rotates, bit tests, byte swap ------------- */
    rol     rax, 1
    rol     rax, 7
    rol     eax, 13
    ror     rax, 5
    ror     ecx, 3
    rol     rax, cl
    ror     eax, cl
    bswap   rax
    bswap   ecx
    bt      rax, 5
    bt      rax, rcx
    bts     rax, 9
    btr     rax, 40
    btc     rcx, 3

/* ---- the tier-3 operations added after the first coverage pass ----- */
    pmuludq xmm0, xmm1
    pmuldq  xmm0, xmm1
    palignr xmm0, xmm1, 5
    palignr xmm0, xmm1, 11
    pshuflw xmm0, xmm1, 0x1b
    pshufhw xmm0, xmm1, 0x39
    pinsrw  xmm0, ecx, 3
    pextrw  eax, xmm1, 5
    unpcklps xmm0, xmm1
    unpckhps xmm0, xmm1
    unpcklpd xmm0, xmm1
    unpckhpd xmm0, xmm1
    movlhps xmm0, xmm1
    movhlps xmm0, xmm1
    shufps  xmm0, xmm1, 0x1b
    shufpd  xmm0, xmm1, 0x2
    addps   xmm0, xmm1
    subps   xmm0, xmm1
    mulps   xmm0, xmm1
    divps   xmm0, xmm1
    addpd   xmm0, xmm1
    mulpd   xmm0, xmm1
    pmaddubsw xmm0, xmm1
    pmulhrsw xmm0, xmm1
    pminsd  xmm0, xmm1
    pmaxud  xmm0, xmm1
    pmulld  xmm0, xmm1
    pcmpeqq xmm0, xmm1
    pcmpgtq xmm0, xmm1

/* ---- AVX2: the same tier-3 operations at 256 bits ------------------ */
    vpaddb  ymm0, ymm1, ymm2
    vpaddw  ymm0, ymm1, ymm2
    vpaddd  ymm0, ymm1, ymm2
    vpaddq  ymm0, ymm1, ymm2
    vpsubw  ymm0, ymm1, ymm2
    vpsubusb ymm0, ymm1, ymm2
    vpaddsw ymm0, ymm1, ymm2
    vpavgb  ymm0, ymm1, ymm2
    vpmullw ymm0, ymm1, ymm2
    vpmulhw ymm0, ymm1, ymm2
    vpmaddwd ymm0, ymm1, ymm2
    vpmaddubsw ymm0, ymm1, ymm2
    vpmulhrsw ymm0, ymm1, ymm2
    vpsadbw ymm0, ymm1, ymm2
    vpminsw ymm0, ymm1, ymm2
    vpmaxub ymm0, ymm1, ymm2
    vpabsw  ymm0, ymm1
    vpcmpeqb ymm0, ymm1, ymm2
    vpcmpgtw ymm0, ymm1, ymm2
    vpand   ymm0, ymm1, ymm2
    vpandn  ymm0, ymm1, ymm2
    vpor    ymm0, ymm1, ymm2
    vpxor   ymm0, ymm1, ymm2
    vpsllw  ymm0, ymm1, 3
    vpsrld  ymm0, ymm1, 5
    vpsraw  ymm0, ymm1, 3
    vpslldq ymm0, ymm1, 3
    vpunpcklbw ymm0, ymm1, ymm2
    vpunpckhwd ymm0, ymm1, ymm2
    vpshufd ymm0, ymm1, 0x1b
    vpshufb ymm0, ymm1, ymm2
    vpshuflw ymm0, ymm1, 0x39
    vpacksswb ymm0, ymm1, ymm2
    vpackuswb ymm0, ymm1, ymm2
    vpalignr ymm0, ymm1, ymm2, 5
    vpmuludq ymm0, ymm1, ymm2
    vpmovmskb eax, ymm1
    vmovdqa ymm0, ymm1
    vmovdqu ymm0, ymmword ptr [r15+16]
    vmovdqa ymmword ptr [r15+96], ymm1
    vaddps  ymm0, ymm1, ymm2
    vmulpd  ymm0, ymm1, ymm2
/* the 128-bit VEX forms, which zero the upper half where the legacy ones
   leave it alone -- the distinction only a 256-bit corpus can see */
    vpaddw  xmm0, xmm1, xmm2
    vpxor   xmm0, xmm1, xmm2
    vmovdqa xmm0, xmm1
    vmovd   xmm0, ecx
    vmovq   xmm0, rcx

/* ---- the lane-crossing operations, which have no SSE counterpart --- */
    vpbroadcastb ymm0, xmm1
    vpbroadcastw ymm0, xmm1
    vpbroadcastd ymm0, xmm1
    vpbroadcastq ymm0, xmm1
    vpbroadcastd xmm0, xmm1
    vbroadcastss ymm0, xmm1
    vbroadcastsd ymm0, xmm1
    /* vbroadcasti128 is left out: Capstone 4.0.2 does not decode it at
       all, so it can neither be checked here nor ever reach the
       interpreter -- a decoder gap, not a semantics one. */
    vinserti128 ymm0, ymm1, xmm2, 1
    vinserti128 ymm0, ymm1, xmm2, 0
    vextracti128 xmm0, ymm1, 1
    vextracti128 xmm0, ymm1, 0
    vperm2i128 ymm0, ymm1, ymm2, 0x31
    vperm2i128 ymm0, ymm1, ymm2, 0x20
    vpermq  ymm0, ymm1, 0x1b
    vpermd  ymm0, ymm1, ymm2
    vzeroupper

/* ---- the last of the cheap tier-3 forms ---------------------------- */
    pmovzxbw xmm0, xmm1
    pmovzxbd xmm0, xmm1
    pmovzxwd xmm0, xmm1
    pmovzxdq xmm0, xmm1
    pmovsxbw xmm0, xmm1
    pmovsxwd xmm0, xmm1
    pmovsxdq xmm0, xmm1
    vpmovzxbw ymm0, xmm1
    vpmovsxwd ymm0, xmm1
    packusdw xmm0, xmm1
    vpackusdw ymm0, ymm1, ymm2
    psignb  xmm0, xmm1
    psignw  xmm0, xmm1
    psignd  xmm0, xmm1
    pblendw xmm0, xmm1, 0x5a
    vpblendw ymm0, ymm1, ymm2, 0xa5
    movddup xmm0, xmm1
    movsldup xmm0, xmm1
    movshdup xmm0, xmm1

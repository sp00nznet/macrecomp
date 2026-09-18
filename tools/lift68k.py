#!/usr/bin/env python3
"""Lift decrypted 68000 Mac CODE segments to C, against the macrecomp runtime.

One C function per 68k subroutine (boundaries from the A5 jump-table directory).
Each instruction -> C over the M68K state; branches -> goto; A-line traps ->
m68k_trap(); inter-segment jsr d(a5) -> m68k_jt_call(). Instructions outside the
implemented set emit m68k_unimplemented() and are counted, so coverage is honest.

Usage:
  python lift68k.py --seg 2 work/unpacked/CODE_2.dec.bin \
      --jt work/unpacked/jumptable.json -o src/gen
"""
import argparse, json, os, re
from collections import Counter
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000); md.skipdata = True
STAT = {"total": 0, "unimpl": 0, "traps": 0}
# Instructions whose operands parse() could not read, by "mnemonic operands".
# Ranked at the end of a run: the top entries are the addressing modes worth
# teaching parse() next.
UNPARSED = Counter()

CC = {"ra":"1","t":"1","f":"0","hi":"(!M.c&&!M.z)","ls":"(M.c||M.z)","cc":"(!M.c)",
      "hs":"(!M.c)","cs":"(M.c)","lo":"(M.c)","ne":"(!M.z)","eq":"(M.z)","vc":"(!M.v)",
      "vs":"(M.v)","pl":"(!M.n)","mi":"(M.n)","ge":"(M.n==M.v)","lt":"(M.n!=M.v)",
      "gt":"(!M.z&&M.n==M.v)","le":"(M.z||M.n!=M.v)"}
SZ = {"b":1,"w":2,"l":4}


def size_of(mn):
    m = re.search(r"\.(b|w|l)$", mn); return SZ[m.group(1)] if m else 2

def rd(sz,a): return f"m68k_r{sz*8}({a})"
def wr(sz,a,v): return f"m68k_w{sz*8}({a},{v});"


class Op:
    def __init__(s,r=None,w=None,pre=(),post=(),addr=None,imm=None,areg=None):
        s.r,s.w,s.pre,s.post,s.addr,s.imm,s.areg = r,w,list(pre),list(post),addr,imm,areg


def mem(addr,sz,pre=(),post=()):
    return Op(r=rd(sz,addr), w=lambda v: wr(sz,addr,v), pre=pre, post=post, addr=addr)


def norm_op(tok):
    """capstone writes indexed modes as "$74(a5, d7.w)"; every operand pattern
    here spells them without the space. Normalise in one place -- missing this
    silently drops a whole addressing mode to unimplemented."""
    return re.sub(r",\s+", ",", tok.strip())


def parse(tok,sz):
    t=norm_op(tok)
    if (m:=re.fullmatch(r"d(\d)",t)):
        n=int(m.group(1))
        r = f"M.d[{n}]" if sz==4 else f"(uint{sz*8}_t)M.d[{n}]"
        w = (lambda v:f"SET_DL({n},{v});") if sz==4 else \
            (lambda v:f"SET_DW({n},{v});") if sz==2 else (lambda v:f"SET_DB({n},{v});")
        return Op(r=r,w=w)
    if (m:=re.fullmatch(r"a(\d)",t)):
        n=int(m.group(1)); return Op(r=f"M.a[{n}]",w=lambda v:f"M.a[{n}]=(uint32_t)({v});",areg=n)
    if (m:=re.fullmatch(r"#(-?\$?[0-9a-fA-F]+)",t)):
        v=int(m.group(1).replace("$","0x"),0)&0xFFFFFFFF; return Op(r=f"0x{v:x}u",imm=v)
    if (m:=re.fullmatch(r"-\(a(\d)\)",t)):
        n=int(m.group(1)); inc=2 if(n==7 and sz==1)else sz
        return mem(f"M.a[{n}]",sz,pre=[f"M.a[{n}]-={inc};"])
    if (m:=re.fullmatch(r"\(a(\d)\)\+",t)):
        n=int(m.group(1)); inc=2 if(n==7 and sz==1)else sz
        return mem(f"M.a[{n}]",sz,post=[f"M.a[{n}]+={inc};"])
    if (m:=re.fullmatch(r"\(a(\d)\)",t)):
        return mem(f"M.a[{int(m.group(1))}]",sz)
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(a(\d)\)",t)):
        d=int(m.group(1).replace("$","0x"),0); n=int(m.group(2)); return mem(f"(M.a[{n}]+{d})",sz)
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(a(\d),(d|a)(\d)\.(w|l)\)",t)):
        d=int(m.group(1).replace("$","0x"),0); n=int(m.group(2))
        xr=m.group(3); xn=int(m.group(4)); xl=m.group(5)=="l"
        idx=f"M.{xr}[{xn}]" if xl else f"(int32_t)(int16_t)M.{xr}[{xn}]"
        return mem(f"(M.a[{n}]+{d}+{idx})",sz)
    if (m:=re.fullmatch(r"\(a(\d),(d|a)(\d)\.(w|l)\)",t)):   # (An,Xn) -- no displacement
        n=int(m.group(1)); xr=m.group(2); xn=int(m.group(3)); xl=m.group(4)=="l"
        idx=f"M.{xr}[{xn}]" if xl else f"(int32_t)(int16_t)M.{xr}[{xn}]"
        return mem(f"(M.a[{n}]+{idx})",sz)
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(pc,(d|a)(\d)\.(w|l)\)",t)):
        off=int(m.group(1).replace("$","0x"),0)&0xFFFFFFFF
        xr=m.group(2); xn=int(m.group(3)); xl=m.group(4)=="l"
        idx=f"M.{xr}[{xn}]" if xl else f"(int32_t)(int16_t)M.{xr}[{xn}]"
        return mem(f"(g_seg_base+0x{off:x}u+{idx})",sz)
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(pc\)",t)):
        off=int(m.group(1).replace("$","0x"),0)&0xFFFFFFFF; return mem(f"(g_seg_base+0x{off:x}u)",sz)
    if (m:=re.fullmatch(r"\$?([0-9a-fA-F]+)\.(w|l)",t)):
        return mem(f"0x{int(m.group(1),16):x}u",sz)
    return None


def not_68000(code, start, probe=16, window=96):
    """True if disassembling from `start` yields forms a 68000 cannot encode.

    capstone decodes 68020 addressing even in M68K_000 mode, so memory-indirect
    `([$fffc,a2])` and a scaled index `(a0,d3.l * 4)` both render happily -- but
    a 68000 binary cannot contain either, and neither can an undecodable
    extension word. Their presence at a candidate function start proves it is
    not an instruction boundary: data, or a stream begun mid-instruction.

    Same argument as the word-alignment rule: an architectural impossibility,
    not a heuristic. Only the first `probe` instructions are examined, because
    real functions often carry data (jump tables, strings) after their code.
    """
    seen = 0
    for ins in md.disasm(code[start:min(start+window, len(code))], start):
        if seen >= probe:
            break
        seen += 1
        op = ins.op_str
        if "([" in op:                                   # 68020 memory indirect
            return True
        if re.search(r"\.[wl]\s*\*\s*[248]\b", op):      # 68020 scaled index
            return True
        if "invalid" in op:                              # undecodable extension word
            return True
    return False


def ops_of(ins):
    out,depth,cur="",0,""
    res=[]
    for ch in ins.op_str:
        if ch=='(':depth+=1
        if ch==')':depth-=1
        if ch==',' and depth==0: res.append(cur); cur=""
        else: cur+=ch
    if cur.strip(): res.append(cur)
    return res


def unimpl(ins):
    STAT["unimpl"]+=1
    return f'm68k_unimplemented("{ins.mnemonic} {ins.op_str}", g_seg_base+0x{ins.address:x}u);'


def btarget(tok):
    m=re.fullmatch(r"\$?([0-9a-fA-F]+)",tok.strip()); return int(m.group(1),16) if m else None

def dnum(tok):
    m=re.search(r"\bd(\d)\b",tok); return int(m.group(1))
def anum(tok):
    m=re.search(r"\ba(\d)\b",tok); return int(m.group(1))

def reglist(tok):
    """Expand a movem list like 'd0-d2/a0-a1' -> ['d0','d1','d2','a0','a1']."""
    regs=[]
    for part in tok.strip().split("/"):
        if (m:=re.fullmatch(r"([da])(\d)-([da])(\d)",part)):
            t=m.group(1); regs+=[f"{t}{i}" for i in range(int(m.group(2)),int(m.group(4))+1)]
        elif (m:=re.fullmatch(r"([da])(\d)",part)):
            regs.append(part)
    return regs

def regref(r):    # 'd3'->'M.d[3]', 'a5'->'M.a[5]'
    return f"M.{r[0]}[{r[1]}]"


def indirect_call(tok):
    t=norm_op(tok)
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(a5\)",t)):
        return f"m68k_jt_call(0x{int(m.group(1).replace('$','0x'),0)&0xffff:x}u);"
    if (m:=re.fullmatch(r"\(a(\d)\)",t)): return f"m68k_call(M.a[{m.group(1)}]);"
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(a(\d)\)",t)):
        d=int(m.group(1).replace("$","0x"),0); n=int(m.group(2)); return f"m68k_call(M.a[{n}]+{d});"
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(pc\)",t)):    # pc-relative -> local call
        return f"m68k_call(g_seg_base+0x{int(m.group(1).replace('$','0x'),0)&0xffffffff:x}u);"
    return None


def indirect_jump(tok):
    """A jmp is a tail transfer (goto/tail-call), NOT a subroutine call: it must
    not push a return address. Mirror indirect_call but with m68k_jump."""
    t=norm_op(tok)
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(a5\)",t)):
        return f"m68k_jt_jump(0x{int(m.group(1).replace('$','0x'),0)&0xffff:x}u);"
    if (m:=re.fullmatch(r"\(a(\d)\)",t)): return f"m68k_jump(M.a[{m.group(1)}]);"
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(a(\d)\)",t)):
        d=int(m.group(1).replace("$","0x"),0); n=int(m.group(2)); return f"m68k_jump(M.a[{n}]+{d});"
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(pc\)",t)):
        return f"m68k_jump(g_seg_base+0x{int(m.group(1).replace('$','0x'),0)&0xffffffff:x}u);"
    # Computed jump (the 68k switch). Lifted for completeness; note that
    # m68k_jump can only resolve a registered function start, so a computed
    # target landing mid-function still fails -- see ROADMAP, entry dispatch.
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(pc,(d|a)(\d)\.(w|l)\)",t)):
        off=int(m.group(1).replace("$","0x"),0)&0xFFFFFFFF
        xr=m.group(2); xn=int(m.group(3)); xl=m.group(4)=="l"
        idx=f"M.{xr}[{xn}]" if xl else f"(int32_t)(int16_t)M.{xr}[{xn}]"
        return f"m68k_jump(g_seg_base+0x{off:x}u+{idx});"
    return None


def brto(t, ins):
    """A goto, with a loop tick on backward branches.

    Any loop in the original code goes round a backward branch, so ticking only
    those finds a spin without instrumenting every instruction. MR_LOOPTICK
    compiles to nothing unless MACRECOMP_LOOPGUARD is defined."""
    if t <= ins.address:
        return f"MR_LOOPTICK(0x{ins.address:x}u); goto L{t:x};"
    return f"goto L{t:x};"


def emit(ins, targets):
    """Lift one instruction, degrading to a counted m68k_unimplemented() stub
    when an operand form parse() does not know turns up.

    Most branches below check their operands, but not all did, and an unhandled
    addressing mode used to take down the whole segment with an AttributeError
    on None. A single unknown mode is a coverage number, not a build failure --
    which is what this file's header promises. The form is recorded in UNPARSED
    so the gap can be ranked and closed."""
    saved = dict(STAT)
    try:
        return emit_inner(ins, targets)
    except (AttributeError, TypeError, IndexError, ValueError):
        STAT.clear(); STAT.update(saved)          # discard the partial attempt
        UNPARSED[f"{ins.mnemonic} {ins.op_str}"] += 1
        return [unimpl(ins)], False


def emit_inner(ins, targets):
    mn=ins.mnemonic; base=mn.split(".")[0]; sz=size_of(mn); ops=ops_of(ins)
    C=[]; term=False
    P=lambda tok,s=sz: parse(tok,s)

    def two(sz2=sz):
        a,b=P(ops[0],sz2),P(ops[1],sz2)
        if not a or not b: return None
        C.extend(a.pre); C.extend(b.pre); return a,b

    if base in ("move",):
        # Operand order matters when both operands touch the same address
        # register. The 68000 fetches the source -- applying its postincrement
        # -- and only then computes the destination address. The Pascal epilogue
        # `move.l (a7)+,(a7)` depends on exactly that: it lifts the return
        # address over the parameter, and A7 must already have moved when the
        # destination is worked out. Emitting both increments at the end makes
        # it a no-op that leaves the return address in place, and the caller
        # then resumes four bytes low and reads its result out of the argument.
        a,b=P(ops[0]),P(ops[1])
        if a and b:
            C.extend(a.pre)
            mid=" ".join(list(a.post)+list(b.pre))
            C.append(f"{{ uint32_t _r={a.r}; {mid} {b.w('_r')} fl_logic(_r,{sz}); }}")
            C.extend(b.post)
        else: C.append(unimpl(ins))
    elif base=="movea":
        # Same ordering rule as `move`, and it bites hardest here: the variadic
        # glue ends `movea.l (a7)+,a7`, which loads a new stack pointer off the
        # old stack. The postincrement belongs to the source fetch, so applying
        # it after the write adds 4 to the register just loaded -- the stack
        # pointer drifts and eventually leaves the address space entirely.
        a=P(ops[0]); n=P(ops[1],4).areg
        if a and n is not None:
            C.extend(a.pre); src=a.r if sz==4 else f"(uint32_t)(int32_t)(int16_t)({a.r})"
            post=" ".join(a.post)
            C.append(f"{{ uint32_t _r={src}; {post} M.a[{n}]=_r; }}")
        else: C.append(unimpl(ins))
    elif base=="exg":
        # Always a full 32-bit swap, whatever the registers are.
        def _reg(tok):
            m=re.fullmatch(r"([da])(\d)", norm_op(tok))
            return f"M.{m.group(1)}[{m.group(2)}]" if m else None
        x,y=_reg(ops[0]),_reg(ops[1])
        C.append(f"{{ uint32_t _t={x}; {x}={y}; {y}=_t; }}" if x and y else unimpl(ins))
    elif base=="moveq":
        # moveq carries an 8-bit immediate and **sign-extends it to 32 bits**.
        # Taking it as unsigned turns the idiomatic `moveq #-1,dN` -- which
        # spells a "not found" or "end of list" result -- into 255, and the
        # caller comparing against -1 never matches.
        b=P(ops[1],4); v=P(ops[0]).imm & 0xFF
        C.append(f"{{ uint32_t _r=(uint32_t)(int32_t)(int8_t)0x{v:02x}; {b.w('_r')} fl_logic(_r,4); }}")
    elif base=="lea":
        a=P(ops[0]); n=P(ops[1],4).areg
        C.append(f"M.a[{n}]={a.addr};" if a and a.addr is not None and n is not None else unimpl(ins))
    elif base=="pea":
        a=P(ops[0]); C.append(f"SP-=4; m68k_w32(SP,{a.addr});" if a and a.addr is not None else unimpl(ins))
    elif base=="clr":
        b=P(ops[0]); C.extend(b.pre); C.append(b.w("0")); C.append(f"fl_logic(0,{sz});"); C.extend(b.post)
    elif base=="tst":
        a=P(ops[0]); C.extend(a.pre); C.append(f"fl_logic({a.r},{sz});"); C.extend(a.post)
    elif base in ("add","addi","addq","sub","subi","subq"):
        r=two()
        if r:
            a,b=r; op="+" if base.startswith("add") else "-"; fn="fl_add" if op=="+" else "fl_sub"
            if b.areg is not None:
                # An destination: the 68000 does not touch the condition codes,
                # and operates on the whole register whatever the size suffix
                # says. Setting flags here quietly changes the branch a caller
                # takes next -- `addq #1,a0` must leave a preceding tst alone.
                C.append(f"M.a[{b.areg}] = M.a[{b.areg}] {op} ({a.r});")
            else:
                C.append(f"{{ uint32_t _s={a.r},_d={b.r},_r=(_d{op}_s); {b.w('_r')} {fn}(_s,_d,_r,{sz}); }}")
            C.extend(a.post+b.post)
        else: C.append(unimpl(ins))
    elif base in ("and","andi","or","ori","eor","eori"):
        r=two()
        if r:
            a,b=r; op={"and":"&","or":"|","eor":"^"}[base.rstrip("i")]
            C.append(f"{{ uint32_t _r=({b.r}{op}{a.r}); {b.w('_r')} fl_logic(_r,{sz}); }}")
            C.extend(a.post+b.post)
        else: C.append(unimpl(ins))
    elif base in ("adda","suba"):
        a=P(ops[0],4); n=P(ops[1],4).areg
        if a and n is not None:
            src=a.r if sz==4 else f"(uint32_t)(int32_t)(int16_t)({a.r})"
            C.extend(a.pre); C.append(f"M.a[{n}]{'+' if base=='adda' else '-'}={src};"); C.extend(a.post)
        else: C.append(unimpl(ins))
    elif base in ("cmp","cmpi","cmpa","cmpm"):
        # cmpm compares memory to memory with both operands postincrementing --
        # the string-compare instruction. It sets flags exactly like cmp, so it
        # belongs here; left out, every byte-by-byte name comparison in a title
        # silently does nothing. HyperCard uses it to check whether the file it
        # just opened really is the home stack.
        s2=4 if base=="cmpa" else sz; r=two(s2)
        if r: a,b=r; C.append(f"fl_cmp({a.r},{b.r},({b.r}-{a.r}),{s2});"); C.extend(a.post+b.post)
        else: C.append(unimpl(ins))
    elif base=="not":
        b=P(ops[0]); C.extend(b.pre); C.append(f"{{ uint32_t _r=~{b.r}; {b.w('_r')} fl_logic(_r,{sz}); }}"); C.extend(b.post)
    elif base=="neg":
        b=P(ops[0]); C.extend(b.pre); C.append(f"{{ uint32_t _d={b.r},_r=(uint32_t)(-(int32_t)_d); {b.w('_r')} fl_sub(_d,0,_r,{sz}); }}"); C.extend(b.post)
    elif base=="ext":
        n=dnum(ops[0]); C.append(f"SET_DL({n},(uint32_t)(int32_t)(int16_t)M.d[{n}]);" if sz==4
                                    else f"SET_DW({n},(uint16_t)(int16_t)(int8_t)M.d[{n}]);")
        C.append(f"fl_logic(M.d[{n}],{sz});")
    elif base=="swap":
        n=dnum(ops[0]); C.append(f"M.d[{n}]=(M.d[{n}]>>16)|(M.d[{n}]<<16); fl_logic(M.d[{n}],4);")
    elif base in ("muls","mulu"):
        a=P(ops[0],2); n=dnum(ops[1]); cast="(int32_t)(int16_t)" if base=="muls" else "(uint32_t)(uint16_t)"
        C.append(f"SET_DL({n},(uint32_t)({cast}({a.r})*{cast}M.d[{n}])); fl_logic(M.d[{n}],4);")
    elif base=="link":
        n=anum(ops[0]); d=P(ops[1]).imm
        C.append(f"SP-=4; m68k_w32(SP,M.a[{n}]); M.a[{n}]=SP; SP+=(int32_t)(int16_t)0x{d&0xffff:x};")
    elif base=="unlk":
        n=anum(ops[0]); C.append(f"SP=M.a[{n}]; M.a[{n}]=m68k_r32(SP); SP+=4;")
    elif base=="movem":
        regs=reglist(ops[0]) if re.search(r"[/-]|^[da]\d$",ops[0].strip()) else None
        if regs is None and re.search(r"[/-]",ops[1]): regs=reglist(ops[1])
        pre=P(ops[1]); post=P(ops[0])
        if re.fullmatch(r"\s*-\(a\d\)\s*",ops[1]):        # store: movem regs,-(An)
            n=anum(ops[1]); regs=reglist(ops[0])
            for r in reversed(regs): C.append(f"SP-=4;" if n==7 else f"M.a[{n}]-=4;"); C.append(
                (f"m68k_w32(SP,{regref(r)});" if n==7 else f"m68k_w32(M.a[{n}],{regref(r)});"))
        elif re.fullmatch(r"\s*\(a\d\)\+\s*",ops[0]):     # load: movem (An)+,regs
            n=anum(ops[0]); regs=reglist(ops[1])
            for r in regs:
                C.append((f"{regref(r)}=m68k_r32(SP); SP+=4;" if n==7
                          else f"{regref(r)}=m68k_r32(M.a[{n}]); M.a[{n}]+=4;"))
        else:
            # General effective-address forms: movem <ea>,regs and movem regs,<ea>.
            # Registers transfer in ascending order (d0-d7 then a0-a7) from the
            # EA upward; only the predecrement form above reverses. This is the
            # frame-pointer epilogue -- `movem.l -$10(a6),d2-d3/a2-a3` -- so it
            # shows up in almost every compiled routine.
            load = bool(reglist(ops[1]))
            regs = reglist(ops[1]) if load else reglist(ops[0])
            ea   = P(ops[0],4) if load else P(ops[1],4)
            if regs and ea is not None and ea.addr is not None:
                C.append(f"{{ uint32_t _ea={ea.addr};")
                for i,r in enumerate(regs):
                    off=i*sz
                    if load:
                        v=f"m68k_r{sz*8}(_ea+{off}u)"
                        if sz==2: v=f"(uint32_t)(int32_t)(int16_t)({v})"
                        C.append(f"  {regref(r)}={v};")
                    else:
                        C.append(f"  m68k_w{sz*8}(_ea+{off}u,{regref(r)});")
                C.append("}")
            else: C.append(unimpl(ins))
    elif base in ("lsl","lsr","asl","asr","rol","ror"):
        fn={"lsl":"m68k_lsl","lsr":"m68k_lsr","asl":"m68k_asl","asr":"m68k_asr",
            "rol":"m68k_rol","ror":"m68k_ror"}[base]
        cnt = P(ops[0]).imm if P(ops[0]) and P(ops[0]).imm is not None else None
        if len(ops)==2 and cnt is not None:              # #cnt,Dn
            n=dnum(ops[1]); wf={1:"SET_DB",2:"SET_DW",4:"SET_DL"}[sz]; rf={1:"DB",2:"DW",4:"DL"}[sz]
            C.append(f"{wf}({n},{fn}({rf}({n}),{cnt},{sz}));")
        elif len(ops)==2:                                 # Dm,Dn (count in Dm mod 64)
            m2=dnum(ops[0]); n=dnum(ops[1]); wf={1:"SET_DB",2:"SET_DW",4:"SET_DL"}[sz]; rf={1:"DB",2:"DW",4:"DL"}[sz]
            C.append(f"{wf}({n},{fn}({rf}({n}),M.d[{m2}]&63,{sz}));")
        else: C.append(unimpl(ins))
    elif base in ("divu","divs"):
        a=P(ops[0],2); n=dnum(ops[1])
        if base=="divs":
            C.append(f"{{ int16_t _s=(int16_t)({a.r}); if(_s){{ int32_t _dd=(int32_t)M.d[{n}];"
                     f" uint32_t _q=(uint32_t)(_dd/_s), _r=(uint32_t)(_dd%_s);"
                     f" SET_DL({n},((_r&0xffff)<<16)|(_q&0xffff)); fl_logic(_q,2); }} }}")
        else:
            C.append(f"{{ uint16_t _s=(uint16_t)({a.r}); if(_s){{ uint32_t _dd=M.d[{n}];"
                     f" uint32_t _q=_dd/_s, _r=_dd%_s;"
                     f" SET_DL({n},((_r&0xffff)<<16)|(_q&0xffff)); fl_logic(_q,2); }} }}")
    elif base in ("btst","bset","bclr","bchg"):
        a=P(ops[0]); b=P(ops[1] if len(ops)>1 else ops[0])
        onreg = re.fullmatch(r"\s*d\d\s*",(ops[1] if len(ops)>1 else "")) is not None
        modw = 32 if onreg else 8
        bit = f"({a.r}%{modw})" if a.imm is None else f"{a.imm%modw}"
        C.extend(b.pre)
        C.append(f"M.z=(({b.r}>>{bit})&1)==0;")
        if base!="btst":
            opx={"bset":"|=","bclr":"&=~","bchg":"^="}[base]
            C.append(f"{{ uint32_t _t={b.r}; _t {opx} (1u<<{bit}); {b.w('_t')} }}")
        C.extend(b.post)
    elif base in ("jsr","bsr"):
        t=btarget(ops[0])
        C.append(f"m68k_call(g_seg_base+0x{t:x}u);" if t is not None else (indirect_call(ops[0]) or unimpl(ins)))
    elif base=="jmp":
        t=btarget(ops[0])
        if t is not None: C.append(brto(t,ins)); term=True; targets.add(t)
        else: C.append(indirect_jump(ops[0]) or unimpl(ins)); C.append("return;"); term=True
    elif base=="rts": C.append("m68k_rts(); return;"); term=True
    elif base=="nop": C.append(";")
    elif base=="bra": t=btarget(ops[0]); C.append(brto(t,ins)); term=True; targets.add(t)
    elif re.fullmatch(r"b(hi|ls|cc|hs|cs|lo|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)",base):
        t=btarget(ops[0]); C.append(f"if({CC[base[1:]]}){{ {brto(t,ins)} }}"); targets.add(t)
    elif base.startswith("db"):
        # DBcc loops while its condition is **false**, which is the opposite of
        # Bcc. `dbra` is the assembler's spelling of `dbf` -- condition false --
        # so it always decrements and branches until the counter reaches -1.
        # Looking "ra" up in the branch table yields "always true", which makes
        # `if(!cc)` unreachable and turns every counted loop in the program into
        # a no-op: the body never runs and the counter never moves. 563 of
        # HyperCard's loops were dead this way.
        suffix=base[2:]
        cc="0" if suffix in ("ra","f") else CC.get(suffix,"0")
        n=dnum(ops[0]); t=btarget(ops[1]); targets.add(t)
        C.append(f"if(!{cc}){{ SET_DW({n},DW({n})-1); if((int16_t)DW({n})!=-1){{ {brto(t,ins)} }} }}")
    elif base.startswith("s") and base[1:] in CC:
        b=P(ops[0],1); C.extend(b.pre); C.append(b.w(f"({CC[base[1:]]}?0xff:0)")); C.extend(b.post)
    else: C.append(unimpl(ins))
    return C, term


# Inline-table `case` dispatchers (THINK C switch helpers): `jsr <a5off>(a5)` with
# the selector in d0, followed in the code stream by an inline table. The handler
# for a case is at (address of its offset word) + offset. Three codegens:
#   word  ($2a): [count] then count*(off:2, sel:2)     selector = (int16)d0
#   long  ($32): [count] then count*(off:2, sel:4)     selector = (int32)d0
#   dense ($3a): [low:2][high:2][default_off:2] then (high-low+1)*(off:2), indexed
# We lower each to a C switch. (These a5 offsets are Shufflepuck's seg-1 jt entries.)
DISPATCH_A5 = {0x2a:"word", 0x32:"long", 0x3a:"dense"}

def dispatch_kind(ins):
    if ins.id==0 or ins.mnemonic!="jsr": return None
    m=re.fullmatch(r"\$([0-9a-fA-F]+)\(a5\)", ins.op_str.strip())
    return DISPATCH_A5.get(int(m.group(1),16)) if m else None

def _s(v,bits): return v-(1<<bits) if v>=(1<<(bits-1)) else v

def read_dispatch(code, tbl, kind):
    """-> (entries[(sel,handler)], table_end, default_handler_or_None) or None."""
    if kind in ("word","long"):
        if tbl+2>len(code): return None
        count=(code[tbl]<<8)|code[tbl+1]
        if count>256: return None
        esz = 4 if kind=="word" else 6
        entries=[]
        for i in range(count):
            pi=tbl+2+esz*i
            if pi+esz>len(code): return None
            off=(code[pi]<<8)|code[pi+1]
            if kind=="word": sel=_s((code[pi+2]<<8)|code[pi+3],16)
            else: sel=_s((code[pi+2]<<24)|(code[pi+3]<<16)|(code[pi+4]<<8)|code[pi+5],32)
            h=pi+off
            if not(0<=h<len(code)): return None
            entries.append((sel,h))
        return entries, tbl+2+esz*count, None
    else:  # dense
        if tbl+6>len(code): return None
        low=_s((code[tbl]<<8)|code[tbl+1],16); high=_s((code[tbl+2]<<8)|code[tbl+3],16)
        n=high-low+1
        if n<1 or n>2048: return None
        doff=(code[tbl+4]<<8)|code[tbl+5]; default_h=(tbl+4)+doff
        if not(0<=default_h<len(code)): return None
        entries=[]
        for k in range(n):
            pi=tbl+6+2*k
            if pi+2>len(code): return None
            off=(code[pi]<<8)|code[pi+1]; h=pi+off
            if not(0<=h<len(code)): return None
            entries.append((low+k,h))
        return entries, tbl+6+2*n, default_h

def disasm_one(code, pc):
    for i in md.disasm(code[pc:min(pc+24,len(code))], pc): return i
    return None


def lift_function(code, seg, start, end):
    # cursor pass: build a stream of ('ins',insn) / ('trapdata',addr,bytes) /
    # ('dispatch',addr,entries,table_end), skipping inline dispatch tables.
    stream=[]; pc=start
    while pc<end:
        ins=disasm_one(code,pc)
        if ins is None or ins.address!=pc:
            stream.append(("trapdata",pc,bytes(code[pc:pc+2]))); pc+=2; continue
        da=dispatch_kind(ins)
        if da is not None:
            d=read_dispatch(code, pc+ins.size, da)
            if d:
                entries,tend,defh=d
                stream.append(("dispatch",pc,entries,tend,defh,da)); pc=tend; continue
        if ins.id==0:
            stream.append(("trapdata",ins.address,bytes(ins.bytes)))
        else:
            stream.append(("ins",ins))
        pc+=ins.size

    # collect branch targets (emit side-effects) without disturbing coverage stats
    targets=set(); saved=dict(STAT)
    for it in stream:
        if it[0]=="ins": emit(it[1],targets)
        elif it[0]=="dispatch":
            for _,h in it[2]: targets.add(h)
            if it[4] is not None: targets.add(it[4])
    STAT.clear(); STAT.update(saved)

    # Every instruction boundary is a potential entry: the original code reaches
    # computed addresses through a register, and such a target is not a branch
    # target anywhere in the binary, so labelling only `targets` cannot reach it.
    # A label costs nothing at run time; the switch below is what makes it
    # addressable from outside.
    boundaries=[it[1].address if it[0]=="ins" else it[1]
                for it in stream if it[0] in ("ins","trapdata")]
    name=f"fn_{seg}_{start:04x}"; L=[f"void {name}(uint32_t entry){{"]
    if boundaries:
        L.append("  if(entry) switch(entry-g_seg_base){")
        for b in boundaries: L.append(f"   case 0x{b:x}u: goto L{b:x};")
        L.append("   default: m68k_entry_miss(entry); return; }")
    emitted=set(); last_term=True
    for it in stream:
        if it[0]=="dispatch":
            _,addr,entries,tend,defh,kind=it
            selexpr = "(int32_t)DL(0)" if kind=="long" else "(int16_t)DW(0)"
            L.append(f"  /* {addr:04x} case-dispatch ({kind}) -> {len(entries)} cases */")
            L.append(f"  switch({selexpr}){{")
            for sel,h in entries: L.append(f"   case {sel}: goto L{h:x};")
            L.append(f"   default: goto L{defh:x}; }}" if defh is not None else "   default: break; }")
            last_term=False; continue
        if it[0]=="trapdata":
            addr,b=it[1],it[2]
            L.append(f" L{addr:x}:;"); emitted.add(addr)
            STAT["total"]+=1
            if len(b)>=2 and 0xA0<=b[0]<=0xAF:
                w=(b[0]<<8)|b[1]
                STAT["traps"]+=1; L.append(f"  m68k_trap(0x{w:04x});")
                # Auto-pop (bit 10 of a Toolbox trap): the dispatcher returns to
                # the address on the stack instead of to the instruction after
                # the trap. The package glue uses it -- it pops its return
                # address, pushes the selector under it and traps -- so the trap
                # *is* the return. Without this the lifted code runs on into
                # whatever follows, which is the next glue entry.
                if (w & 0xF800) == 0xA800 and (w & 0x0400):
                    L.append("  m68k_rts(); return;   /* auto-pop trap returns */")
                    last_term=True; continue
            else: L.append(f"  /* data {b.hex()} */")
            last_term=False; continue
        ins=it[1]
        L.append(f" L{ins.address:x}:;"); emitted.add(ins.address)
        STAT["total"]+=1
        stmts,term=emit(ins,targets); last_term=term
        L.append(f"  /* {ins.address:04x} {ins.mnemonic} {ins.op_str} */")
        L+= [f"  {s}" for s in stmts]
    if not last_term and end < len(code):
        L.append(f"  m68k_jump(g_seg_base+0x{end:x}u); return; /* fall-through */")
    for t in sorted(targets - emitted):
        L.append(f" L{t:x}: m68k_jump(g_seg_base+0x{t:x}u); return;")
    L.append("}")
    return name,"\n".join(L),start,end


def looks_like_prologue(code, off):
    """True if `off` opens the way a compiled 68k function almost always does.

    `link aN,#d` sets up the frame; `movem.l <regs>,-(a7)` saves the callee-saved
    registers. Either is a strong signal, and neither is a plausible reading of
    the middle of some other instruction."""
    if off + 2 > len(code):
        return False
    w = (code[off] << 8) | code[off + 1]
    return 0x4E50 <= w <= 0x4E57 or w == 0x48E7


def confirm_starts(code, offs, trusted):
    """Drop candidate starts that do not land on an instruction boundary.

    Starts come partly from a linear sweep that drifts over embedded data, so a
    candidate can sit *inside* an instruction. Decoding forward from a start
    already known good gives the real boundaries, and a candidate that is not
    one of them is a misread -- this is the same rule the ROADMAP states for
    reading a single instruction, applied to function splitting.

    Getting this wrong is not a small error. A start one byte inside a 6-byte
    `move.l` truncated the function before its epilogue and made the remainder
    decode as a fresh function whose first instruction was `unlk a6` with no
    matching `link`. Every call walked the caller's frame pointer down four
    bytes, and the corruption surfaced much later as a null pointer in an
    unrelated subsystem.

    Jump-table entries are trusted outright: the Segment Loader itself enters
    there, so they are starts by definition even if the sweep disagrees."""
    good, cur = [], None
    for o in offs:
        if cur is None or o in trusted:
            good.append(o); cur = o; continue
        edge = min(o + 16, len(code))
        bounds = set()
        for ins in md.disasm(code[cur:edge], cur):
            if ins.address > o: break
            bounds.add(ins.address)
        # The reference decode is itself only as good as the bytes it crossed:
        # a string constant embedded in the body drifts it, and then a genuine
        # routine after that data looks mid-instruction. So a candidate that
        # opens with a prologue is kept anyway -- that is the stronger evidence,
        # and it is what resynchronises the decode.
        if o in bounds or looks_like_prologue(code, o):
            good.append(o); cur = o
    return good


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("segfile"); ap.add_argument("--seg",type=int,required=True)
    ap.add_argument("--jt",required=True); ap.add_argument("-o","--out",default="src/gen")
    ap.add_argument("--entry",default="",help="extra function offsets, comma-sep hex (e.g. 0x3838)")
    a=ap.parse_args(); os.makedirs(a.out,exist_ok=True)
    raw=open(a.segfile,"rb").read(); code=raw[4:]     # skip 4-byte seg header
    jt=json.load(open(a.jt))
    # function starts = jump-table entries for this segment + every intra-segment
    # bsr/jsr call target (local subroutines that have no jump-table entry).
    starts={e["offset"] for e in jt["entries"] if e["thunk"] and e["segment"]==a.seg}
    branches=[]                                            # (from_addr, target) for bra/bcc/dbcc/jmp
    for ins in md.disasm(code, 0):
        if not ins.id: continue
        mn=ins.mnemonic.split(".")[0]; op=ins.op_str.strip()
        pcrel=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(pc\)",op)
        if mn in ("bsr","jsr"):
            t=btarget(op)
            if t is None and pcrel: t=int(pcrel.group(1).replace("$","0x"),0)&0xFFFFFFFF
            if t is not None and 0<=t<len(code): starts.add(t)
        elif mn=="jmp" and pcrel:                          # tail-call jmp $x(pc)
            t=int(pcrel.group(1).replace("$","0x"),0)&0xFFFFFFFF
            if 0<=t<len(code): starts.add(t)
        if mn in ("bra","jmp") or mn.startswith("db") or re.fullmatch(r"b(hi|ls|cc|hs|cs|lo|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)",mn):
            bm=re.search(r"\$([0-9a-fA-F]+)\s*$", op)       # trailing bare-hex branch target
            if bm: branches.append((ins.address, int(bm.group(1),16)))

    for tok in a.entry.split(","):
        if tok.strip(): starts.add(int(tok,0))
    # refine: any branch whose target lands in a *different* function than the
    # branch (dual-entry / shared-tail routines) makes that target its own function
    for _ in range(6):
        offs=sorted(x for x in starts if x<len(code) and not(x&1))
        added=False
        for fa,t in branches:
            if not(0<=t<len(code)) or (t&1) or t in starts: continue
            # A branch can sit below every known entry (code ahead of the first
            # jump-table routine); that region starts at 0.
            s=max((o for o in offs if o<=fa), default=0)
            e=min([o for o in offs if o>fa]+[len(code)])
            if not(s<=t<e): starts.add(t); added=True
        if not added: break
    # The 68000 fetches instructions on word boundaries, so a function can never
    # start at an odd address. An odd "start" is a false positive from scanning
    # data as code: disassembling from it lands mid-instruction and every
    # instruction after it decodes as garbage, which then runs as if it were the
    # program. Drop them.
    dropped=sorted(x for x in starts if x<len(code) and (x&1))
    offs=sorted(x for x in starts if x<len(code) and not(x&1))
    # Second filter, same argument as the first: reject a start whose stream
    # decodes to addressing modes a 68000 does not have.
    notcode=[x for x in offs if not_68000(code, x)]
    if notcode:
        offs=[x for x in offs if x not in set(notcode)]
    # Third filter: a start must sit on an instruction boundary of the decode
    # that reaches it. See confirm_starts -- a mid-instruction start silently
    # produces a function with a stray epilogue.
    trusted={e["offset"] for e in jt["entries"] if e["thunk"] and e["segment"]==a.seg}
    for tok in a.entry.split(","):
        if tok.strip(): trusted.add(int(tok,0))
    before=len(offs)
    offs=confirm_starts(code, offs, trusted)
    midins=before-len(offs)
    bounds=list(zip(offs, offs[1:]+[len(code)]))
    fns=[]
    for start,end in bounds:
        if start>=len(code): continue
        fns.append(lift_function(code, a.seg, start, min(end,len(code))))
    out=[f'#include "macrecomp/m68k.h"',
         f"/* auto-generated by lift68k.py from CODE {a.seg} -- do not edit */",
         f"static uint32_t g_seg_base = 0; /* segment load base; set by register_seg_{a.seg} */",""]
    out+=[c for _,c,_,_ in fns]
    out.append(f"\nvoid register_seg_{a.seg}(uint32_t base){{")
    out.append(f"  g_seg_base = base;")
    for name,_,fstart,fend in fns:
        out.append(f"  m68k_register(base+0x{fstart:x}u, base+0x{fend:x}u, {name});")
    out.append("}")
    path=os.path.join(a.out,f"code_{a.seg}.c"); open(path,"w").write("\n".join(out))
    cov=100*(STAT["total"]-STAT["unimpl"])/max(STAT["total"],1)
    print(f"CODE {a.seg}: {len(fns)} functions, {STAT['total']} insns, "
          f"{STAT['unimpl']} unimplemented, {STAT['traps']} traps -> {path}")
    print(f"  instruction coverage: {cov:.1f}%")
    if dropped:
        print(f"  dropped {len(dropped)} odd-addressed function start(s) "
              f"(data misread as code)")
    if notcode:
        print(f"  dropped {len(notcode)} start(s) decoding to non-68000 forms "
              f"(data misread as code)")
    if midins:
        print(f"  dropped {midins} start(s) not on an instruction boundary "
              f"(the sweep drifted mid-instruction)")
    if UNPARSED:
        n = sum(UNPARSED.values())
        print(f"  {n} insn(s) in {len(UNPARSED)} form(s) had operands parse() "
              f"cannot read; most common:")
        for form, c in UNPARSED.most_common(12):
            print(f"    {c:5d}x  {form}")



if __name__=="__main__":
    main()

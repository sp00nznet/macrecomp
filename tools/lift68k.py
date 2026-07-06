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
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000); md.skipdata = True
STAT = {"total": 0, "unimpl": 0, "traps": 0}

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


def parse(tok,sz):
    t=tok.strip()
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
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(pc\)",t)):
        off=int(m.group(1).replace("$","0x"),0)&0xFFFFFFFF; return mem(f"(g_seg_base+0x{off:x}u)",sz)
    if (m:=re.fullmatch(r"\$?([0-9a-fA-F]+)\.(w|l)",t)):
        return mem(f"0x{int(m.group(1),16):x}u",sz)
    return None


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
    t=tok.strip()
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(a5\)",t)):
        return f"m68k_jt_call(0x{int(m.group(1).replace('$','0x'),0)&0xffff:x}u);"
    if (m:=re.fullmatch(r"\(a(\d)\)",t)): return f"m68k_call(M.a[{m.group(1)}]);"
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(a(\d)\)",t)):
        d=int(m.group(1).replace("$","0x"),0); n=int(m.group(2)); return f"m68k_call(M.a[{n}]+{d});"
    if (m:=re.fullmatch(r"(-?\$?[0-9a-fA-F]+)\(pc\)",t)):    # pc-relative -> local call
        return f"m68k_call(g_seg_base+0x{int(m.group(1).replace('$','0x'),0)&0xffffffff:x}u);"
    return None


def emit(ins, targets):
    mn=ins.mnemonic; base=mn.split(".")[0]; sz=size_of(mn); ops=ops_of(ins)
    C=[]; term=False
    P=lambda tok,s=sz: parse(tok,s)

    def two(sz2=sz):
        a,b=P(ops[0],sz2),P(ops[1],sz2)
        if not a or not b: return None
        C.extend(a.pre); C.extend(b.pre); return a,b

    if base in ("move",):
        r=two()
        if r: a,b=r; C.append(f"{{ uint32_t _r={a.r}; {b.w('_r')} fl_logic(_r,{sz}); }}"); C.extend(a.post+b.post)
        else: C.append(unimpl(ins))
    elif base=="movea":
        a=P(ops[0]); n=P(ops[1],4).areg
        if a and n is not None:
            C.extend(a.pre); src=a.r if sz==4 else f"(uint32_t)(int32_t)(int16_t)({a.r})"
            C.append(f"M.a[{n}]={src};"); C.extend(a.post)
        else: C.append(unimpl(ins))
    elif base=="moveq":
        b=P(ops[1],4); C.append(f"{{ uint32_t _r=0x{P(ops[0]).imm&0xffffffff:x}u; {b.w('_r')} fl_logic(_r,4); }}")
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
    elif base in ("cmp","cmpi","cmpa"):
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
        else: C.append(unimpl(ins))
    elif base in ("lsl","lsr","asl","asr"):
        fn={"lsl":"m68k_lsl","lsr":"m68k_lsr","asl":"m68k_asl","asr":"m68k_asr"}[base]
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
        if t is not None: C.append(f"goto L{t:x};"); term=True; targets.add(t)
        else: C.append(indirect_call(ops[0]) or unimpl(ins)); term=True
    elif base=="rts": C.append("return;"); term=True
    elif base=="nop": C.append(";")
    elif base=="bra": t=btarget(ops[0]); C.append(f"goto L{t:x};"); term=True; targets.add(t)
    elif re.fullmatch(r"b(hi|ls|cc|hs|cs|lo|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)",base):
        t=btarget(ops[0]); C.append(f"if({CC[base[1:]]}) goto L{t:x};"); targets.add(t)
    elif base.startswith("db"):
        cc=CC.get(base[2:],"0"); n=dnum(ops[0]); t=btarget(ops[1]); targets.add(t)
        C.append(f"if(!{cc}){{ SET_DW({n},DW({n})-1); if((int16_t)DW({n})!=-1) goto L{t:x}; }}")
    elif base.startswith("s") and base[1:] in CC:
        b=P(ops[0],1); C.extend(b.pre); C.append(b.w(f"({CC[base[1:]]}?0xff:0)")); C.extend(b.post)
    else: C.append(unimpl(ins))
    return C, term


def lift_function(code, seg, start, end):
    # pass 1: collect local branch targets (don't let it affect coverage stats)
    targets=set(); saved=dict(STAT)
    for ins in md.disasm(code[start:end], start):
        if ins.id==0: continue
        emit(ins, targets)
    STAT.clear(); STAT.update(saved)
    # pass 2: emit
    name=f"fn_{seg}_{start:04x}"; L=[f"void {name}(void){{"]; emitted=set()
    for ins in md.disasm(code[start:end], start):
        if ins.address in targets: L.append(f" L{ins.address:x}:;"); emitted.add(ins.address)
        STAT["total"]+=1
        b=ins.bytes
        if ins.id==0 and len(b)==2 and 0xA0<=b[0]<=0xAF:
            STAT["traps"]+=1; L.append(f"  m68k_trap(0x{(b[0]<<8)|b[1]:04x});"); continue
        if ins.id==0: L.append(f"  /* data {b.hex()} */"); continue
        stmts,_=emit(ins,targets)
        L.append(f"  /* {ins.address:04x} {ins.mnemonic} {ins.op_str} */")
        L+= [f"  {s}" for s in stmts]
    # trampolines for branch targets that land outside this function (tail calls
    # / shared handlers) -- call the containing function's address and return
    for t in sorted(targets - emitted):
        L.append(f" L{t:x}: m68k_call(g_seg_base+0x{t:x}u); return;")
    L.append("}")
    return name,"\n".join(L)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("segfile"); ap.add_argument("--seg",type=int,required=True)
    ap.add_argument("--jt",required=True); ap.add_argument("-o","--out",default="src/gen")
    a=ap.parse_args(); os.makedirs(a.out,exist_ok=True)
    raw=open(a.segfile,"rb").read(); code=raw[4:]     # skip 4-byte seg header
    jt=json.load(open(a.jt))
    offs=sorted({e["offset"] for e in jt["entries"] if e["thunk"] and e["segment"]==a.seg})
    offs=[o-4 if o>=4 else o for o in offs]           # jt offset is into code incl header; our code[] already dropped it? keep simple:
    offs=sorted({max(0,o) for o in [e["offset"] for e in jt["entries"] if e["thunk"] and e["segment"]==a.seg]})
    bounds=list(zip(offs, offs[1:]+[len(code)]))
    fns=[]
    for start,end in bounds:
        if start>=len(code): continue
        fns.append(lift_function(code, a.seg, start, min(end,len(code))))
    out=[f'#include "macrecomp/m68k.h"',
         f"/* auto-generated by lift68k.py from CODE {a.seg} -- do not edit */",
         f"static const uint32_t g_seg_base = 0; /* set by loader; PC-relative refs use it */",""]
    out+=[c for _,c in fns]
    out.append(f"\nvoid register_seg_{a.seg}(uint32_t base){{")
    out.append(f"  /* g_seg_base = base; (per-translation-unit; see runtime) */")
    for name,_ in fns:
        off=int(name.split("_")[-1],16)
        out.append(f"  m68k_register(base+0x{off:x}u, {name});")
    out.append("}")
    path=os.path.join(a.out,f"code_{a.seg}.c"); open(path,"w").write("\n".join(out))
    cov=100*(STAT["total"]-STAT["unimpl"])/max(STAT["total"],1)
    print(f"CODE {a.seg}: {len(fns)} functions, {STAT['total']} insns, "
          f"{STAT['unimpl']} unimplemented, {STAT['traps']} traps -> {path}")
    print(f"  instruction coverage: {cov:.1f}%")


if __name__=="__main__":
    main()

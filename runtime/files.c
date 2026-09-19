/* files.c - File Manager over the files extracted from a title's own media.
 *
 * HyperCard cannot open a stack without this, and a stack is the whole point:
 * the recompiled application is a player, and the documents are what it plays.
 * Until it lands, HyperCard fails its first Open, calls ParamText and puts up
 * its "can't open" dialog, which is exactly where the run used to stop.
 *
 * These are **OS traps**, not Pascal ones: A0 points at a ParamBlockRec and the
 * result code comes back in D0 (and in ioResult). Nothing is read off the 68k
 * stack here.
 *
 * Forks are read from the host on demand rather than held in memory -- one
 * CD-ROM's forks run to hundreds of megabytes, and a stack is read a few
 * hundred bytes at a time anyway.
 *
 * Read-only by design: the tool never writes to the user's media. Write and
 * Create report wrPermErr, which is a real File Manager answer and one that
 * titles are required to cope with.
 *
 * Field offsets follow Inside Macintosh IV (ParamBlockRec); no Apple code. */
#include "macrecomp/m68k.h"
#include "macrecomp/toolbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ParamBlockRec (IOParam / FileParam share the first 24 bytes) ---- */
#define PB_IORESULT     16
#define PB_IONAMEPTR    18
#define PB_IOVREFNUM    22
#define PB_IOREFNUM     24
#define PB_IOPERMSSN    27
#define PB_IOMISC       28
#define PB_IOBUFFER     32
#define PB_IOREQCOUNT   36
#define PB_IOACTCOUNT   40
#define PB_IOPOSMODE    44
#define PB_IOPOSOFFSET  46
/* FileParam, for GetFileInfo */
#define PB_IOFDIRINDEX  28
#define PB_IOFLATTRIB   30
#define PB_IOFLFNDRINFO 32
#define PB_IOFLNUM      48
#define PB_IOFLLGLEN    54
#define PB_IOFLPYLEN    58
#define PB_IOFLRLGLEN   64
#define PB_IOFLRPYLEN   68

/* ---- result codes (Inside Macintosh II) ---- */
#define NOERR         0
#define EOFERR      (-39)
#define FNFERR      (-43)
#define WRPERMERR   (-61)
#define PARAMERR    (-50)
#define RFNUMERR    (-51)

#define MAXFILES 512
#define MAXOPEN   64
#define NAMEMAX   64

typedef struct {
    char     name[NAMEMAX];       /* the file's own name, no path */
    char     type[5], creator[5];
    char     dpath[512], rpath[512];
    uint32_t dlen, rlen;
} FSFile;

static FSFile g_file[MAXFILES];
static int    g_nfile;

typedef struct { int used, idx, rsrc; uint32_t pos; } FSOpen;
static FSOpen g_open[MAXOPEN];    /* refNum is the index + 1: 0 is never a file */

static int trace(void){ return getenv("MRFILE") != 0; }

void fs_add(const char *name, const char *type, const char *creator,
            const char *dpath, uint32_t dlen, const char *rpath, uint32_t rlen){
    if(g_nfile >= MAXFILES) return;
    FSFile *f = &g_file[g_nfile++];
    snprintf(f->name, sizeof f->name, "%s", name ? name : "");
    snprintf(f->type, sizeof f->type, "%s", type ? type : "");
    snprintf(f->creator, sizeof f->creator, "%s", creator ? creator : "");
    snprintf(f->dpath, sizeof f->dpath, "%s", dpath ? dpath : "");
    snprintf(f->rpath, sizeof f->rpath, "%s", rpath ? rpath : "");
    f->dlen = dlen; f->rlen = rlen;
}

/* HFS is case-insensitive, and a name may arrive as a partial path
 * ("Disk:Folder:Home"), so compare only the last colon-separated element. */
static const char *leaf(const char *s){
    const char *c = strrchr(s, ':');
    return c ? c + 1 : s;
}
static int same_name(const char *a, const char *b){
    a = leaf(a); b = leaf(b);
    while(*a && *b){
        int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        int cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if(ca != cb) return 0;
        a++; b++;
    }
    return !*a && !*b;
}
static int find_file(const char *name){
    if(!name || !*name) return -1;
    for(int i = 0; i < g_nfile; i++) if(same_name(g_file[i].name, name)) return i;
    return -1;
}
/* A Pascal string out of guest memory. */
static void pstr(uint32_t p, char *out, int max){
    int n = p ? (int)m68k_r8(p) : 0;
    if(n > max - 1) n = max - 1;
    for(int i = 0; i < n; i++) out[i] = (char)m68k_r8(p + 1 + i);
    out[n] = 0;
}
static void fail(uint32_t pb, int err){
    m68k_w16(pb + PB_IORESULT, (uint16_t)err);
    M.d[0] = (uint32_t)err;
}
static uint32_t fork_len(const FSFile *f, int rsrc){ return rsrc ? f->rlen : f->dlen; }
static const char *fork_path(const FSFile *f, int rsrc){ return rsrc ? f->rpath : f->dpath; }

static int do_open(uint32_t pb, int rsrc){
    char name[NAMEMAX];
    pstr(m68k_r32(pb + PB_IONAMEPTR), name, sizeof name);
    int idx = find_file(name);
    if(trace()) fprintf(stderr, "[File] Open%s pb=%08x a5=%08x '%s' vRef=%d perm=%d -> %s\n",
                        rsrc ? "RF" : "", pb, M.a[5], name, (int16_t)m68k_r16(pb + PB_IOVREFNUM),
                        (int)m68k_r8(pb + PB_IOPERMSSN),
                        idx < 0 ? "fnfErr" : g_file[idx].name);
    if(idx < 0){ fail(pb, FNFERR); return FNFERR; }
    for(int i = 0; i < MAXOPEN; i++){
        if(g_open[i].used) continue;
        g_open[i].used = 1; g_open[i].idx = idx; g_open[i].rsrc = rsrc; g_open[i].pos = 0;
        m68k_w16(pb + PB_IOREFNUM, (uint16_t)(i + 1));
        fail(pb, NOERR); return NOERR;
    }
    fail(pb, PARAMERR); return PARAMERR;
}

static FSOpen *slot(uint32_t pb){
    int rn = (int16_t)m68k_r16(pb + PB_IOREFNUM);
    if(rn < 1 || rn > MAXOPEN || !g_open[rn - 1].used) return 0;
    return &g_open[rn - 1];
}

static int do_read(uint32_t pb){
    FSOpen *o = slot(pb);
    if(!o){ fail(pb, RFNUMERR); return RFNUMERR; }
    const FSFile *f = &g_file[o->idx];
    uint32_t len = fork_len(f, o->rsrc);
    uint32_t req = m68k_r32(pb + PB_IOREQCOUNT);
    uint32_t buf = m68k_r32(pb + PB_IOBUFFER);
    int      mode = (int16_t)m68k_r16(pb + PB_IOPOSMODE) & 3;
    int32_t  off = (int32_t)m68k_r32(pb + PB_IOPOSOFFSET);

    uint32_t pos = o->pos;                       /* 0 = fsAtMark, 3 = fsFromMark */
    if(mode == 1) pos = (uint32_t)(off < 0 ? 0 : off);          /* fsFromStart */
    else if(mode == 2) pos = (uint32_t)((int32_t)len + off);    /* fsFromLEOF */
    else if(mode == 3) pos = (uint32_t)((int32_t)o->pos + off);

    if(pos > len) pos = len;
    uint32_t n = req;
    if(pos + n > len) n = len - pos;

    if(n){
        FILE *fp = fopen(fork_path(f, o->rsrc), "rb");
        if(!fp){ m68k_w32(pb + PB_IOACTCOUNT, 0); fail(pb, FNFERR); return FNFERR; }
        static uint8_t tmp[65536];
        uint32_t done = 0;
        fseek(fp, (long)pos, SEEK_SET);
        while(done < n){
            uint32_t chunk = n - done;
            if(chunk > sizeof tmp) chunk = sizeof tmp;
            size_t got = fread(tmp, 1, chunk, fp);
            if(!got) break;
            for(size_t i = 0; i < got; i++) m68k_w8(buf + done + i, tmp[i]);
            done += (uint32_t)got;
        }
        fclose(fp);
        n = done;
    }
    o->pos = pos + n;
    m68k_w32(pb + PB_IOACTCOUNT, n);
    if(trace()) fprintf(stderr, "[File] Read %s%s req=%u got=%u pos=%u/%u buf=%08x%s\n",
                        f->name, o->rsrc ? " (rsrc)" : "", req, n, o->pos, len, buf,
                        (buf + n > M.memsize) ? "  !! buffer outside guest memory" : "");
    /* Short of the request means end of file, and the count still stands. */
    int err = (n < req) ? EOFERR : NOERR;
    fail(pb, err);
    return err;
}

static void four(uint32_t p, const char *s){        /* a 4-char type/creator */
    for(int i = 0; i < 4; i++) m68k_w8(p + i, s[i] ? (uint8_t)s[i] : ' ');
}

static int do_getfileinfo(uint32_t pb){
    char name[NAMEMAX];
    int di = (int16_t)m68k_r16(pb + PB_IOFDIRINDEX);
    int idx;
    if(di > 0){                                     /* index into the directory */
        idx = (di <= g_nfile) ? di - 1 : -1;
        if(idx >= 0){
            uint32_t np = m68k_r32(pb + PB_IONAMEPTR);
            if(np){ const char *n = g_file[idx].name; int l = (int)strlen(n);
                    if(l > 31) l = 31; m68k_w8(np, (uint8_t)l);
                    for(int i = 0; i < l; i++) m68k_w8(np + 1 + i, (uint8_t)n[i]); }
        }
    } else {
        pstr(m68k_r32(pb + PB_IONAMEPTR), name, sizeof name);
        idx = find_file(name);
    }
    if(trace()){
        char shown[NAMEMAX]; pstr(m68k_r32(pb + PB_IONAMEPTR), shown, sizeof shown);
        fprintf(stderr, "[File] GetFileInfo name='%s' dirIndex=%d vRef=%d dirID=%u -> %s\n",
                shown, di, (int16_t)m68k_r16(pb + PB_IOVREFNUM),
                m68k_r32(pb + PB_IOFLNUM), idx < 0 ? "fnfErr" : g_file[idx].name);
    }
    if(idx < 0){ fail(pb, FNFERR); return FNFERR; }
    const FSFile *f = &g_file[idx];
    m68k_w8 (pb + PB_IOFLATTRIB, 0);
    four(pb + PB_IOFLFNDRINFO,     f->type);
    four(pb + PB_IOFLFNDRINFO + 4, f->creator);
    m68k_w32(pb + PB_IOFLNUM,   (uint32_t)(idx + 2));   /* a stable non-zero file number */
    m68k_w32(pb + PB_IOFLLGLEN, f->dlen);
    m68k_w32(pb + PB_IOFLPYLEN, (f->dlen + 511) & ~511u);
    m68k_w32(pb + PB_IOFLRLGLEN, f->rlen);
    m68k_w32(pb + PB_IOFLRPYLEN, (f->rlen + 511) & ~511u);
    fail(pb, NOERR);
    return NOERR;
}

/* ---- one volume, one directory ----
 * The media this serves is a flat list of files, so the catalogue model is the
 * smallest one that is still truthful: a single volume (vRefNum -1) whose root
 * directory is the standard HFS root (dirID 2) and holds every file. A title
 * that walks the catalogue by index, or asks what directory it is in, gets a
 * consistent answer rather than an error it has no path around.
 * ponytail: no subdirectories. The disc has none; add a parent-ID column if a
 * title's media does. */
#define VREFNUM   (-1)
#define ROOT_DIR    2
#define PB_IODIRID   48     /* ioDirID / ioFlNum share this slot */
#define PB_IOWDVREFNUM 30   /* WDPBRec only; distinct from ioVRefNum at 22 */
#define PB_IODRNMFLS 52     /* files in a directory (ioDrNmFls) */
#define PB_IOPARID  100     /* ioDrParID / ioFlParID: the enclosing directory */
#define ROOT_PARENT   1     /* the root's parent, by HFS convention */
#define ATTRIB_DIR 0x10

static void put_pstr(uint32_t p, const char *s){
    if(!p) return;
    int l = (int)strlen(s); if(l > 31) l = 31;
    m68k_w8(p, (uint8_t)l);
    for(int i = 0; i < l; i++) m68k_w8(p + 1 + i, (uint8_t)s[i]);
}

/* PBGetCatInfo. ioFDirIndex selects the question:
 *   > 0  the n'th entry of the directory
 *   = 0  the file named by ioNamePtr
 *   < 0  the directory named by ioDirID (name is an output here) */
static int do_getcatinfo(uint32_t pb){
    int di = (int16_t)m68k_r16(pb + PB_IOFDIRINDEX);
    if(di < 0){
        uint32_t dir = m68k_r32(pb + PB_IODIRID);
        if(dir != ROOT_DIR && dir != 0){
            if(trace()) fprintf(stderr, "[File] GetCatInfo dirID=%u -> fnfErr\n", dir);
            fail(pb, FNFERR); return FNFERR;
        }
        put_pstr(m68k_r32(pb + PB_IONAMEPTR), "Untitled");
        m68k_w16(pb + PB_IOVREFNUM, (uint16_t)VREFNUM);
        m68k_w8 (pb + PB_IOFLATTRIB, ATTRIB_DIR);
        m68k_w32(pb + PB_IODIRID, ROOT_DIR);
        m68k_w16(pb + PB_IODRNMFLS, (uint16_t)g_nfile);
        /* The parent id is what stops a caller walking up the tree. HyperCard
         * climbs from wherever it is towards the root, and without this it asks
         * for the same directory forever -- 712,029 times in one run before
         * this line existed. */
        m68k_w32(pb + PB_IOPARID, ROOT_PARENT);
        if(trace()) fprintf(stderr, "[File] GetCatInfo root -> dirID %d, %d files\n",
                            ROOT_DIR, g_nfile);
        fail(pb, NOERR); return NOERR;
    }
    int r = do_getfileinfo(pb);
    if(r == NOERR){                      /* a file, in the one directory there is */
        m68k_w16(pb + PB_IOVREFNUM, (uint16_t)VREFNUM);
        m68k_w8 (pb + PB_IOFLATTRIB, 0);
        m68k_w32(pb + PB_IOPARID, ROOT_DIR);
    }
    return r;
}

/* PBGetFCBInfo: what is open on this refNum. HyperCard asks immediately after
 * opening a stack, and a refusal there is as good as never having opened it.
 * FCBPBRec fields (Inside Macintosh IV). */
#define PB_IOFCBINDX   28
#define PB_IOFCBFLNM   32
#define PB_IOFCBFLAGS  36
#define PB_IOFCBEOF    40
#define PB_IOFCBPLEN   44
#define PB_IOFCBCRPS   48
#define PB_IOFCBVREF   52
#define PB_IOFCBPARID  58

static int do_getfcbinfo(uint32_t pb){
    int indx = (int16_t)m68k_r16(pb + PB_IOFCBINDX);
    FSOpen *o = 0;
    if(indx <= 0) o = slot(pb);                 /* by refNum */
    else {                                       /* the n'th open file */
        int n = 0;
        for(int i = 0; i < MAXOPEN; i++)
            if(g_open[i].used && ++n == indx){
                o = &g_open[i];
                m68k_w16(pb + PB_IOREFNUM, (uint16_t)(i + 1));
                break; }
    }
    if(!o){ fail(pb, RFNUMERR); return RFNUMERR; }
    const FSFile *f = &g_file[o->idx];
    uint32_t len = fork_len(f, o->rsrc);
    put_pstr(m68k_r32(pb + PB_IONAMEPTR), f->name);
    m68k_w32(pb + PB_IOFCBFLNM,  (uint32_t)(o->idx + 2));
    m68k_w16(pb + PB_IOFCBFLAGS, (uint16_t)(o->rsrc ? 0x0200 : 0));  /* resource fork */
    m68k_w32(pb + PB_IOFCBEOF,   len);
    m68k_w32(pb + PB_IOFCBPLEN,  (len + 511) & ~511u);
    m68k_w32(pb + PB_IOFCBCRPS,  o->pos);
    m68k_w16(pb + PB_IOFCBVREF,  (uint16_t)VREFNUM);
    m68k_w32(pb + PB_IOFCBPARID, ROOT_DIR);
    if(trace()) fprintf(stderr, "[File] GetFCBInfo %s%s eof=%u pos=%u\n",
                        f->name, o->rsrc ? " (rsrc)" : "", len, o->pos);
    fail(pb, NOERR);
    return NOERR;
}

/* Returns 1 if it handled the trap. */
int fs_trap(uint16_t w){
    uint32_t pb = M.a[0];
    switch(w){
    case 0xA000: /*Open*/   case 0xA200: /*HOpen*/   do_open(pb, 0); return 1;
    case 0xA00A: /*OpenRF*/ case 0xA20A: /*HOpenRF*/ do_open(pb, 1); return 1;
    case 0xA002: /*Read*/   do_read(pb); return 1;
    case 0xA00C: /*GetFileInfo*/ case 0xA20C: /*HGetFileInfo*/ do_getfileinfo(pb); return 1;

    case 0xA001: { /*Close*/
        FSOpen *o = slot(pb);
        if(o) o->used = 0;
        fail(pb, o ? NOERR : RFNUMERR); return 1; }

    case 0xA011: { /*GetEOF*/
        FSOpen *o = slot(pb);
        if(!o){ fail(pb, RFNUMERR); return 1; }
        m68k_w32(pb + PB_IOMISC, fork_len(&g_file[o->idx], o->rsrc));
        fail(pb, NOERR); return 1; }

    case 0xA018: { /*GetFPos*/
        FSOpen *o = slot(pb);
        if(!o){ fail(pb, RFNUMERR); return 1; }
        m68k_w32(pb + PB_IOPOSOFFSET, o->pos);
        m68k_w32(pb + PB_IOREQCOUNT, 0); m68k_w32(pb + PB_IOACTCOUNT, 0);
        fail(pb, NOERR); return 1; }

    case 0xA044: { /*SetFPos*/
        FSOpen *o = slot(pb);
        if(!o){ fail(pb, RFNUMERR); return 1; }
        uint32_t len = fork_len(&g_file[o->idx], o->rsrc);
        int mode = (int16_t)m68k_r16(pb + PB_IOPOSMODE) & 3;
        int32_t off = (int32_t)m68k_r32(pb + PB_IOPOSOFFSET);
        int32_t pos = (int32_t)o->pos;
        if(mode == 1) pos = off;
        else if(mode == 2) pos = (int32_t)len + off;
        else if(mode == 3) pos = (int32_t)o->pos + off;
        int err = NOERR;
        if(pos < 0){ pos = 0; err = PARAMERR; }
        if(pos > (int32_t)len){ pos = (int32_t)len; err = EOFERR; }
        o->pos = (uint32_t)pos;
        m68k_w32(pb + PB_IOPOSOFFSET, o->pos);
        fail(pb, err); return 1; }

    /* Read-only media. These are real answers, not silence: a title that is
     * told it cannot write can say so, where one told "fine" loses data. */
    case 0xA003: /*Write*/ case 0xA008: /*Create*/ case 0xA009: /*Delete*/
    case 0xA00D: /*SetFileInfo*/ case 0xA012: /*SetEOF*/ case 0xA010: /*Allocate*/
        fail(pb, WRPERMERR); return 1;

    case 0xA013: /*FlushVol*/ case 0xA035: /*OffLine*/ case 0xA017: /*Eject*/
        fail(pb, NOERR); return 1;

    case 0xA014: /*GetVol*/ {
        uint32_t np = m68k_r32(pb + PB_IONAMEPTR);
        if(np){ const char *v = "Untitled"; int l = (int)strlen(v);
                m68k_w8(np, (uint8_t)l);
                for(int i = 0; i < l; i++) m68k_w8(np + 1 + i, (uint8_t)v[i]); }
        m68k_w16(pb + PB_IOVREFNUM, (uint16_t)(-1));
        fail(pb, NOERR); return 1; }
    case 0xA015: /*SetVol*/ fail(pb, NOERR); return 1;

    default: return 0;
    }
}

/* FSDispatch / HFSDispatch: one trap, many calls, selected by D0's low word.
 * The ROADMAP's warning applies -- a wrong stub is worse than none -- so this
 * answers only the selectors it actually implements and reports the rest by
 * number, rather than returning noErr and leaving the caller to act on a
 * parameter block nobody filled in. */
int fs_dispatch(uint16_t w){
    uint32_t pb = M.a[0];
    int sel = (int16_t)(M.d[0] & 0xFFFF);
    if(trace()) fprintf(stderr, "[File] %s selector %d\n",
                        w == 0xA260 ? "HFSDispatch" : "FSDispatch", sel);
    switch(sel){
    case 0x0009: /*PBGetCatInfo*/ do_getcatinfo(pb); return 1;
    case 0x0007: /*PBGetWDInfo*/
    case 0x0001: /*PBOpenWD*/
        /* Working directories collapse onto the one real directory: the volume
         * reference and the root are the only answer there is.
         *
         * PBGetWDInfo answers in WDPBRec's own fields, not the ones a plain
         * ParamBlockRec uses: ioWDProcID(26), ioWDVRefNum(30) and
         * ioWDDirID(48). Setting only ioVRefNum leaves a caller that asked
         * "which volume is this working directory on?" reading zero. */
        m68k_w16(pb + PB_IOVREFNUM, (uint16_t)VREFNUM);
        m68k_w16(pb + PB_IOWDVREFNUM, (uint16_t)VREFNUM);
        m68k_w32(pb + PB_IODIRID, ROOT_DIR);
        if(m68k_r32(pb + PB_IONAMEPTR)) put_pstr(m68k_r32(pb + PB_IONAMEPTR), "Untitled");
        fail(pb, NOERR); return 1;
    case 0x0002: /*PBCloseWD*/
        fail(pb, NOERR); return 1;
    case 0x0008: /*PBGetFCBInfo*/ do_getfcbinfo(pb); return 1;
    default:
        fail(pb, PARAMERR);        /* say "I did not do this" rather than "fine" */
        return 1;
    }
}

/* ---- opening a document's resource fork -----------------------------------
 *
 * A stack carries its own resources, and HyperCard opens them with OpenRFPerm
 * before it reads a single card. The fork is a self-contained little database,
 * so it is parsed here once and handed to the Resource Manager rather than
 * being read a piece at a time through the File Manager.
 *
 * Layout (Inside Macintosh I-128): a 16-byte header gives the offsets of the
 * data area and the map; the map holds a type list, each type naming a
 * reference list, and each reference giving an id and an offset into the data
 * area, where the resource is preceded by its 4-byte length. */
static uint32_t rd32(const uint8_t *b, uint32_t o){
    return ((uint32_t)b[o]<<24)|((uint32_t)b[o+1]<<16)|((uint32_t)b[o+2]<<8)|b[o+3];
}
static uint32_t rd16(const uint8_t *b, uint32_t o){ return ((uint32_t)b[o]<<8)|b[o+1]; }

static void parse_resfork(int refnum, const uint8_t *b, uint32_t n){
    if(n < 16) return;
    uint32_t dataOff = rd32(b,0), mapOff = rd32(b,4);
    uint32_t dataLen = rd32(b,8), mapLen = rd32(b,12);
    if(mapOff + 30 > n || mapOff + mapLen > n || dataOff > n) return;

    uint32_t typeListOff = mapOff + rd16(b, mapOff + 24);
    if(typeListOff + 2 > n) return;
    int nTypes = (int)rd16(b, typeListOff) + 1;
    int added = 0;

    for(int t = 0; t < nTypes; t++){
        uint32_t te = typeListOff + 2 + (uint32_t)t * 8;
        if(te + 8 > n) break;
        char type[5];
        for(int k = 0; k < 4; k++) type[k] = (char)b[te + k];
        type[4] = 0;
        int nRefs = (int)rd16(b, te + 4) + 1;
        uint32_t refList = typeListOff + rd16(b, te + 6);
        for(int r = 0; r < nRefs; r++){
            uint32_t re = refList + (uint32_t)r * 12;
            if(re + 12 > n) break;
            int id = (int)(int16_t)rd16(b, re);
            uint32_t off = rd32(b, re + 4) & 0x00FFFFFFu;   /* attrs in the top byte */
            uint32_t d = dataOff + off;
            if(d + 4 > n) continue;
            uint32_t len = rd32(b, d);
            if(d + 4 + len > n || len > dataLen) continue;
            res_add_file(refnum, type, id, b + d + 4, (int)len);
            added++;
        }
    }
    if(trace()) fprintf(stderr, "[File] resource fork: %d resources in %d type(s)\n",
                        added, nTypes);
}

/* Open a file's resource fork by name and make it the current resource file.
 * Returns a refNum, or -1 if there is no such file. */
int fs_open_resfork(uint32_t namePtr){
    char name[NAMEMAX];
    pstr(namePtr, name, sizeof name);
    int idx = find_file(name);
    if(idx < 0){ if(trace()) fprintf(stderr, "[File] OpenRF '%s' -> fnfErr\n", name);
                 M.d[0] = (uint32_t)FNFERR; return -1; }
    FSFile *f = &g_file[idx];

    /* Each fork is parsed once; re-opening the same one just selects it. */
    static int done[MAXFILES];
    int refnum = idx + 2;                     /* 1 is the application */
    if(!done[idx]){
        done[idx] = 1;
        if(f->rlen){
            FILE *fp = fopen(f->rpath, "rb");
            if(fp){
                uint8_t *buf = malloc(f->rlen);   /* kept: the map points into it */
                if(buf && fread(buf, 1, f->rlen, fp) == f->rlen)
                    parse_resfork(refnum, buf, f->rlen);
                fclose(fp);
            }
        }
    }
    res_use_file(refnum);
    if(trace()) fprintf(stderr, "[File] OpenRF '%s' -> refNum %d (%u bytes)\n",
                        f->name, refnum, f->rlen);
    M.d[0] = NOERR;
    return refnum;
}

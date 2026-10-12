/* CRI ADX for the PC port.
 *
 * On a PS2, CRI's library streams ADX sound from the disc on its own threads and keeps a file
 * system of "partitions": AFS archives whose table of contents is read once, so files inside
 * can be opened by number. Here the partitions are real: they read the AFS archives from the
 * user's disc image (iso9660.c). Sound playback is still to do: the ADXT calls keep their
 * settings and report "stopped".
 */
#include "cri.h"
#include "iso9660.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- file system set-up: nothing to do on PC, the disc image is already open ---- */

void ADXPS2_SetupThrd(void *tprm) {}
void ADXPS2_SetupHstFs(void *prm) { log_info("adx: host file system (not used on PC)"); }
void ADXPS2_SetupDvdFs(void *prm) { log_info("adx: DVD file system on the disc image"); }
/* The disc's file list (0flist.dir) speeds up file lookups on a PS2; the PC reads the ISO directory. */
void ADXPS2_LoadFileListHst(void *prm) {}
s32 ADXPS2_LoadFileListDvd(void *prm) { return 0; }
void func_0012C1E0(s32 mode) {}
void func_0012B360(void (*cb)(void), void *obj) {}

/* ---- partitions ---- */

#define MAX_PT 256

typedef struct {
    int loaded;
    IsoFile file;          /* the disc file the partition lives in */
    uint32_t base;         /* offset of its AFS archive inside that file */
    uint32_t count;
    uint32_t *offset;      /* per entry, relative to base */
    uint32_t *size;
} Partition;

static Partition g_pt[MAX_PT];

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Read an AFS table of contents at `base` in `file` into partition `ptid`. */
static int load_afs(s32 ptid, const IsoFile *file, uint32_t base, const char *what) {
    uint8_t head[8];
    Partition *pt;

    if (ptid < 0 || ptid >= MAX_PT) {
        log_error("adx: partition id %d out of range", ptid);
        return -1;
    }
    pt = &g_pt[ptid];
    free(pt->offset);
    free(pt->size);
    memset(pt, 0, sizeof(*pt));
    if (iso_read(file, base, head, 8) != 8 || memcmp(head, "AFS", 4) != 0) {
        log_error("adx: %s is not an AFS archive", what);
        return -1;
    }
    pt->count = le32(head + 4);
    uint8_t *toc = malloc(pt->count * 8u + 1);
    pt->offset = malloc(pt->count * 4u + 1);
    pt->size = malloc(pt->count * 4u + 1);
    if (!toc || !pt->offset || !pt->size || iso_read(file, base + 8, toc, pt->count * 8) != (int64_t)pt->count * 8) {
        log_error("adx: cannot read the table of contents of %s", what);
        free(toc);
        return -1;
    }
    for (uint32_t i = 0; i < pt->count; i++) {
        pt->offset[i] = le32(toc + 8 * i);
        pt->size[i] = le32(toc + 8 * i + 4);
    }
    free(toc);
    pt->file = *file;
    pt->base = base;
    pt->loaded = 1;
    log_info("adx: partition %d: %s, %u files", ptid, what, pt->count);
    return 0;
}

void ADXF_LoadPartitionNw(s32 ptid, const char *fname, void *dir, void *ptinfo) {
    IsoFile f;
    if (iso_find(fname, &f) != 0) {
        log_error("adx: partition %d: %s isn't on the disc", ptid, fname);
        return;
    }
    load_afs(ptid, &f, 0, fname);
}

void ADXF_LoadPtFromAfsNw(s32 ptid, s32 afs_ptid, s32 fid, void *ptinfo) {
    char what[64];
    Partition *afs = afs_ptid >= 0 && afs_ptid < MAX_PT ? &g_pt[afs_ptid] : NULL;

    if (!afs || !afs->loaded || fid < 0 || (uint32_t)fid >= afs->count) {
        log_error("adx: partition %d: file %d of partition %d doesn't exist", ptid, fid, afs_ptid);
        return;
    }
    snprintf(what, sizeof what, "file %d of partition %d", fid, afs_ptid);
    load_afs(ptid, &afs->file, afs->base + afs->offset[fid], what);
}

/* ADXF_STAT_READEND: partitions load at once on PC */
s32 ADXF_GetPtStat(s32 ptid) { return 3; }

/* For the PC's own file access: where file `fid` of partition `ptid` is on the disc. */
int adx_partition_file(s32 ptid, s32 fid, IsoFile *disc_file, uint32_t *offset, uint32_t *size) {
    Partition *pt = ptid >= 0 && ptid < MAX_PT ? &g_pt[ptid] : NULL;
    if (!pt || !pt->loaded || fid < 0 || (uint32_t)fid >= pt->count) return -1;
    *disc_file = pt->file;
    *offset = pt->base + pt->offset[fid];
    *size = pt->size[fid];
    return 0;
}

int adx_partition_file(s32 ptid, s32 fid, IsoFile *disc_file, uint32_t *offset, uint32_t *size);

/* ---- ADXF: reading whole files (the game's overlays) ---- */

typedef struct {
    IsoFile file;
    uint32_t base;          /* where the file starts in the disc file (AFS archives hold many) */
    uint32_t size;
    s32 stat;
} AdxfHandle;

#define ADXF_STAT_READEND 3

ADXF ADXF_Open(const char *fname, void *atr) {
    AdxfHandle *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    if (iso_find(fname, &h->file) != 0) {
        log_error("adx: %s isn't on the disc", fname);
        free(h);
        return NULL;             /* the game retries until it opens */
    }
    h->size = h->file.size;
    return h;
}

/* File `fid` of partition `ptid` */
ADXF ADXF_OpenAfs(s32 ptid, s32 fid) {
    AdxfHandle *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    if (adx_partition_file(ptid, fid, &h->file, &h->base, &h->size) != 0) {
        log_error("adx: file %d of partition %d doesn't exist", fid, ptid);
        free(h);
        return NULL;
    }
    return h;
}

void ADXF_Close(ADXF adxf) { free(adxf); }

s32 ADXF_GetFsizeSct(ADXF adxf) {
    AdxfHandle *h = adxf;
    return h ? (s32)((h->size + 2047) / 2048) : 0;
}

/* Reads at once; the game then waits for ADXF_STAT_READEND. */
s32 ADXF_ReadNw(ADXF adxf, s32 nsct, void *buf) {
    AdxfHandle *h = adxf;
    uint32_t want;
    int64_t n;
    if (!h) return -1;
    want = (uint32_t)nsct * 2048u;
    if (want > h->size) want = h->size;
    n = iso_read(&h->file, h->base, buf, want);
    if (n < 0) {
        log_error("adx: read error");
        return -1;
    }
    h->stat = ADXF_STAT_READEND;
    if (getenv("OUTBREAK_TRACE_FILES")) {
        log_info("adx: read %u bytes at offset 0x%llx of the disc file", (unsigned)want, (unsigned long long)h->base);
    }
    return (s32)((n + 2047) / 2048);
}

s32 ADXF_GetStat(ADXF adxf) { return adxf ? ((AdxfHandle *)adxf)->stat : 0; }

/* ---- ADXT: sound streams (playback to do) ---- */

typedef struct {
    s32 maxnch;
    s32 stat;
    s32 vol;
    s32 svrfreq;
    s32 reloadSct;
    s32 paused;
    s32 unk6D;
} AdxtHandle;

void ADXT_Init(void) { log_info("adx: ADXT_Init (sound playback is not implemented yet)"); }

ADXT ADXT_Create(s32 maxnch, void *work, s32 worksize) {
    AdxtHandle *h = calloc(1, sizeof(*h));
    if (h) h->maxnch = maxnch;
    return h;
}

void ADXT_Stop(ADXT adxt) { if (adxt) ((AdxtHandle *)adxt)->stat = 0; }
s32 ADXT_GetStat(ADXT adxt) { return adxt ? ((AdxtHandle *)adxt)->stat : 0; }   /* 0: ADXT_STAT_STOP */
void ADXT_SetOutVol(ADXT adxt, s32 vol) { if (adxt) ((AdxtHandle *)adxt)->vol = vol; }
void ADXT_SetSvrFreq(ADXT adxt, s32 freq) { if (adxt) ((AdxtHandle *)adxt)->svrfreq = freq; }
void ADXT_SetReloadSct(ADXT adxt, s32 sct) { if (adxt) ((AdxtHandle *)adxt)->reloadSct = sct; }
void ADXT_Pause(ADXT adxt, s32 on) { if (adxt) ((AdxtHandle *)adxt)->paused = on; }
void func_00132908(ADXT adxt, s32 n) { if (adxt) ((AdxtHandle *)adxt)->unk6D = n; }
void func_00133168(void) {}

/* ---- the CRI v-blank wait the frame loop uses while the middleware runs ---- */

s32 sceGsSyncV(s32 mode);

/* On a PS2: lock the Sofdec server, wait for the v-blank with ADX's server running, unlock.
   Here the ADX server has nothing to do yet, so it is the v-blank wait. */
void func_0015F790(void) { sceGsSyncV(0); }
/* CRI: hold/release the disc for the game's own reads (nothing to share on PC) */
void func_0012BAB8(void) {}
void func_0012BAD0(void) {}
/* ADX server, run once a frame by the game (sound playback is still to do) */
void func_0012BC38(void) {}

/* ---- Sofdec (movies): to do ---- */

void mwPlyInitSfdFx(void *prm) { log_info("sofdec: movie player not implemented yet"); }

/* CRI Sofdec, the PS2 movie player, for the PC port.
 *
 * The game plays its movies (.sfd: MPEG-1 video with ADX sound, in an MPEG system stream) from the
 * AFS archives through a handful of mwPly calls: create a player for a size, queue one or more files,
 * then once a frame ask for the current picture and upload it to the GS. Here the video is decoded
 * with pl_mpeg (third_party/pl_mpeg, MIT) straight from the disc. The picture is handed over the way
 * Sofdec does on the PS2: RGBA, in 16x16 macroblocks of 1 KB stored column by column, in the work
 * area the game gave the player, so the game's own upload code sends it to the GS unchanged.
 *
 * Movie sound (ADX) is still to do: the audio stream is skipped.
 */
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "cri.h"
#include "iso9660.h"
#include "log.h"

#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "../third_party/pl_mpeg/pl_mpeg.h"

int adx_partition_file(s32 ptid, s32 fid, IsoFile *disc_file, uint32_t *offset, uint32_t *size);
extern uint32_t pc_vblank_count;

enum { STAT_STOP, STAT_PREP, STAT_PLAYING, STAT_END, STAT_ERROR };
#define MAX_ENTRIES 32
#define VBLANK_HZ 59.94

typedef struct {
    MwPly base;                 /* what the game sees: the vtable pointer */
    s32 stat;
    struct { s32 ptid, fid; } entry[MAX_ENTRIES];
    int nentry, cur;
    int loop;
    /* the file being played */
    IsoFile file;
    uint32_t file_base, file_size, file_pos;
    plm_t *plm;
    double fps;
    /* timing: frames shown so far (all files), against v-blanks since the start */
    uint32_t start_vblank;
    int shown;
    /* pictures: two in the work area, used in turn */
    uint8_t *pic[2];
    uint32_t pic_size;
    int which;
    uint8_t *rgba;              /* the decoded frame before it's cut into macroblocks */
    int width, height;          /* rounded up to whole macroblocks */
} Player;

/* ---- reading the movie from the disc ---- */

static void load_cb(plm_buffer_t *b, void *user) {
    Player *p = user;
    if (b->discard_read_bytes) plm_buffer_discard_read_bytes(b);
    size_t room = b->capacity - b->length;
    uint32_t left = p->file_size - p->file_pos;
    if (room > left) room = left;
    int64_t n = room ? iso_read(&p->file, p->file_base + p->file_pos, b->bytes + b->length, (uint32_t)room) : 0;
    if (n <= 0) {
        b->has_ended = TRUE;
        return;
    }
    p->file_pos += (uint32_t)n;
    b->length += (size_t)n;
}

static void seek_cb(plm_buffer_t *b, size_t offset, void *user) {
    Player *p = user;
    p->file_pos = offset < p->file_size ? (uint32_t)offset : p->file_size;
}

static size_t tell_cb(plm_buffer_t *b, void *user) {
    return ((Player *)user)->file_pos;
}

static void close_file(Player *p) {
    if (p->plm) plm_destroy(p->plm);
    p->plm = NULL;
}

/* Opens queue entry i; 0 if it's a movie we can play. */
static int open_entry(Player *p, int i) {
    close_file(p);
    if (adx_partition_file(p->entry[i].ptid, p->entry[i].fid, &p->file, &p->file_base, &p->file_size) != 0) {
        log_error("sofdec: file %d of partition %d doesn't exist", p->entry[i].fid, p->entry[i].ptid);
        return -1;
    }
    p->file_pos = 0;
    plm_buffer_t *buf = plm_buffer_create_with_callbacks(load_cb, seek_cb, tell_cb, p->file_size, p);
    p->plm = plm_create_with_buffer(buf, 1);
    plm_set_audio_enabled(p->plm, 0);          /* ADX, not MPEG audio: still to do */
    plm_set_loop(p->plm, 0);
    if (!plm_probe(p->plm, 512 * 1024) || plm_get_width(p->plm) <= 0) {
        log_error("sofdec: file %d of partition %d isn't a movie we can play (v-blank %u)", p->entry[i].fid, p->entry[i].ptid, pc_vblank_count);
        close_file(p);
        return -1;
    }
    int w = (plm_get_width(p->plm) + 15) & ~15, h = (plm_get_height(p->plm) + 15) & ~15;
    if ((uint32_t)(w * h * 4) > p->pic_size) {
        log_error("sofdec: a %dx%d movie doesn't fit the player's memory", w, h);
        close_file(p);
        return -1;
    }
    if (w != p->width || h != p->height) {
        free(p->rgba);
        p->rgba = malloc((size_t)w * h * 4);
        p->width = w;
        p->height = h;
    }
    p->fps = plm_get_framerate(p->plm) > 0 ? plm_get_framerate(p->plm) : 29.97;
    p->cur = i;
    log_info("sofdec: playing file %d of partition %d (%dx%d, %.2f fps)", p->entry[i].fid, p->entry[i].ptid,
             plm_get_width(p->plm), plm_get_height(p->plm), p->fps);
    return 0;
}

static void start(Player *p) {
    p->shown = 0;
    p->start_vblank = pc_vblank_count;
    p->stat = p->nentry && open_entry(p, 0) == 0 ? STAT_PLAYING : STAT_ERROR;
}

/* The next frame of the queue, or NULL at its end. */
static plm_frame_t *next_frame(Player *p) {
    for (int tries = 0; tries <= MAX_ENTRIES; tries++) {
        plm_frame_t *f = p->plm ? plm_decode_video(p->plm) : NULL;
        if (f) return f;
        int next = p->cur + 1;
        if (next >= p->nentry) {
            if (!p->loop) return NULL;
            next = 0;
        }
        if (open_entry(p, next) != 0) return NULL;
    }
    return NULL;
}

/* RGBA rows -> 16x16 macroblocks, column by column, alpha 0x80 (1.0 on the GS). */
static void to_macroblocks(Player *p, plm_frame_t *f, uint8_t *out) {
    int w = p->width, h = p->height;
    memset(p->rgba, 0, (size_t)w * h * 4);
    plm_frame_to_rgba(f, p->rgba, w * 4);
    for (int bx = 0; bx < w / 16; bx++)
        for (int by = 0; by < h / 16; by++)
            for (int y = 0; y < 16; y++) {
                const uint8_t *src = p->rgba + ((size_t)(by * 16 + y) * w + bx * 16) * 4;
                for (int x = 0; x < 16; x++, src += 4, out += 4) {
                    out[0] = src[0];
                    out[1] = src[1];
                    out[2] = src[2];
                    out[3] = 0x80;
                }
            }
}

/* ---- the player handle's methods ---- */

static void ply_stop(MwPly *ply) {
    Player *p = (Player *)ply;
    close_file(p);
    p->stat = STAT_STOP;
}

static void ply_destroy(MwPly *ply) {
    Player *p = (Player *)ply;
    close_file(p);
    free(p->rgba);
    free(p);
}

static s32 ply_get_stat(MwPly *ply) { return ((Player *)ply)->stat; }

static void ply_get_time(MwPly *ply, s32 *time, s32 *tunit) {
    Player *p = (Player *)ply;
    *time = p->shown * 100;
    *tunit = (s32)(p->fps * 100 + 0.5);
}

static MwPlyVtbl g_vtbl = {
    .destroy = ply_destroy,
    .stop = ply_stop,
    .getStat = ply_get_stat,
    .getTime = ply_get_time,
};

/* ---- the calls the game makes ---- */

void mwPlyInitSfdFx(void *prm) {}

/* Two pictures of the largest size the game asked for (480 lines at least: some movies have 480). */
static uint32_t pic_size(const MwsfdCrePrm *c) {
    uint32_t w = ((uint32_t)c->maxWidth + 15) & ~15u, h = ((uint32_t)c->maxHeight + 15) & ~15u;
    if (h < 480) h = 480;
    return w * h * 4;
}

u32 mwPlyCalcWorkCprmSfd(MwsfdCrePrm *cprm) { return 2 * pic_size(cprm) + 64; }

MwPly *mwPlyCreateSofdec(MwsfdCrePrm *cprm) {
    if (cprm->workSize < mwPlyCalcWorkCprmSfd(cprm)) {
        log_error("sofdec: the work area is too small");
        return NULL;
    }
    Player *p = calloc(1, sizeof *p);
    p->base.vtbl = &g_vtbl;
    p->pic_size = pic_size(cprm);
    p->pic[0] = (uint8_t *)(uintptr_t)((cprm->work + 63) & ~63u);
    p->pic[1] = p->pic[0] + p->pic_size;
    return &p->base;
}

void mwPlyEntryAfs(MwPly *ply, s32 ptid, s32 fid) {
    Player *p = (Player *)ply;
    if (p->nentry < MAX_ENTRIES) {
        p->entry[p->nentry].ptid = ptid;
        p->entry[p->nentry].fid = fid;
        p->nentry++;
    }
}

void mwPlySetLpFlg(MwPly *ply, s32 flag) { ((Player *)ply)->loop = flag != 0; }

void mwPlyStartSeamless(MwPly *ply) { start((Player *)ply); }

void mwPlyStartAfs(MwPly *ply, s32 ptid, s32 fid) {
    Player *p = (Player *)ply;
    p->nentry = 0;
    mwPlyEntryAfs(ply, ptid, fid);
    start(p);
}

void mwPlyGetCurFrm(MwPly *ply, MwsfdFrmObj *frm) {
    Player *p = (Player *)ply;
    memset(frm, 0, sizeof *frm);
    if (p->stat != STAT_PLAYING) return;
    /* the frame due now; frames we're behind on are decoded and dropped */
    int due = (int)((pc_vblank_count - p->start_vblank) * p->fps / VBLANK_HZ);
    if (p->shown > due) return;
    plm_frame_t *f = NULL;
    do {
        f = next_frame(p);
        if (!f) {
            p->stat = STAT_END;
            close_file(p);
            return;
        }
        p->shown++;
    } while (p->shown <= due && due - p->shown < 8);
    uint8_t *out = p->pic[p->which];
    p->which ^= 1;
    to_macroblocks(p, f, out);
    frm->bufadr = (u32)(uintptr_t)out;
    frm->frmid = p->shown;
    frm->width = p->width;
    frm->height = p->height;
    frm->frmno = p->shown - 1;
}

void mwPlyRelCurFrm(MwPly *ply, MwsfdFrmObj *frm) {}

void mwPlyGetPlyInf(MwPly *ply, MwsfdPlyInf *inf) { memset(inf, 0, sizeof *inf); }

/* No more entries will be queued: play to the end of the last one. */
void mwPlyReleaseSeamless(MwPly *ply) { ((Player *)ply)->loop = 0; }

void mwPlyStartAfsLp(MwPly *ply, s32 ptid, s32 fid) {
    Player *p = (Player *)ply;
    p->nentry = 0;
    mwPlyEntryAfs(ply, ptid, fid);
    p->loop = 1;
    start(p);
}

/* Memory cards (Sony's libmc2) for the PC port: the card in slot 1 is a folder on disk
   (saves/card1 by default, --saves DIR to change it). Each save directory on the card is a
   folder in it and each file a file, so saves can be copied around like any other files.

   The library's calls are asynchronous: the game starts a command, then polls
   sceMc2CheckAsync until it has finished. Here every command finishes at once, and the poll
   gets its result. Errors are 0x8101xxxx with an errno-like code in the low bits, which is
   all the game looks at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#define mkdir_(p) mkdir(p, 0755)
#endif
#include "common.h"
#include "mcjob.h"
#include "log.h"

#define ERR(code) ((s32)(0x81010000u | (code)))
#define E_NOENT   2
#define E_EXIST   0x11
#define E_NODEV   0x13
#define E_NOSPC   0x1C
#define E_BUSY    0x10
#define CARD_CLUSTERS 8000      /* an 8 MB card in 1 KB clusters */

static char g_root[1024] = "saves/card1";
static int g_sockets;
static int g_port[8];           /* socket -> card port (2 = slot 1) */
static char g_cwd[8][64];
static int g_pending, g_cmd, g_result;

void pc_mc2_set_dir(const char *dir) {
    snprintf(g_root, sizeof g_root, "%s/card1", dir);
}

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir_(tmp);
            *p = '/';
        }
    }
    mkdir_(tmp);
}

/* The card's file name -> the host path. Card names are "dir/file" or "/dir/file". */
static void host_path(s32 socket, const char *name, char *out, size_t n) {
    char rel[256];
    if (name[0] == '/') {
        snprintf(rel, sizeof rel, "%s", name + 1);
    } else if (g_cwd[socket & 7][0]) {
        snprintf(rel, sizeof rel, "%s/%s", g_cwd[socket & 7], name);
    } else {
        snprintf(rel, sizeof rel, "%s", name);
    }
    for (char *p = rel; *p; p++) {
        if (*p == '\\' || *p == ':') *p = '_';      /* keep names inside the card folder */
    }
    if (strstr(rel, "..")) rel[0] = 0;
    snprintf(out, n, "%s/%s", g_root, rel);
    size_t l = strlen(out);
    while (l > 0 && out[l - 1] == '/') out[--l] = 0;
}

static int card_present(s32 socket) {
    return socket >= 0 && socket < g_sockets && g_port[socket] == 2;
}

static s32 finish(int cmd, s32 result) {
    if (g_pending) return ERR(E_BUSY);
    g_pending = 1;
    g_cmd = cmd;
    g_result = result;
    return 0;
}

static long file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

/* Clusters used: every file rounded up to 1 KB, plus a cluster per directory entry pair. */
static int used_clusters(const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int used = 0, entries = 0;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        entries++;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) used += used_clusters(p, depth + 1) + 2;
        } else {
            used += (int)((st.st_size + 1023) / 1024);
        }
    }
    closedir(d);
    return used + (entries + 1) / 2;
}

s32 sceMc2Init(s32 mode) {
    mkdirs(g_root);
    log_info("memory card: slot 1 is %s", g_root);
    return 0;
}

s32 sceMc2CreateSocket(sceMc2SocketParam *param, void *buffer) {
    if (g_sockets >= 8) return -1;
    g_port[g_sockets] = param->port;
    g_cwd[g_sockets][0] = 0;
    return g_sockets++;
}

s32 sceMc2CheckAsync(s32 *cmd, s32 *result) {
    if (!g_pending) return -1;
    if (cmd) *cmd = g_cmd;
    if (result) *result = g_result;
    g_pending = 0;
    return 1;
}

s32 sceMc2WaitAsync(s32 *cmd, s32 *result) {
    return sceMc2CheckAsync(cmd, result);
}

s32 sceMc2GetInfoAsync(s32 socket, McInfo *info) {
    if (!card_present(socket)) return finish(2, ERR(E_NODEV));
    mkdirs(g_root);
    info->type = 2;
    info->formatted = 1;
    int free_ = CARD_CLUSTERS - used_clusters(g_root, 0);
    info->freeClusters = free_ < 0 ? 0 : free_;
    return finish(2, 0);
}

s32 sceMc2GetInfo(s32 socket, McInfo *info) {
    s32 r = sceMc2GetInfoAsync(socket, info);
    if (r < 0) return r;
    sceMc2CheckAsync(NULL, &r);
    return r;
}

static void fill_entry(const char *path, const char *name, McDirEntry *ent) {
    struct stat st;
    memset(ent, 0, sizeof *ent);
    if (stat(path, &st) != 0) return;
    time_t t = st.st_mtime;
    struct tm *tm = localtime(&t);
    if (tm) {
        /* sceCdCLOCK-like: 0, sec, min, hour, day, month, year (u16) */
        u8 c[8] = {0, (u8)tm->tm_sec, (u8)tm->tm_min, (u8)tm->tm_hour, (u8)tm->tm_mday, (u8)(tm->tm_mon + 1),
                   (u8)((tm->tm_year + 1900) & 0xFF), (u8)((tm->tm_year + 1900) >> 8)};
        memcpy(ent->created, c, 8);
        memcpy(ent->modified, c, 8);
    }
    ent->size = S_ISDIR(st.st_mode) ? 0 : (u32)st.st_size;
    ent->attr = S_ISDIR(st.st_mode) ? 0x8427 : 0x8497;
    snprintf(ent->name, sizeof ent->name, "%s", name);
}

s32 sceMc2SearchFileAsync(s32 socket, const char *name, McDirEntry *ent) {
    if (!card_present(socket)) return finish(0xE, ERR(E_NODEV));
    char p[1024];
    host_path(socket, name, p, sizeof p);
    struct stat st;
    if (stat(p, &st) != 0) return finish(0xE, ERR(E_NOENT));
    const char *base = strrchr(name, '/');
    fill_entry(p, base ? base + 1 : name, ent);
    return finish(0xE, 0);
}

s32 sceMc2ReadFileAsync(s32 socket, const char *name, void *buf, s32 offset, s32 size) {
    if (!card_present(socket)) return finish(5, ERR(E_NODEV));
    char p[1024];
    host_path(socket, name, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return finish(5, ERR(E_NOENT));
    fseek(f, offset, SEEK_SET);
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    log_info("memory card: read %s (%d bytes)", name, (int)n);
    return finish(5, (s32)n);
}

s32 sceMc2WriteFileAsync(s32 socket, const char *name, const void *buf, s32 offset, s32 size) {
    if (!card_present(socket)) return finish(6, ERR(E_NODEV));
    char p[1024];
    host_path(socket, name, p, sizeof p);
    FILE *f = fopen(p, "r+b");
    if (!f) f = fopen(p, "wb");
    if (!f) return finish(6, ERR(E_NOENT));
    fseek(f, offset, SEEK_SET);
    size_t n = fwrite(buf, 1, (size_t)size, f);
    fclose(f);
    log_info("memory card: wrote %s (%d bytes)", name, (int)n);
    return finish(6, n == (size_t)size ? (s32)n : ERR(E_NOSPC));
}

s32 sceMc2CreateFileAsync(s32 socket, const char *name) {
    if (!card_present(socket)) return finish(7, ERR(E_NODEV));
    char p[1024];
    host_path(socket, name, p, sizeof p);
    if (file_size(p) >= 0) return finish(7, ERR(E_EXIST));
    FILE *f = fopen(p, "wb");
    if (!f) return finish(7, ERR(E_NOENT));
    fclose(f);
    return finish(7, 0);
}

static int remove_tree(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *e;
        char q[1024];
        while (d && (e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(q, sizeof q, "%s/%s", p, e->d_name);
            remove_tree(q);
        }
        if (d) closedir(d);
        return rmdir(p);
    }
    return remove(p);
}

s32 sceMc2DeleteAsync(s32 socket, const char *name) {
    if (!card_present(socket)) return finish(8, ERR(E_NODEV));
    char p[1024];
    host_path(socket, name, p, sizeof p);
    return finish(8, remove_tree(p) == 0 ? 0 : ERR(E_NOENT));
}

s32 sceMc2RenameAsync(s32 socket, const char *from, const char *to) {
    if (!card_present(socket)) return finish(9, ERR(E_NODEV));
    char a[1024], b[1024];
    host_path(socket, from, a, sizeof a);
    /* the new name is in the same directory */
    snprintf(b, sizeof b, "%s", a);
    char *slash = strrchr(b, '/');
    if (slash) snprintf(slash + 1, sizeof b - (size_t)(slash + 1 - b), "%s", to);
    return finish(9, rename(a, b) == 0 ? 0 : ERR(E_NOENT));
}

/* "dir/*" style listing; the count of entries goes to *count and the result. */
s32 sceMc2GetDirAsync(s32 socket, const char *name, s32 offset, s32 max, McDirEntry *ents, s32 *count) {
    if (!card_present(socket)) return finish(0xA, ERR(E_NODEV));
    char p[1024], pat[256] = "";
    host_path(socket, name, p, sizeof p);
    char *slash = strrchr(p, '/');
    if (slash && strchr(slash, '*')) {
        snprintf(pat, sizeof pat, "%s", slash + 1);
        *slash = 0;
    }
    DIR *d = opendir(p);
    if (!d) {
        if (count) *count = 0;
        return finish(0xA, ERR(E_NOENT));
    }
    int n = 0, seen = 0;
    struct dirent *e;
    char q[1024];
    size_t plen = strcspn(pat, "*");
    while ((e = readdir(d)) != NULL && n < max) {
        if (!strcmp(e->d_name, "..") && !pat[0]) continue;
        if (pat[0] && strncmp(e->d_name, pat, plen) != 0) continue;
        if (seen++ < offset) continue;
        snprintf(q, sizeof q, "%s/%s", p, e->d_name);
        fill_entry(q, e->d_name, &ents[n++]);
    }
    closedir(d);
    if (count) *count = n;
    return finish(0xA, n);
}

s32 sceMc2MkdirAsync(s32 socket, const char *name) {
    if (!card_present(socket)) return finish(0xB, ERR(E_NODEV));
    char p[1024];
    host_path(socket, name, p, sizeof p);
    struct stat st;
    if (stat(p, &st) == 0) return finish(0xB, ERR(E_EXIST));
    return finish(0xB, mkdir_(p) == 0 ? 0 : ERR(E_NOENT));
}

s32 sceMc2ChdirAsync(s32 socket, const char *name, char *cur) {
    if (!card_present(socket)) return finish(0xC, ERR(E_NODEV));
    if (cur) snprintf(cur, 64, "/%s", g_cwd[socket & 7]);
    if (name) snprintf(g_cwd[socket & 7], sizeof g_cwd[0], "%s", name[0] == '/' ? name + 1 : name);
    return finish(0xC, 0);
}

s32 sceMc2ChmodAsync(s32 socket, const char *name, s32 mode) {
    if (!card_present(socket)) return finish(0xD, ERR(E_NODEV));
    return finish(0xD, 0);
}

s32 sceMc2GetEntSpaceAsync(s32 socket, const char *dir) {
    if (!card_present(socket)) return finish(0xF, ERR(E_NODEV));
    return finish(0xF, 64);
}

s32 sceMc2FormatAsync(s32 socket) {
    if (!card_present(socket)) return finish(3, ERR(E_NODEV));
    mkdirs(g_root);
    return finish(3, 0);
}

s32 sceMc2UnformatAsync(s32 socket) {
    return finish(4, card_present(socket) ? 0 : ERR(E_NODEV));
}

s32 sceMc2Format(s32 socket) {
    s32 r = sceMc2FormatAsync(socket);
    if (r < 0) return r;
    sceMc2CheckAsync(NULL, &r);
    return r;
}

s32 sceMc2Unformat(s32 socket) {
    s32 r = sceMc2UnformatAsync(socket);
    if (r < 0) return r;
    sceMc2CheckAsync(NULL, &r);
    return r;
}

#include "iso9660.h"
#include "log.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SECTOR 2048u

static FILE *g_iso;
static IsoFile g_root;

/* Folder mode: the disc's files copied to a folder (like PCSX2's host file system / QuickLoading).
   IsoFile.lba is then an index into g_host. */
#define MAX_HOST_FILES 512
static char g_folder[1024];
static struct { char path[1200]; FILE *f; } g_host[MAX_HOST_FILES];
static int g_nhost;

static int is_folder(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* case-insensitive lookup of one name in a host directory */
static int host_find_entry(const char *dir, const char *comp, size_t len, char *out, size_t outsz) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (len >= 2 && comp[len - 2] == ';') len -= 2;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        if (strlen(e->d_name) != len) continue;
        size_t i = 0;
        while (i < len && toupper((unsigned char)e->d_name[i]) == toupper((unsigned char)comp[i])) i++;
        if (i == len) {
            snprintf(out, outsz, "%s/%s", dir, e->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static int host_find(const char *path, IsoFile *out) {
    char cur[1200];
    struct stat st;
    snprintf(cur, sizeof cur, "%s", g_folder);
    while (*path == '/' || *path == '\\') path++;
    while (*path) {
        const char *end = path;
        char next[1200];
        while (*end && *end != '/' && *end != '\\') end++;
        if (host_find_entry(cur, path, (size_t)(end - path), next, sizeof next) != 0) return -1;
        memcpy(cur, next, sizeof cur);
        path = end;
        while (*path == '/' || *path == '\\') path++;
    }
    if (stat(cur, &st) != 0 || S_ISDIR(st.st_mode)) return -1;
    for (int i = 0; i < g_nhost; i++)
        if (!strcmp(g_host[i].path, cur)) {
            out->lba = (uint32_t)i;
            out->size = (uint32_t)st.st_size;
            return 0;
        }
    if (g_nhost == MAX_HOST_FILES) return -1;
    snprintf(g_host[g_nhost].path, sizeof g_host[g_nhost].path, "%s", cur);
    g_host[g_nhost].f = NULL;
    out->lba = (uint32_t)g_nhost++;
    out->size = (uint32_t)st.st_size;
    return 0;
}

static int64_t host_read(const IsoFile *f, uint32_t offset, void *dst, uint32_t size) {
    if (f->lba >= (uint32_t)g_nhost) return -1;
    if (!g_host[f->lba].f && !(g_host[f->lba].f = fopen(g_host[f->lba].path, "rb"))) return -1;
#ifdef _WIN32
    if (_fseeki64(g_host[f->lba].f, offset, SEEK_SET) != 0) return -1;
#else
    if (fseeko(g_host[f->lba].f, (off_t)offset, SEEK_SET) != 0) return -1;
#endif
    return (int64_t)fread(dst, 1, size, g_host[f->lba].f);
}

static int seek(uint64_t off) {
#ifdef _WIN32
    return _fseeki64(g_iso, (long long)off, SEEK_SET);
#else
    return fseeko(g_iso, (off_t)off, SEEK_SET);
#endif
}

static int read_at(uint64_t off, void *dst, uint32_t n) {
    if (seek(off) != 0) return -1;
    return fread(dst, 1, n, g_iso) == n ? 0 : -1;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int iso_open(const char *path) {
    uint8_t pvd[SECTOR];
    if (is_folder(path)) {
        snprintf(g_folder, sizeof g_folder, "%s", path);
        log_info("iso: using the disc's files in the folder %s", path);
        return 0;
    }
    g_iso = fopen(path, "rb");
    if (!g_iso) {
        log_error("iso: cannot open %s", path);
        return -1;
    }
    if (read_at(16ull * SECTOR, pvd, SECTOR) != 0 || memcmp(pvd + 1, "CD001", 5) != 0) {
        log_error("iso: %s is not an ISO 9660 image", path);
        iso_close();
        return -1;
    }
    g_root.lba = le32(pvd + 156 + 2);
    g_root.size = le32(pvd + 156 + 10);
    log_info("iso: opened %s", path);
    return 0;
}

void iso_close(void) {
    if (g_iso) fclose(g_iso);
    g_iso = NULL;
}

/* compare one path component with a directory record name ("NAME.EXT;1"), ignoring case and ";1" */
static int name_eq(const char *comp, size_t len, const uint8_t *name, uint8_t nlen) {
    size_t n = nlen;
    for (size_t i = 0; i < nlen; i++)
        if (name[i] == ';') { n = i; break; }
    if (len >= 2 && comp[len - 2] == ';') len -= 2;
    if (n != len) return 0;
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)comp[i]) != toupper(name[i])) return 0;
    return 1;
}

static int find_in_dir(const IsoFile *dir, const char *comp, size_t len, IsoFile *out, int *is_dir) {
    uint8_t *buf = malloc(dir->size);
    if (!buf || read_at((uint64_t)dir->lba * SECTOR, buf, dir->size) != 0) {
        free(buf);
        return -1;
    }
    for (uint32_t i = 0; i < dir->size;) {
        uint8_t rlen = buf[i];
        if (rlen == 0) {                      /* records don't cross sectors */
            i = (i / SECTOR + 1) * SECTOR;
            continue;
        }
        uint8_t nlen = buf[i + 32];
        if (name_eq(comp, len, buf + i + 33, nlen)) {
            out->lba = le32(buf + i + 2);
            out->size = le32(buf + i + 10);
            *is_dir = (buf[i + 25] & 2) != 0;
            free(buf);
            return 0;
        }
        i += rlen;
    }
    free(buf);
    return -1;
}

int iso_find(const char *path, IsoFile *out) {
    IsoFile cur = g_root;
    int is_dir = 1;
    if (g_folder[0]) return host_find(path, out);
    if (!g_iso) return -1;
    while (*path == '/' || *path == '\\') path++;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/' && *end != '\\') end++;
        if (!is_dir || find_in_dir(&cur, path, (size_t)(end - path), &cur, &is_dir) != 0) return -1;
        path = end;
        while (*path == '/' || *path == '\\') path++;
    }
    if (is_dir) return -1;
    *out = cur;
    return 0;
}

int64_t iso_read(const IsoFile *f, uint32_t offset, void *dst, uint32_t size) {
    if (offset >= f->size) return 0;
    if (size > f->size - offset) size = f->size - offset;
    if (g_folder[0]) return host_read(f, offset, dst, size);
    if (read_at((uint64_t)f->lba * SECTOR + offset, dst, size) != 0) return -1;
    return size;
}

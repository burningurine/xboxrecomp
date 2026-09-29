/*
 * kernel_zipfs.c - game files served straight out of a zip archive (the APK)
 *
 * An Android build can carry the title's disc files inside its own APK, under
 * assets/game/, so the app needs nothing from the player. The entries are
 * STORED, not deflated, which makes every file one contiguous byte range of
 * the APK: an open hands back an fd on the APK plus that range, and a read is
 * a plain pread. Nothing is copied or extracted.
 *
 * The archive is a read-only overlay on the game directory. A path under the
 * overlaid directory that names an archive entry is served from the archive;
 * every other path (saves, caches, anything the title writes) goes to the real
 * directory as before. Lookups ignore case, like FATX and the Xbox DVD.
 *
 * POSIX only. The Windows oracle reads its game directory directly.
 */

#include "kernel.h"

#if !defined(_WIN32)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

typedef struct {
    char     *name;          /* path below the root, archive spelling ("media/E01_fr.dds") */
    uint32_t  hash;          /* of the lower-cased name */
    int32_t   next_hash;     /* bucket chain */
    int32_t   parent;        /* -1 for the root */
    int32_t   first_child, last_child, next_sibling;
    int       is_dir;
    uint16_t  dos_time, dos_date;
    uint64_t  lho;           /* local header offset */
    uint64_t  data;          /* file data offset; 0 until first resolved */
    uint64_t  size;
} zipfs_node;

static struct {
    int          files;      /* files mounted; 0 = inactive */
    char         zip_path[1024];
    char         root[1024]; /* host directory the archive overlays */
    size_t       root_len;
    zipfs_node  *nodes;
    int32_t      count, cap;
    int32_t     *buckets;
    uint32_t     nbuckets;
} Z;

static pthread_mutex_t s_zlock = PTHREAD_MUTEX_INITIALIZER;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

static int read_at(int fd, void *buf, size_t n, uint64_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

static uint32_t name_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint8_t)tolower((unsigned char)s[i]);
        h *= 16777619u;
    }
    return h;
}

static int32_t zfind(const char *rel, size_t n)
{
    uint32_t h = name_hash(rel, n);
    int32_t i;
    if (!Z.buckets) return -1;
    for (i = Z.buckets[h & (Z.nbuckets - 1)]; i >= 0; i = Z.nodes[i].next_hash) {
        const zipfs_node *e = &Z.nodes[i];
        if (e->hash == h && strlen(e->name) == n && strncasecmp(e->name, rel, n) == 0)
            return i;
    }
    return -1;
}

/* Find or create the node for rel[0..n), creating its parents as directories. */
static int32_t zadd(const char *rel, size_t n, int is_dir)
{
    int32_t i = zfind(rel, n), parent = -1;
    zipfs_node *e;
    size_t cut;

    if (i >= 0) return i;
    if (n > 0) {
        cut = n;
        while (cut > 0 && rel[cut - 1] != '/') cut--;
        parent = zadd(rel, cut ? cut - 1 : 0, 1);
        if (parent < 0) return -1;
    }
    if (Z.count == Z.cap) {
        int32_t cap = Z.cap ? Z.cap * 2 : 1024;
        zipfs_node *nn = (zipfs_node *)realloc(Z.nodes, (size_t)cap * sizeof(*nn));
        if (!nn) return -1;
        Z.nodes = nn;
        Z.cap = cap;
    }
    i = Z.count++;
    e = &Z.nodes[i];
    memset(e, 0, sizeof(*e));
    e->name = (char *)malloc(n + 1);
    if (!e->name) { Z.count--; return -1; }
    memcpy(e->name, rel, n);
    e->name[n] = '\0';
    e->hash = name_hash(rel, n);
    e->parent = parent;
    e->first_child = e->last_child = e->next_sibling = -1;
    e->is_dir = is_dir;
    e->next_hash = Z.buckets[e->hash & (Z.nbuckets - 1)];
    Z.buckets[e->hash & (Z.nbuckets - 1)] = i;
    if (parent >= 0) {                       /* keep archive order */
        zipfs_node *p = &Z.nodes[parent];
        if (p->last_child >= 0) Z.nodes[p->last_child].next_sibling = i;
        else p->first_child = i;
        p->last_child = i;
    }
    return i;
}

/* The node a host path names, or -1. Paths outside the root never match. */
static int32_t zlookup(const char *host_path)
{
    char rel[1024];
    const char *p;
    size_t n = 0;

    if (!Z.files || !host_path || strncmp(host_path, Z.root, Z.root_len) != 0)
        return -1;
    p = host_path + Z.root_len;
    if (*p && *p != '/') return -1;          /* "<root>x" is a sibling, not a child */

    /* Collapse "//" and "." so the key matches the archive's spelling. */
    while (*p) {
        const char *c;
        size_t len;
        while (*p == '/') p++;
        c = p;
        while (*p && *p != '/') p++;
        len = (size_t)(p - c);
        if (len == 0 || (len == 1 && c[0] == '.')) continue;
        if (len == 2 && c[0] == '.' && c[1] == '.') {
            while (n > 0 && rel[n - 1] != '/') n--;
            if (n > 0) n--;
            continue;
        }
        if (n + len + 2 > sizeof(rel)) return -1;
        if (n) rel[n++] = '/';
        memcpy(rel + n, c, len);
        n += len;
    }
    return zfind(rel, n);
}

/* Offset of an entry's data: past its local header, whose name and extra
 * lengths can differ from the central directory's. */
static uint64_t zdata(int32_t i)
{
    zipfs_node *e = &Z.nodes[i];
    uint64_t data;
    pthread_mutex_lock(&s_zlock);
    data = e->data;
    if (!data) {
        uint8_t h[30];
        int fd = open(Z.zip_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (read_at(fd, h, sizeof(h), e->lho) && rd32(h) == 0x04034b50u)
                data = e->lho + 30u + rd16(h + 26) + rd16(h + 28);
            close(fd);
        }
        e->data = data;
    }
    pthread_mutex_unlock(&s_zlock);
    return data;
}

static void zstat(const zipfs_node *e, struct stat *st)
{
    struct tm tm;
    time_t t;

    memset(st, 0, sizeof(*st));
    st->st_mode = e->is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_nlink = 1;
    st->st_size = e->is_dir ? 0 : (off_t)e->size;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((e->dos_date >> 9) & 0x7F) + 80;
    tm.tm_mon = ((e->dos_date >> 5) & 0x0F) - 1;
    tm.tm_mday = e->dos_date & 0x1F;
    tm.tm_hour = (e->dos_time >> 11) & 0x1F;
    tm.tm_min = (e->dos_time >> 5) & 0x3F;
    tm.tm_sec = (e->dos_time & 0x1F) * 2;
    tm.tm_isdst = -1;
    t = e->dos_date ? mktime(&tm) : 0;
    if (t == (time_t)-1) t = 0;
    st->st_mtime = st->st_ctime = st->st_atime = t;
}

int xbox_zipfs_mount(const char *zip_path, const char *prefix, const char *host_root)
{
    uint8_t *tail = NULL, *cd = NULL;
    uint64_t fsz, tail_len, cd_off, cd_size, entries, k, bytes = 0;
    size_t plen = strlen(prefix), pos;
    int fd, skipped = 0;
    int64_t eocd = -1;
    int64_t i;

    if (Z.files)                                    /* once per process */
        return strcmp(zip_path, Z.zip_path) == 0 ? Z.files : -1;

    fd = open(zip_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "zipfs: cannot open %s (errno %d)", zip_path, errno);
        return -1;
    }
    fsz = (uint64_t)lseek(fd, 0, SEEK_END);
    tail_len = fsz < 65557 ? fsz : 65557;          /* EOCD + the longest comment */
    tail = (uint8_t *)malloc((size_t)tail_len);
    if (!tail || tail_len < 22 || !read_at(fd, tail, (size_t)tail_len, fsz - tail_len))
        goto fail;
    for (i = (int64_t)tail_len - 22; i >= 0; i--)
        if (rd32(tail + i) == 0x06054b50u) { eocd = i; break; }
    if (eocd < 0) goto fail;

    entries = rd16(tail + eocd + 10);
    cd_size = rd32(tail + eocd + 12);
    cd_off = rd32(tail + eocd + 16);
    if (entries == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        uint8_t loc[20], z64[56];                   /* zip64 locator sits right before the EOCD */
        uint64_t at = fsz - tail_len + (uint64_t)eocd;
        if (at < 20 || !read_at(fd, loc, sizeof(loc), at - 20) || rd32(loc) != 0x07064b50u ||
            !read_at(fd, z64, sizeof(z64), rd64(loc + 8)) || rd32(z64) != 0x06064b50u)
            goto fail;
        entries = rd64(z64 + 32);
        cd_size = rd64(z64 + 40);
        cd_off = rd64(z64 + 48);
    }
    if (cd_off + cd_size > fsz || cd_size > (256u << 20)) goto fail;
    cd = (uint8_t *)malloc((size_t)cd_size);
    if (!cd || !read_at(fd, cd, (size_t)cd_size, cd_off)) goto fail;

    Z.nbuckets = 1;
    while (Z.nbuckets < entries * 2 + 16) Z.nbuckets <<= 1;
    Z.buckets = (int32_t *)malloc(Z.nbuckets * sizeof(int32_t));
    if (!Z.buckets) goto fail;
    memset(Z.buckets, 0xFF, Z.nbuckets * sizeof(int32_t));
    if (zadd("", 0, 1) != 0) goto fail;             /* node 0 = the root */

    for (pos = 0, k = 0; k < entries; k++) {
        const uint8_t *c = cd + pos;
        uint16_t flags, method, nlen, xlen, clen;
        uint64_t csize, usize, lho;
        const char *name;
        int32_t n;

        if (pos + 46 > cd_size || rd32(c) != 0x02014b50u) break;
        flags = rd16(c + 8);
        method = rd16(c + 10);
        csize = rd32(c + 20);
        usize = rd32(c + 24);
        nlen = rd16(c + 28);
        xlen = rd16(c + 30);
        clen = rd16(c + 32);
        lho = rd32(c + 42);
        if (pos + 46 + nlen + xlen + clen > cd_size) break;
        name = (const char *)c + 46;
        pos += 46u + nlen + xlen + clen;

        if (nlen <= plen || strncmp(name, prefix, plen) != 0 || name[nlen - 1] == '/')
            continue;
        if (usize == 0xFFFFFFFFu || csize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) {
            const uint8_t *x = c + 46 + nlen, *xe = x + xlen;
            while (x + 4 <= xe) {                   /* zip64 extended information */
                uint16_t id = rd16(x), sz = rd16(x + 2);
                if (id == 0x0001) {
                    const uint8_t *v = x + 4;
                    if (usize == 0xFFFFFFFFu && v + 8 <= x + 4 + sz) { usize = rd64(v); v += 8; }
                    if (csize == 0xFFFFFFFFu && v + 8 <= x + 4 + sz) { csize = rd64(v); v += 8; }
                    if (lho == 0xFFFFFFFFu && v + 8 <= x + 4 + sz) { lho = rd64(v); }
                    break;
                }
                x += 4u + sz;
            }
        }
        /* Only stored, unencrypted entries are one readable byte range. */
        if (method != 0 || (flags & 1) || csize != usize) {
            if (skipped++ < 8)
                xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE,
                         "zipfs: %.*s is compressed or encrypted, skipped", (int)nlen, name);
            continue;
        }
        n = zadd(name + plen, nlen - plen, 0);
        if (n < 0) goto fail;
        Z.nodes[n].lho = lho;
        Z.nodes[n].size = usize;
        Z.nodes[n].dos_time = rd16(c + 12);
        Z.nodes[n].dos_date = rd16(c + 14);
        Z.files++;
        bytes += usize;
    }
    free(cd);
    free(tail);
    close(fd);

    snprintf(Z.zip_path, sizeof(Z.zip_path), "%s", zip_path);
    snprintf(Z.root, sizeof(Z.root), "%s", host_root);
    Z.root_len = strlen(Z.root);
    while (Z.root_len > 1 && Z.root[Z.root_len - 1] == '/')
        Z.root[--Z.root_len] = '\0';
    fprintf(stderr, "  [ZIPFS] %d game files (%llu MB) from %s over %s%s\n", Z.files,
            (unsigned long long)(bytes >> 20), zip_path, Z.root,
            skipped ? " (some entries skipped: not stored)" : "");
    fflush(stderr);
    return Z.files;

fail:
    xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "zipfs: %s is not a readable zip", zip_path);
    free(cd);
    free(tail);
    close(fd);
    for (i = 0; i < Z.count; i++) free(Z.nodes[i].name);
    free(Z.nodes);
    free(Z.buckets);
    memset(&Z, 0, sizeof(Z));
    return -1;
}

int xbox_zipfs_active(void) { return Z.files > 0; }

int xbox_zipfs_stat(const char *host_path, struct stat *st)
{
    int32_t i = zlookup(host_path);
    if (i < 0) return 0;
    if (st) zstat(&Z.nodes[i], st);
    return Z.nodes[i].is_dir ? 2 : 1;
}

int xbox_zipfs_canon(char *host_path, size_t n)
{
    int32_t i = zlookup(host_path);
    if (i < 0) return 0;
    if (Z.nodes[i].name[0])
        snprintf(host_path, n, "%s/%s", Z.root, Z.nodes[i].name);
    else
        snprintf(host_path, n, "%s", Z.root);
    return 1;
}

int xbox_zipfs_open(const char *host_path, int64_t *base, int64_t *size)
{
    int32_t i = zlookup(host_path);
    uint64_t data;
    int fd;

    if (i < 0) return -1;
    fd = open(Z.zip_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (Z.nodes[i].is_dir) {                        /* a placeholder fd; never read */
        *base = 0;
        *size = 0;
        return fd;
    }
    data = zdata(i);
    if (!data) { close(fd); return -1; }
    lseek(fd, (off_t)data, SEEK_SET);
    *base = (int64_t)data;
    *size = (int64_t)Z.nodes[i].size;
    return fd;
}

int xbox_zipfs_read_file(const char *host_path, void **out, size_t *out_size)
{
    int64_t base, size;
    void *buf;
    int fd = xbox_zipfs_open(host_path, &base, &size);
    if (fd < 0) return 0;
    buf = size > 0 ? malloc((size_t)size) : NULL;
    if (!buf || !read_at(fd, buf, (size_t)size, (uint64_t)base)) {
        free(buf);
        close(fd);
        return 0;
    }
    close(fd);
    *out = buf;
    *out_size = (size_t)size;
    return 1;
}

int32_t xbox_zipfs_first_child(const char *host_dir)
{
    int32_t i = zlookup(host_dir);
    return (i >= 0 && Z.nodes[i].is_dir) ? Z.nodes[i].first_child : -1;
}

int32_t xbox_zipfs_child(int32_t node, const char **leaf, struct stat *st)
{
    const zipfs_node *e = &Z.nodes[node];
    const char *slash = strrchr(e->name, '/');
    if (leaf) *leaf = slash ? slash + 1 : e->name;
    if (st) zstat(e, st);
    return e->next_sibling;
}

#else
typedef int xbox_zipfs_unused;   /* the Windows backend reads the game directory */
#endif

/* FAT16/FAT32 reader with Linux 2.4 vfat semantics. See card.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gport2x/card.h"
#include "gport2x/log.h"

#define DIRENT_SIZE 32u
#define ATTR_RO 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR 0x10
#define ATTR_ARCH 0x20
#define ATTR_EXT 0x0F /* long-name slot */
#define DELETED_FLAG 0xE5
#define MSDOS_ROOT_INO 1
#define MSDOS_SUPER_MAGIC 0x4D44
#define LFN_MAX_SLOTS 20
#define LFN_CHARS_PER_SLOT 13

struct card {
    int fd;
    const uint8_t *img;
    size_t img_len;
    uint32_t bytes_per_sector, sectors_per_cluster, reserved, nfats, root_entries;
    uint32_t sectors_per_fat, total_sectors, fat_bits, root_cluster;
    uint32_t fat_sector, root_sector, root_sectors, data_sector;
    uint32_t clusters;      /* data clusters (2.4: (total - data_start) / spc) */
    uint32_t cluster_bytes;
    int64_t free_clusters;  /* -1 until counted */
    int tz_minuteswest;
    unsigned umask;
    uint32_t uid, gid, dev;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Linux 2.4 fs/fat/misc.c date_dos2unix, reimplemented from its definition. */
int64_t card_fat_time_to_unix(uint16_t time, uint16_t date, int tz_minuteswest)
{
    static const int day_n[16] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 0, 0, 0, 0 };
    int month = ((date >> 5) & 15) - 1;
    int year = date >> 9;
    if (month < 0)
        month = 0; /* a zero month field; the kernel reads before day_n[0] here */
    int64_t secs = (time & 31) * 2 + 60 * ((time >> 5) & 63) + (time >> 11) * 3600 +
                   86400LL * ((date & 31) - 1 + day_n[month] + (year / 4) + year * 365 -
                              ((year & 3) == 0 && month < 2 ? 1 : 0) + 3653);
    secs += (int64_t)tz_minuteswest * 60;
    return secs;
}

static const uint8_t *sector_ptr(const card_t *c, uint64_t sector)
{
    uint64_t off = sector * c->bytes_per_sector;
    if (off + c->bytes_per_sector > c->img_len)
        return NULL;
    return c->img + off;
}

/* FAT entry n: 0 = free, else the next cluster; UINT32_MAX = end of chain or bad. */
static uint32_t fat_next(const card_t *c, uint32_t n)
{
    uint64_t off = (uint64_t)c->fat_sector * c->bytes_per_sector;
    uint32_t v;
    if (c->fat_bits == 16) {
        off += (uint64_t)n * 2;
        if (off + 2 > c->img_len)
            return UINT32_MAX;
        v = rd16(c->img + off);
        if (v >= 0xFFF7)
            return UINT32_MAX;
    } else {
        off += (uint64_t)n * 4;
        if (off + 4 > c->img_len)
            return UINT32_MAX;
        v = rd32(c->img + off) & 0x0FFFFFFF;
        if (v >= 0x0FFFFFF7)
            return UINT32_MAX;
    }
    if (v != 0 && (v < 2 || v >= c->clusters + 2))
        return UINT32_MAX; /* out of range: treat as end, as a chain walk must stop */
    return v;
}

static uint64_t cluster_sector(const card_t *c, uint32_t n)
{
    return (uint64_t)c->data_sector + (uint64_t)(n - 2) * c->sectors_per_cluster;
}

int card_open(const char *image_path, card_t **out)
{
    *out = NULL;
    int fd = open(image_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 512) {
        close(fd);
        return -EINVAL;
    }
    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -errno;
    }
    card_t *c = calloc(1, sizeof *c);
    if (!c) {
        munmap(map, (size_t)st.st_size);
        close(fd);
        return -ENOMEM;
    }
    c->fd = fd;
    c->img = map;
    c->img_len = (size_t)st.st_size;
    const uint8_t *b = c->img;
    c->bytes_per_sector = rd16(b + 11);
    c->sectors_per_cluster = b[13];
    c->reserved = rd16(b + 14);
    c->nfats = b[16];
    c->root_entries = rd16(b + 17);
    c->total_sectors = rd16(b + 19);
    if (!c->total_sectors)
        c->total_sectors = rd32(b + 32);
    c->sectors_per_fat = rd16(b + 22);
    if (!c->sectors_per_fat) {
        c->sectors_per_fat = rd32(b + 36); /* FAT32 BPB_FATSz32 */
        c->root_cluster = rd32(b + 44);
    }
    bool sane = rd16(b + 510) == 0xAA55 && (c->bytes_per_sector == 512 || c->bytes_per_sector == 1024 ||
                                            c->bytes_per_sector == 2048 || c->bytes_per_sector == 4096) &&
                c->sectors_per_cluster && !(c->sectors_per_cluster & (c->sectors_per_cluster - 1)) &&
                c->reserved && c->nfats && c->sectors_per_fat && c->total_sectors;
    if (!sane) {
        card_close(c);
        return -EINVAL;
    }
    c->cluster_bytes = c->bytes_per_sector * c->sectors_per_cluster;
    c->fat_sector = c->reserved;
    c->root_sector = c->reserved + c->nfats * c->sectors_per_fat;
    c->root_sectors = (c->root_entries * DIRENT_SIZE + c->bytes_per_sector - 1) / c->bytes_per_sector;
    c->data_sector = c->root_sector + c->root_sectors;
    if (c->data_sector >= c->total_sectors) {
        card_close(c);
        return -EINVAL;
    }
    c->clusters = (c->total_sectors - c->data_sector) / c->sectors_per_cluster;
    /* FAT type by cluster count (the only valid determination). */
    if (c->clusters < 4085) {
        card_close(c); /* FAT12: not a GP2X card size; unsupported */
        return -EINVAL;
    }
    c->fat_bits = c->clusters < 65525 ? 16 : 32;
    if (c->fat_bits == 32 && c->root_entries) {
        card_close(c);
        return -EINVAL;
    }
    if (c->fat_bits == 16 && !c->root_entries) {
        card_close(c);
        return -EINVAL;
    }
    c->free_clusters = -1;
    c->umask = 022;
    c->dev = 0xF101; /* the card's block device id as the guest sees it (OPEN: genuine major) */
    gp_info("card: FAT%u, %u B/sector, %u sectors/cluster, %u clusters, %u total sectors", c->fat_bits,
            c->bytes_per_sector, c->sectors_per_cluster, c->clusters, c->total_sectors);
    *out = c;
    return 0;
}

void card_close(card_t *c)
{
    if (!c)
        return;
    if (c->img)
        munmap((void *)c->img, c->img_len);
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}

void card_set_tz_minuteswest(card_t *c, int minuteswest) { c->tz_minuteswest = minuteswest; }
void card_set_umask(card_t *c, unsigned umask) { c->umask = umask & 0777; }
void card_set_owner(card_t *c, uint32_t uid, uint32_t gid) { c->uid = uid; c->gid = gid; }
void card_set_dev(card_t *c, uint32_t dev) { c->dev = dev; }
uint32_t card_cluster_bytes(const card_t *c) { return c->cluster_bytes; }
uint32_t card_cluster_count(const card_t *c) { return c->clusters; }

void card_root(const card_t *c, card_node_t *out)
{
    memset(out, 0, sizeof *out);
    out->is_dir = true;
    out->is_root = true;
    out->first_cluster = c->fat_bits == 32 ? c->root_cluster : 0;
    out->ino = MSDOS_ROOT_INO;
    out->attr = ATTR_DIR;
}

/* Directory streams: a directory is a sequence of 32-byte slots. For the
 * FAT16 root they are the fixed root sectors; otherwise a cluster chain. */
typedef struct dir_iter {
    const card_t *c;
    const card_node_t *dir;
    uint32_t pos;        /* byte offset of the next slot */
    uint32_t cluster;    /* current cluster (chain dirs) */
    uint32_t cluster_pos; /* byte offset where the current cluster starts */
    bool fixed;          /* FAT16 root */
} dir_iter_t;

static void dir_iter_init(const card_t *c, const card_node_t *dir, dir_iter_t *it)
{
    memset(it, 0, sizeof *it);
    it->c = c;
    it->dir = dir;
    it->fixed = dir->is_root && c->fat_bits == 16;
    it->cluster = dir->first_cluster;
}

/* Positions the iterator at byte offset pos (walking the chain). */
static bool dir_iter_seek(dir_iter_t *it, uint32_t pos)
{
    it->pos = pos;
    if (it->fixed)
        return pos < it->c->root_entries * DIRENT_SIZE;
    it->cluster = it->dir->first_cluster;
    it->cluster_pos = 0;
    if (it->cluster < 2)
        return false;
    while (pos >= it->cluster_pos + it->c->cluster_bytes) {
        uint32_t next = fat_next(it->c, it->cluster);
        if (next == UINT32_MAX || next == 0)
            return false;
        it->cluster = next;
        it->cluster_pos += it->c->cluster_bytes;
    }
    return true;
}

/* The slot at the iterator (NULL at the end of the directory), advancing. */
static const uint8_t *dir_iter_next(dir_iter_t *it, uint32_t *slot_pos, uint64_t *slot_sector, uint32_t *slot_index)
{
    const card_t *c = it->c;
    uint64_t sector;
    uint32_t in_sector;
    if (it->fixed) {
        if (it->pos >= c->root_entries * DIRENT_SIZE)
            return NULL;
        sector = c->root_sector + it->pos / c->bytes_per_sector;
        in_sector = it->pos % c->bytes_per_sector;
    } else {
        if (it->cluster < 2)
            return NULL;
        if (it->pos >= it->cluster_pos + c->cluster_bytes) {
            uint32_t next = fat_next(c, it->cluster);
            if (next == UINT32_MAX || next == 0)
                return NULL;
            it->cluster = next;
            it->cluster_pos += c->cluster_bytes;
        }
        uint32_t in_cluster = it->pos - it->cluster_pos;
        sector = cluster_sector(c, it->cluster) + in_cluster / c->bytes_per_sector;
        in_sector = in_cluster % c->bytes_per_sector;
    }
    const uint8_t *sp = sector_ptr(c, sector);
    if (!sp)
        return NULL;
    *slot_pos = it->pos;
    *slot_sector = sector;
    *slot_index = in_sector / DIRENT_SIZE;
    it->pos += DIRENT_SIZE;
    return sp + in_sector;
}

static uint8_t short_checksum(const uint8_t *name11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

/* One UTF-16 unit to UTF-8, as 2.4's utf8 NLS did it (no surrogate pairing). */
static size_t put_utf8(uint16_t u, char *out)
{
    if (u < 0x80) {
        out[0] = (char)u;
        return 1;
    }
    if (u < 0x800) {
        out[0] = (char)(0xC0 | (u >> 6));
        out[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (u >> 12));
    out[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    out[2] = (char)(0x80 | (u & 0x3F));
    return 3;
}

/* The display form of a short entry: 2.4 vfat lower-cases it. */
static size_t short_display_name(const uint8_t *slot, char *out)
{
    size_t n = 0, last = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t ch = slot[i];
        if (!ch)
            break;
        if (ch >= 'A' && ch <= 'Z')
            ch += 32;
        if (ch == 0x05)
            ch = 0xE5;
        if (ch != ' ')
            last = n + 1;
        out[n++] = (char)ch;
    }
    n = last;
    out[n++] = '.';
    for (int i = 8; i < 11; i++) {
        uint8_t ch = slot[i];
        if (!ch)
            break;
        if (ch >= 'A' && ch <= 'Z')
            ch += 32;
        if (ch != ' ')
            last = n + 1;
        out[n++] = (char)ch;
    }
    n = last;
    out[n] = 0;
    return n;
}

/* The next real entry of a directory with its name (long if valid, else
 * short). Returns the slot pointer or NULL at the end. */
typedef struct dir_entry {
    const uint8_t *slot;
    uint32_t pos;    /* byte offset of the short slot */
    uint32_t next;   /* byte offset after the short slot */
    uint64_t sector;
    uint32_t index;
    char name[256];
    size_t namelen;
    bool has_long;
} dir_entry_t;

static bool dir_next_entry(dir_iter_t *it, dir_entry_t *e)
{
    uint16_t uni[LFN_MAX_SLOTS * LFN_CHARS_PER_SLOT + 1];
    int slots_left = 0, total_slots = 0;
    uint8_t alias_sum = 0;
    for (;;) {
        uint32_t pos, index;
        uint64_t sector;
        const uint8_t *s = dir_iter_next(it, &pos, &sector, &index);
        if (!s)
            return false;
        uint8_t first = s[0];
        if (first == DELETED_FLAG || first == 0) {
            slots_left = 0;
            continue; /* free slot (2.4 keeps scanning past a zero name) */
        }
        uint8_t attr = s[11];
        if (attr == ATTR_EXT) {
            uint8_t id = s[0];
            if (id & 0x40) {
                slots_left = id & 0x3F;
                if (slots_left < 1 || slots_left > LFN_MAX_SLOTS) {
                    slots_left = 0;
                    continue;
                }
                total_slots = slots_left;
                alias_sum = s[13];
                memset(uni, 0, sizeof uni);
            } else if (slots_left == 0 || (id & 0x3F) != slots_left || s[13] != alias_sum) {
                slots_left = 0;
                continue;
            }
            int slot = (id & 0x3F) - 1;
            uint16_t *dst = uni + slot * LFN_CHARS_PER_SLOT;
            for (int i = 0; i < 5; i++)
                dst[i] = rd16(s + 1 + i * 2);
            for (int i = 0; i < 6; i++)
                dst[5 + i] = rd16(s + 14 + i * 2);
            dst[11] = rd16(s + 28);
            dst[12] = rd16(s + 30);
            slots_left--;
            continue;
        }
        if (attr & ATTR_VOLUME) {
            slots_left = 0;
            continue;
        }
        e->slot = s;
        e->pos = pos;
        e->next = pos + DIRENT_SIZE;
        e->sector = sector;
        e->index = index;
        e->has_long = false;
        if (total_slots && slots_left == 0 && short_checksum(s) == alias_sum) {
            size_t n = 0;
            for (int i = 0; i < total_slots * LFN_CHARS_PER_SLOT; i++) {
                uint16_t u = uni[i];
                if (u == 0 || u == 0xFFFF)
                    break;
                if (n + 3 >= sizeof e->name)
                    break;
                n += put_utf8(u, e->name + n);
            }
            e->name[n] = 0;
            e->namelen = n;
            e->has_long = n > 0;
        }
        if (!e->has_long)
            e->namelen = short_display_name(s, e->name);
        return true;
    }
}

static void node_from_slot(const card_t *c, const dir_entry_t *e, card_node_t *out)
{
    const uint8_t *s = e->slot;
    memset(out, 0, sizeof *out);
    out->attr = s[11];
    out->is_dir = (s[11] & ATTR_DIR) != 0;
    out->first_cluster = rd16(s + 26);
    if (c->fat_bits == 32)
        out->first_cluster |= (uint32_t)rd16(s + 20) << 16;
    out->size = rd32(s + 28);
    out->ctime_cs = s[13];
    out->ctime = rd16(s + 14);
    out->cdate = rd16(s + 16);
    out->time = rd16(s + 22);
    out->date = rd16(s + 24);
    out->ino = (uint32_t)(e->sector << 4) | e->index;
    if (out->is_dir && c->fat_bits == 32 && out->first_cluster == c->root_cluster) {
        out->is_root = true; /* ".." of a first-level directory on FAT32 */
        out->ino = MSDOS_ROOT_INO;
    }
    if (out->is_dir && out->first_cluster == 0 && c->fat_bits == 16) {
        out->is_root = true; /* ".." pointing at the root */
        out->ino = MSDOS_ROOT_INO;
    }
}

static int ascii_casecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return x - y;
        if (!x)
            return 0;
    }
}

int card_lookup_in(card_t *c, const card_node_t *dir, const char *name, card_node_t *out)
{
    if (!dir->is_dir)
        return -ENOTDIR;
    if (!*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
        return -EINVAL;
    dir_iter_t it;
    dir_iter_init(c, dir, &it);
    if (!dir_iter_seek(&it, 0))
        return -ENOENT;
    dir_entry_t e;
    while (dir_next_entry(&it, &e)) {
        if (!ascii_casecmp(e.name, name)) {
            node_from_slot(c, &e, out);
            return 0;
        }
        if (e.has_long) {
            char shortname[13];
            short_display_name(e.slot, shortname);
            if (!ascii_casecmp(shortname, name)) {
                node_from_slot(c, &e, out);
                return 0;
            }
        }
    }
    return -ENOENT;
}

int card_lookup(card_t *c, const char *path, card_node_t *out)
{
    card_node_t cur;
    card_root(c, &cur);
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= 256)
            return -ENAMETOOLONG;
        char comp[256];
        memcpy(comp, p, n);
        comp[n] = 0;
        card_node_t next;
        int r = card_lookup_in(c, &cur, comp, &next);
        if (r < 0)
            return r;
        cur = next;
        p += n;
    }
    *out = cur;
    return 0;
}

static uint64_t chain_length(const card_t *c, uint32_t first)
{
    uint64_t n = 0;
    uint32_t cl = first;
    while (cl >= 2 && cl != UINT32_MAX && n < c->clusters + 2) {
        n++;
        cl = fat_next(c, cl);
    }
    return n;
}

static uint32_t count_subdirs(card_t *c, const card_node_t *dir)
{
    dir_iter_t it;
    dir_iter_init(c, dir, &it);
    if (!dir_iter_seek(&it, 0))
        return 0;
    dir_entry_t e;
    uint32_t n = 0;
    while (dir_next_entry(&it, &e))
        if ((e.slot[11] & ATTR_DIR) && strcmp(e.name, ".") && strcmp(e.name, ".."))
            n++;
    return n;
}

int card_stat(card_t *c, const card_node_t *n, card_stat_t *st)
{
    memset(st, 0, sizeof *st);
    st->ino = n->ino;
    st->uid = c->uid;
    st->gid = c->gid;
    st->dev = c->dev;
    st->blksize = c->cluster_bytes;
    uint32_t perm = (n->attr & ATTR_RO) ? 0555 : 0777;
    if (n->is_root) {
        st->mode = 040000 | (0777 & ~c->umask);
        st->nlink = 2 + count_subdirs(c, n);
        st->size = c->fat_bits == 16 ? (uint64_t)c->root_entries * DIRENT_SIZE
                                     : chain_length(c, n->first_cluster) * c->cluster_bytes;
        /* 2.4 fat_read_root: times are 0 */
    } else if (n->is_dir) {
        st->mode = 040000 | (perm & ~c->umask);
        st->nlink = 2 + count_subdirs(c, n);
        st->size = chain_length(c, n->first_cluster) * c->cluster_bytes;
        st->mtime = st->atime = card_fat_time_to_unix(n->time, n->date, c->tz_minuteswest);
        st->ctime = card_fat_time_to_unix(n->ctime, n->cdate, c->tz_minuteswest);
    } else {
        st->mode = 0100000 | (perm & ~c->umask);
        st->nlink = 1;
        st->size = n->size;
        st->mtime = st->atime = card_fat_time_to_unix(n->time, n->date, c->tz_minuteswest);
        st->ctime = card_fat_time_to_unix(n->ctime, n->cdate, c->tz_minuteswest);
    }
    st->blocks = ((st->size + c->cluster_bytes - 1) & ~(uint64_t)(c->cluster_bytes - 1)) >> 9;
    return 0;
}

int card_statfs(card_t *c, card_statfs_t *out)
{
    if (c->free_clusters < 0) {
        uint64_t free_n = 0;
        for (uint32_t n = 2; n < c->clusters + 2; n++)
            if (fat_next(c, n) == 0)
                free_n++;
        c->free_clusters = (int64_t)free_n;
    }
    memset(out, 0, sizeof *out);
    out->type = MSDOS_SUPER_MAGIC;
    out->bsize = c->cluster_bytes;
    out->frsize = c->cluster_bytes;
    out->blocks = c->clusters;
    out->bfree = (uint32_t)c->free_clusters;
    out->bavail = (uint32_t)c->free_clusters;
    out->namelen = 260;
    return 0;
}

int64_t card_read(card_t *c, const card_node_t *n, uint64_t off, void *buf, size_t len)
{
    if (n->is_dir)
        return -EISDIR;
    if (off >= n->size)
        return 0;
    if (len > n->size - off)
        len = (size_t)(n->size - off);
    uint8_t *dst = buf;
    size_t done = 0;
    /* Walk the chain to the cluster holding off, then copy cluster by cluster. */
    uint32_t cl = n->first_cluster;
    uint64_t skip = off / c->cluster_bytes;
    while (skip && cl >= 2 && cl != UINT32_MAX) {
        cl = fat_next(c, cl);
        skip--;
    }
    uint32_t in_cluster = (uint32_t)(off % c->cluster_bytes);
    while (done < len) {
        if (cl < 2 || cl == UINT32_MAX)
            return done ? (int64_t)done : -EIO; /* chain shorter than the size field */
        size_t chunk = c->cluster_bytes - in_cluster;
        if (chunk > len - done)
            chunk = len - done;
        uint64_t byte = cluster_sector(c, cl) * c->bytes_per_sector + in_cluster;
        if (byte + chunk > c->img_len)
            return done ? (int64_t)done : -EIO;
        memcpy(dst + done, c->img + byte, chunk);
        done += chunk;
        in_cluster = 0;
        if (done < len)
            cl = fat_next(c, cl);
    }
    return (int64_t)done;
}

/* Cookies: for the root, 0 and 1 are the synthesised "." and "..", and
 * 2 + byte offset continues in the slots. Other directories use the byte
 * offset directly; their "." and ".." are on disk. */
int card_readdir(card_t *c, const card_node_t *dir, uint32_t cookie, card_dirent_t *out)
{
    if (!dir->is_dir)
        return -ENOTDIR;
    memset(out, 0, sizeof *out);
    uint32_t base = 0;
    if (dir->is_root) {
        if (cookie < 2) {
            strcpy(out->name, cookie == 0 ? "." : "..");
            out->ino = MSDOS_ROOT_INO;
            out->is_dir = true;
            out->off = cookie + 1;
            return 1;
        }
        base = 2;
    }
    if ((cookie - base) & (DIRENT_SIZE - 1))
        return -ENOENT; /* 2.4: a cookie that is not a slot boundary */
    dir_iter_t it;
    dir_iter_init(c, dir, &it);
    if (!dir_iter_seek(&it, cookie - base))
        return 0;
    dir_entry_t e;
    if (!dir_next_entry(&it, &e))
        return 0;
    memcpy(out->name, e.name, e.namelen + 1);
    out->is_dir = (e.slot[11] & ATTR_DIR) != 0;
    if (!strcmp(e.name, ".")) {
        out->ino = dir->ino;
    } else if (!strcmp(e.name, "..")) {
        card_node_t parent;
        node_from_slot(c, &e, &parent);
        out->ino = parent.is_root ? MSDOS_ROOT_INO : parent.ino; /* 2.4 fat_parent_ino: the parent's own slot */
    } else {
        out->ino = (uint32_t)(e.sector << 4) | e.index;
    }
    out->off = base + e.next;
    return 1;
}

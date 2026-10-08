/* GPort2X card reader: the user's FAT16/FAT32 card image, read-only, with the
 * semantics Linux 2.4 vfat gives the guest (spec 6.2, 6.4, #8):
 *   - sizes and FAT timestamps (local FAT time + the kernel timezone,
 *     OPEN-17: the firmware never sets one, so the default offset is 0);
 *   - readdir in on-disk order; the root directory gets "." and ".."
 *     synthesised first; long names win, short-only names display in lower
 *     case (2.4 vfat "shortname=lower"); cookies fit in 32 bits;
 *   - statfs geometry: f_bsize = f_frsize = cluster size, f_blocks = data
 *     clusters, f_namelen 260, f_type 0x4D44;
 *   - case-insensitive lookup against the long and the short name;
 *   - inode numbers, modes (0777 & ~umask, read-only attribute drops w),
 *     nlink, blocks as 2.4 fat computes them.
 * The image is mapped read-only and never written. Functions that can fail
 * return 0 or a negative errno. */
#ifndef GPORT2X_CARD_H
#define GPORT2X_CARD_H

#include "gport2x/types.h"

typedef struct card card_t;

/* A directory entry resolved to its on-disk facts: the handle for stat,
 * read and readdir. */
typedef struct card_node {
    bool is_dir;
    bool is_root;
    uint32_t first_cluster; /* 0: FAT16 root, or an empty file / directory */
    uint32_t size;          /* files: the size field */
    uint32_t ino;           /* 2.4 fat: (directory sector << 4) | entry index; root = 1 */
    uint8_t attr;
    uint8_t ctime_cs;
    uint16_t time, date, ctime, cdate;
} card_node_t;

typedef struct card_stat {
    uint32_t ino, mode, nlink, uid, gid, dev;
    uint64_t size; /* directories: cluster-chain length x cluster size (FAT16 root: root entries x 32) */
    uint32_t blksize;
    uint64_t blocks; /* 512-byte units, rounded up to a whole cluster */
    int64_t atime, mtime, ctime;
} card_stat_t;

typedef struct card_statfs {
    uint32_t type, bsize, frsize, blocks, bfree, bavail, files, ffree, namelen;
} card_statfs_t;

typedef struct card_dirent {
    char name[256];
    uint32_t ino;
    uint32_t off; /* the cookie that continues after this entry (fits 31 bits) */
    bool is_dir;
} card_dirent_t;

int card_open(const char *image_path, card_t **out); /* -errno; -EINVAL if not FAT16/FAT32 */
void card_close(card_t *c);

/* Mount-time facts (defaults: tz 0, umask 022, uid/gid 0, dev 0xF101). */
void card_set_tz_minuteswest(card_t *c, int minuteswest);
void card_set_umask(card_t *c, unsigned umask);
void card_set_owner(card_t *c, uint32_t uid, uint32_t gid);
void card_set_dev(card_t *c, uint32_t dev);

uint32_t card_cluster_bytes(const card_t *c);
uint32_t card_cluster_count(const card_t *c);

void card_root(const card_t *c, card_node_t *out);
/* One path component in dir (no '/', not "." or ".."). -ENOENT, -ENOTDIR. */
int card_lookup_in(card_t *c, const card_node_t *dir, const char *name, card_node_t *out);
/* A '/'-separated path from the root; "." and ".." components are not allowed
 * (the fs layer normalises paths lexically, as the kernel's dentry walk does). */
int card_lookup(card_t *c, const char *path, card_node_t *out);

int card_stat(card_t *c, const card_node_t *n, card_stat_t *st);
int card_statfs(card_t *c, card_statfs_t *out);

/* Reads up to len bytes of a file at off. Returns the count (0 at EOF). */
int64_t card_read(card_t *c, const card_node_t *n, uint64_t off, void *buf, size_t len);

/* Returns 1 with the entry at cookie (0 = first), 0 at the end, -errno. */
int card_readdir(card_t *c, const card_node_t *dir, uint32_t cookie, card_dirent_t *out);

/* Linux 2.4 fat date_dos2unix: FAT local date/time to epoch seconds. */
int64_t card_fat_time_to_unix(uint16_t time, uint16_t date, int tz_minuteswest);

#endif /* GPORT2X_CARD_H */

/* Card test helper: prints what the card module reports, for test_card.py.
 *   tool_card statfs IMG
 *   tool_card stat IMG PATH
 *   tool_card ls IMG PATH
 *   tool_card cat IMG PATH [OFF LEN]      (raw bytes to stdout)
 *   tool_card fattime TIME DATE TZMINUTESWEST */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gport2x/card.h"

static int fail(const char *what, int err)
{
    fprintf(stderr, "%s: %s\n", what, strerror(-err));
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    if (!strcmp(argv[1], "fattime")) {
        if (argc != 5)
            return 2;
        printf("%lld\n", (long long)card_fat_time_to_unix((uint16_t)strtoul(argv[2], NULL, 0),
                                                           (uint16_t)strtoul(argv[3], NULL, 0), atoi(argv[4])));
        return 0;
    }
    card_t *c;
    int r = card_open(argv[2], &c);
    if (r < 0)
        return fail("open", r);
    if (!strcmp(argv[1], "statfs")) {
        card_statfs_t s;
        card_statfs(c, &s);
        printf("type %#x bsize %u frsize %u blocks %u bfree %u bavail %u files %u ffree %u namelen %u\n", s.type,
               s.bsize, s.frsize, s.blocks, s.bfree, s.bavail, s.files, s.ffree, s.namelen);
        return 0;
    }
    if (argc < 4)
        return 2;
    card_node_t n;
    r = card_lookup(c, argv[3], &n);
    if (r < 0)
        return fail("lookup", r);
    if (!strcmp(argv[1], "stat")) {
        card_stat_t s;
        card_stat(c, &n, &s);
        printf("ino %u mode %o nlink %u size %llu blksize %u blocks %llu mtime %lld ctime %lld atime %lld dev %#x uid %u gid %u\n",
               s.ino, s.mode, s.nlink, (unsigned long long)s.size, s.blksize, (unsigned long long)s.blocks,
               (long long)s.mtime, (long long)s.ctime, (long long)s.atime, s.dev, s.uid, s.gid);
        return 0;
    }
    if (!strcmp(argv[1], "ls")) {
        card_dirent_t d;
        uint32_t cookie = 0;
        for (;;) {
            r = card_readdir(c, &n, cookie, &d);
            if (r < 0)
                return fail("readdir", r);
            if (r == 0)
                break;
            printf("%s\t%u\t%u\t%d\n", d.name, d.ino, d.off, d.is_dir);
            cookie = d.off;
        }
        return 0;
    }
    if (!strcmp(argv[1], "cat")) {
        uint64_t off = argc > 4 ? strtoull(argv[4], NULL, 0) : 0;
        size_t len = argc > 5 ? strtoul(argv[5], NULL, 0) : n.size;
        uint8_t *buf = malloc(len ? len : 1);
        int64_t got = card_read(c, &n, off, buf, len);
        if (got < 0)
            return fail("read", (int)got);
        fwrite(buf, 1, (size_t)got, stdout);
        return 0;
    }
    return 2;
}

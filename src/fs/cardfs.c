/* The card image as a guest backend (read-only; card.h provides the
 * genuine semantics). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gport2x/fs.h"

static void map_stat(const fs_backend_t *b, const card_stat_t *cs, fs_stat_t *st)
{
    memset(st, 0, sizeof *st);
    st->dev = b->dev;
    st->ino = cs->ino;
    st->mode = cs->mode;
    st->nlink = cs->nlink;
    st->uid = cs->uid;
    st->gid = cs->gid;
    st->size = (int64_t)cs->size;
    st->blksize = cs->blksize;
    st->blocks = cs->blocks;
    st->atime = cs->atime;
    st->mtime = cs->mtime;
    st->ctime = cs->ctime;
}

static int cf_stat(fs_backend_t *b, const char *rel, fs_stat_t *st)
{
    card_t *c = b->priv;
    card_node_t n;
    int r = card_lookup(c, rel, &n);
    if (r < 0)
        return r;
    card_stat_t cs;
    card_stat(c, &n, &cs);
    map_stat(b, &cs, st);
    return 0;
}

/* The file's private state. f->backend may be re-pointed at an overlay
 * that wraps this backend, so the card is kept here. */
typedef struct cardfile {
    card_t *card;
    card_node_t node;
} cardfile_t;

static int64_t cf_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    cardfile_t *cf = f->priv;
    return card_read(cf->card, &cf->node, (uint64_t)off, buf, n);
}

static int cf_fstat(fs_file_t *f, fs_stat_t *st)
{
    cardfile_t *cf = f->priv;
    card_stat_t cs;
    card_stat(cf->card, &cf->node, &cs);
    map_stat(f->backend, &cs, st);
    return 0;
}

static void cf_release(fs_file_t *f)
{
    free(f->priv);
}

static const fs_file_ops_t cardfile_ops = { .read = cf_read, .fstat = cf_fstat, .release = cf_release };

static int cf_open(fs_backend_t *b, const char *rel, int gflags, uint32_t mode, fs_file_t **out)
{
    (void)mode;
    if ((gflags & GUEST_O_ACCMODE) != GUEST_O_RDONLY || (gflags & (GUEST_O_CREAT | GUEST_O_TRUNC)))
        return -EROFS;
    cardfile_t *cf = malloc(sizeof *cf);
    if (!cf)
        return -ENOMEM;
    cf->card = b->priv;
    int r = card_lookup(cf->card, rel, &cf->node);
    if (r < 0) {
        free(cf);
        return r;
    }
    if (cf->node.is_dir) {
        free(cf);
        return -EISDIR;
    }
    fs_file_t *f = fs_file_new(&cardfile_ops, gflags);
    if (!f) {
        free(cf);
        return -ENOMEM;
    }
    f->priv = cf;
    f->backend = b;
    *out = f;
    return 0;
}

static int cf_readdir(fs_backend_t *b, const char *rel, uint64_t cookie, fs_dirent_t *out)
{
    card_node_t n;
    int r = card_lookup(b->priv, rel, &n);
    if (r < 0)
        return r;
    if (cookie > 0xFFFFFFFFu)
        return 0;
    card_dirent_t d;
    r = card_readdir(b->priv, &n, (uint32_t)cookie, &d);
    if (r <= 0)
        return r;
    memset(out, 0, sizeof *out);
    memcpy(out->name, d.name, sizeof d.name);
    out->ino = d.ino;
    out->off = d.off;
    out->type = d.is_dir ? GUEST_DT_DIR : GUEST_DT_REG;
    return 1;
}

static int cf_rofs(fs_backend_t *b, const char *rel) { (void)b; (void)rel; return -EROFS; }
static int cf_rofs_mode(fs_backend_t *b, const char *rel, uint32_t mode) { (void)b; (void)rel; (void)mode; return -EROFS; }
static int cf_rofs2(fs_backend_t *b, const char *a, const char *c) { (void)b; (void)a; (void)c; return -EROFS; }
static int cf_rofs_size(fs_backend_t *b, const char *rel, int64_t size) { (void)b; (void)rel; (void)size; return -EROFS; }

static int cf_statfs(fs_backend_t *b, const char *rel, fs_statfs_t *out)
{
    (void)rel;
    card_statfs_t s;
    card_statfs(b->priv, &s);
    memset(out, 0, sizeof *out);
    out->type = s.type;
    out->bsize = s.bsize;
    out->frsize = s.frsize;
    out->blocks = s.blocks;
    out->bfree = s.bfree;
    out->bavail = s.bavail;
    out->files = s.files;
    out->ffree = s.ffree;
    out->namelen = s.namelen;
    return 0;
}

static void cf_destroy(fs_backend_t *b)
{
    card_close(b->priv);
    free(b);
}

static const fs_backend_ops_t cardfs_ops = {
    .stat = cf_stat, .open = cf_open, .readdir = cf_readdir, .unlink = cf_rofs, .mkdir = cf_rofs_mode,
    .rmdir = cf_rofs, .rename = cf_rofs2, .truncate = cf_rofs_size, .chmod = cf_rofs_mode, .statfs = cf_statfs, .destroy = cf_destroy,
};

fs_backend_t *fs_backend_card(card_t *card)
{
    fs_backend_t *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->ops = &cardfs_ops;
    b->readonly = true;
    b->priv = card;
    card_stat_t cs;
    card_node_t root;
    card_root(card, &root);
    card_stat(card, &root, &cs);
    b->dev = cs.dev;
    return b;
}

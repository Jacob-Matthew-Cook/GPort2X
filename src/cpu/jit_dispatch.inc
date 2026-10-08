/* JIT block lookup (docs/JIT.md), included by cpu.c after a backend. */

/* The block starting at pc on the decoded page dp, translating it if needed. */
static struct jblock *jit_get(cpu_t *c, struct dpage *dp, gaddr_t pc)
{
    if (dp->nojit || !jit_init())
        return NULL;
    const uint8_t *hp = dp->host + (pc & (GP2X_PAGE_SIZE - 1u));
    if (c->jepoch != jit_epoch) {
        memset(c->jcache, 0, sizeof c->jcache);
        c->jepoch = jit_epoch;
    }
    struct jblock **slot = &c->jcache[(pc >> 2) & (JCACHE_SIZE - 1u)];
    struct jblock *b = *slot;
    if (b && b->pc == pc && b->host == hp)
        return b;
    for (b = jit_hash[jit_bucket(pc, hp)]; b; b = b->next)
        if (b->pc == pc && b->host == hp)
            break;
    if (!b) {
        /* Translate only code that has run JIT_HOT_THRESHOLD times (tiered
         * execution): start-up paths and the firmware's short-lived processes
         * stay in the interpreter instead of paying for compilation. */
        uint8_t *hot = &c->jhot[(pc >> 2) & (JCACHE_SIZE - 1u)];
        if (*hot < jit_hot) {
            (*hot)++;
            return NULL;
        }
        *hot = 0;
        b = jit_translate(c, pc, dp->host);
        if (c->jepoch != jit_epoch) { /* the translation flushed everything */
            memset(c->jcache, 0, sizeof c->jcache);
            c->jepoch = jit_epoch;
        }
        if (!b) {
            dp->nojit = 1;
            return NULL;
        }
    }
    c->jcache[(pc >> 2) & (JCACHE_SIZE - 1u)] = b;
    return b;
}


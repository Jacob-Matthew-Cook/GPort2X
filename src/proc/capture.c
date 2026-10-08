/* Call capture (--capture): records real calls of chosen functions so a
 * reimplementation can be checked against them on the game's own data.
 *
 * At a capture address (a breakpoint, in tasks whose program path contains
 * capture_exe) a record starts: the entry registers, then a memory watch on
 * the task's cpu keeps, per byte, the value of its FIRST read when nothing
 * in the call had written it yet (the call's inputs, callees' reads
 * included) and the LAST value written (its outputs); syscalls are listed
 * with their results. The record ends when the task returns to the entry's
 * lr with the entry's sp (a breakpoint there): r0/r1 are its results. A call
 * that runs past capture_budget instructions, execs or leaves another way is
 * written as incomplete. Records nest: a call of another chosen function
 * inside one being recorded starts its own record (a never-returning caller
 * such as the main loop must not hide its callees); every access and syscall
 * goes to every open record of the task. One JSON object per line:
 *
 *   {"addr":A,"seq":K,"pid":P,"flip":F,"insns":N,"complete":true,
 *    "regs":[r0..r15],"cpsr":C,"ret":[r0,r1],
 *    "svcs":[[imm,r7,r0_in,r0_out,[[addr,"hexbytes"],...]],...],
 *    "reads":[[addr,"hexbytes"],...],"writes":[[addr,"hexbytes"],...]}
 *
 * A syscall's own stores into guest memory (a read()'s data, a stat buffer)
 * are listed with it: they are not the call's writes, they are inputs that
 * arrive at that point (a chunked read refills one buffer).
 *
 * Accesses are watched in the interpreter only, so a run with captures
 * turns the JIT off. Instruction fetches are not inputs (code comes from
 * the image); a store into code shows as a write. */
#include "task.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gport2x/log.h"

#define CAP_PAGE 4096u
#define F_READ 1u  /* its first access was a read: val_r is an input */
#define F_WRITE 2u /* written: val_w is the last value stored */

typedef struct cpage {
    uint32_t page;
    uint8_t flags[CAP_PAGE], val_r[CAP_PAGE], val_w[CAP_PAGE];
} cpage_t;

typedef struct crec {
    struct crec *next;        /* the task's open records, innermost first */
    gtask_t *task;
    cpu_t *cpu;
    unsigned site;            /* index into capture.addrs */
    uint32_t regs[16], cpsr;
    gaddr_t ret_pc, entry_sp;
    uint64_t insns0, flip;
    cpage_t **pages;          /* open addressing on page number */
    size_t npages, cap_pages;
    uint32_t (*svcs)[4];
    struct svcout { uint8_t *buf; size_t len, cap; } *out; /* per syscall: [addr u32][n u32][n bytes]... */
    size_t nsvcs, cap_svcs;
} crec_t;

struct capture {
    gaddr_t *addrs;
    unsigned *seen, *written;
    unsigned n, max, skip;
    uint64_t budget;
    char exe[256];
    FILE *out;
};

static cpage_t *page_get(crec_t *r, uint32_t page)
{
    if (r->npages * 2 >= r->cap_pages) {
        size_t nc = r->cap_pages ? r->cap_pages * 2 : 64;
        cpage_t **np = calloc(nc, sizeof *np);
        if (!np)
            return NULL;
        for (size_t i = 0; i < r->cap_pages; i++)
            if (r->pages[i]) {
                size_t h = (r->pages[i]->page * 2654435761u) & (nc - 1);
                while (np[h])
                    h = (h + 1) & (nc - 1);
                np[h] = r->pages[i];
            }
        free(r->pages);
        r->pages = np;
        r->cap_pages = nc;
    }
    size_t h = (page * 2654435761u) & (r->cap_pages - 1);
    while (r->pages[h]) {
        if (r->pages[h]->page == page)
            return r->pages[h];
        h = (h + 1) & (r->cap_pages - 1);
    }
    cpage_t *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->page = page;
    r->pages[h] = p;
    r->npages++;
    return p;
}

static void record_access(crec_t *r, gaddr_t addr, unsigned size, uint32_t value, bool write)
{
    for (unsigned i = 0; i < size; i++) {
        gaddr_t a = addr + i;
        cpage_t *p = page_get(r, a / CAP_PAGE);
        if (!p)
            return;
        unsigned o = a % CAP_PAGE;
        uint8_t b = (uint8_t)(value >> (8 * i));
        if (write) {
            p->val_w[o] = b;
            p->flags[o] |= F_WRITE;
        } else if (!p->flags[o]) {
            p->val_r[o] = b;
            p->flags[o] = F_READ;
        }
    }
}

static void on_access(void *ctx, gaddr_t addr, unsigned size, uint32_t value, bool write)
{
    for (crec_t *r = ((gtask_t *)ctx)->cap_rec; r; r = r->next)
        record_access(r, addr, size, value, write);
}

static int page_cmp(const void *a, const void *b)
{
    uint32_t x = (*(cpage_t *const *)a)->page, y = (*(cpage_t *const *)b)->page;
    return x < y ? -1 : x > y;
}

/* "[[addr,"hex"],...]" for the bytes with flag f, contiguous runs merged. */
static void write_runs(FILE *o, cpage_t **pages, size_t n, uint8_t f, bool wvals)
{
    fputc('[', o);
    bool first = true, open = false;
    gaddr_t next = 0;
    for (size_t k = 0; k < n; k++) {
        const cpage_t *p = pages[k];
        for (unsigned i = 0; i < CAP_PAGE; i++) {
            gaddr_t a = p->page * CAP_PAGE + i;
            bool on = (p->flags[i] & f) != 0;
            if (on && open && a != next) {
                fputs("\"]", o);
                open = false;
            }
            if (on && !open) {
                fprintf(o, "%s[%" PRIu32 ",\"", first ? "" : ",", a);
                first = false;
                open = true;
            }
            if (on) {
                fprintf(o, "%02x", wvals ? p->val_w[i] : p->val_r[i]);
                next = a + 1;
            }
        }
    }
    if (open)
        fputs("\"]", o);
    fputc(']', o);
}

static void rec_free(crec_t *r)
{
    for (size_t i = 0; i < r->cap_pages; i++)
        free(r->pages[i]);
    free(r->pages);
    for (size_t i = 0; i < r->nsvcs; i++)
        free(r->out[i].buf);
    free(r->svcs);
    free(r->out);
    free(r);
}

static void finish(gsys_t *s, gtask_t *t, crec_t *r, bool complete)
{
    struct capture *c = s->cap;
    for (crec_t **pp = &t->cap_rec; *pp; pp = &(*pp)->next)
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    if (r->cpu == t->cpu) {
        if (!t->cap_rec)
            cpu_set_watch(r->cpu, NULL, NULL);
        bool keep = false; /* another site, or another open record, still needs this breakpoint */
        for (unsigned i = 0; i < c->n; i++)
            keep |= c->addrs[i] == r->ret_pc;
        for (crec_t *o = t->cap_rec; o; o = o->next)
            keep |= o->ret_pc == r->ret_pc;
        if (!keep)
            cpu_remove_breakpoint(r->cpu, r->ret_pc);
    }
    cpage_t **pages = malloc((r->npages ? r->npages : 1) * sizeof *pages);
    size_t n = 0;
    for (size_t i = 0; i < r->cap_pages && pages; i++)
        if (r->pages[i])
            pages[n++] = r->pages[i];
    if (pages)
        qsort(pages, n, sizeof *pages, page_cmp);
    const cpu_regs_t *now = cpu_regs(t->cpu);
    FILE *o = c->out;
    fprintf(o, "{\"addr\":%" PRIu32 ",\"seq\":%u,\"pid\":%d,\"flip\":%" PRIu64 ",\"insns\":%" PRIu64
               ",\"complete\":%s,\"regs\":[",
            c->addrs[r->site], c->seen[r->site] - 1, t->pid, r->flip,
            cpu_insn_count(t->cpu) - r->insns0, complete ? "true" : "false");
    for (int i = 0; i < 16; i++)
        fprintf(o, "%s%" PRIu32, i ? "," : "", r->regs[i]);
    fprintf(o, "],\"cpsr\":%" PRIu32 ",\"ret\":[%" PRIu32 ",%" PRIu32 "],\"svcs\":[", r->cpsr,
            complete ? now->r[0] : 0, complete ? now->r[1] : 0);
    for (size_t i = 0; i < r->nsvcs; i++) {
        fprintf(o, "%s[%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",[", i ? "," : "", r->svcs[i][0],
                r->svcs[i][1], r->svcs[i][2], r->svcs[i][3]);
        const struct svcout *so = &r->out[i];
        for (size_t k = 0; k + 8 <= so->len;) {
            uint32_t a, len;
            memcpy(&a, so->buf + k, 4);
            memcpy(&len, so->buf + k + 4, 4);
            fprintf(o, "%s[%" PRIu32 ",\"", k ? "," : "", a);
            for (uint32_t j = 0; j < len; j++)
                fprintf(o, "%02x", so->buf[k + 8 + j]);
            fputs("\"]", o);
            k += 8 + len;
        }
        fputs("]]", o);
    }
    fputs("],\"reads\":", o);
    if (pages)
        write_runs(o, pages, n, F_READ, false);
    fputs(",\"writes\":", o);
    if (pages)
        write_runs(o, pages, n, F_WRITE, true);
    fputs("}\n", o);
    fflush(o);
    c->written[r->site]++;
    gp_info("capture: %08x call %u (pid %d): %s, %" PRIu64 " instructions, %zu pages", c->addrs[r->site],
            c->seen[r->site] - 1, t->pid, complete ? "complete" : "incomplete", cpu_insn_count(t->cpu) - r->insns0,
            r->npages);
    free(pages);
    rec_free(r);
}

int capture_create(gsys_t *s)
{
    const gsys_config_t *cfg = &s->cfg;
    if (!cfg->capture)
        return 0;
    struct capture *c = calloc(1, sizeof *c);
    if (!c)
        return -ENOMEM;
    for (const char *p = cfg->capture; *p;) {
        char *end;
        unsigned long a = strtoul(p, &end, 0);
        if (end == p || (*end && *end != ','))
            break;
        gaddr_t *na = realloc(c->addrs, (c->n + 1) * sizeof *na);
        if (!na)
            return -ENOMEM;
        c->addrs = na;
        c->addrs[c->n++] = (gaddr_t)a;
        p = *end ? end + 1 : end;
    }
    c->seen = calloc(c->n ? c->n : 1, sizeof *c->seen);
    c->written = calloc(c->n ? c->n : 1, sizeof *c->written);
    c->max = cfg->capture_max ? cfg->capture_max : 3;
    c->skip = cfg->capture_skip;
    c->budget = cfg->capture_budget ? cfg->capture_budget : 200000000ull;
    snprintf(c->exe, sizeof c->exe, "%s", cfg->capture_exe ? cfg->capture_exe : "");
    c->out = fopen(cfg->capture_out ? cfg->capture_out : "capture.jsonl", "w");
    if (!c->out || !c->seen || !c->written || !c->n) {
        gp_error("capture: bad --capture list or cannot write %s", cfg->capture_out ? cfg->capture_out : "capture.jsonl");
        return -EINVAL;
    }
    s->cap = c;
    gp_info("capture: %u address(es), %u call(s) each after %u, budget %" PRIu64 " instructions", c->n, c->max,
            c->skip, c->budget);
    return 0;
}

void capture_destroy(gsys_t *s)
{
    struct capture *c = s->cap;
    if (!c)
        return;
    for (gtask_t *t = s->tasks; t; t = t->next)
        while (t->cap_rec)
            finish(s, t, t->cap_rec, false);
    for (unsigned i = 0; i < c->n; i++)
        gp_info("capture: %08x: %u call(s) seen, %u recorded", c->addrs[i], c->seen[i], c->written[i]);
    fclose(c->out);
    free(c->addrs);
    free(c->seen);
    free(c->written);
    free(c);
    s->cap = NULL;
}

void capture_before_slice(gsys_t *s, gtask_t *t)
{
    struct capture *c = s->cap;
    while (t->cap_rec && t->cap_rec->cpu != t->cpu) /* exec replaced the image mid-call */
        finish(s, t, t->cap_rec, false);
    if (t->cap_armed == t->cpu)
        return;
    t->cap_armed = t->cpu;
    if (c->exe[0] && (!t->mm || !strstr(t->mm->exe, c->exe)))
        return;
    for (unsigned i = 0; i < c->n; i++)
        cpu_add_breakpoint(t->cpu, c->addrs[i]);
}

void capture_after_slice(gsys_t *s, gtask_t *t)
{
    for (crec_t *r = t->cap_rec, *next; r; r = next) {
        next = r->next;
        if (t->cpu != r->cpu || cpu_insn_count(t->cpu) - r->insns0 > s->cap->budget ||
            t->state == TASK_ZOMBIE || t->state == TASK_DEAD)
            finish(s, t, r, false);
    }
}

bool capture_breakpoint(gsys_t *s, gtask_t *t, gaddr_t pc)
{
    struct capture *c = s->cap;
    const cpu_regs_t *g = cpu_regs(t->cpu);
    crec_t *r;
    bool ret_hit = false;
    for (crec_t *o = t->cap_rec, *next; o; o = next) {
        next = o->next;
        if (pc == o->ret_pc && g->r[13] == o->entry_sp) {
            finish(s, t, o, true);
            ret_hit = true;
        }
    }
    unsigned site = c->n;
    for (unsigned i = 0; i < c->n; i++)
        if (c->addrs[i] == pc)
            site = i;
    if (site == c->n)
        return ret_hit || t->cap_rec != NULL; /* a return breakpoint (possibly a nested frame's) */
    for (crec_t *o = t->cap_rec; o; o = o->next)
        if (o->site == site) /* recursion: the outer call's record covers it */
            return true;
    unsigned k = c->seen[site]++;
    if (k < c->skip || c->written[site] >= c->max)
        return true;
    r = calloc(1, sizeof *r);
    if (!r)
        return true;
    r->task = t;
    r->cpu = t->cpu;
    r->site = site;
    memcpy(r->regs, g->r, sizeof r->regs);
    r->regs[15] = pc;
    r->cpsr = g->cpsr;
    r->ret_pc = g->r[14] & ~1u;
    r->entry_sp = g->r[13];
    r->insns0 = cpu_insn_count(t->cpu);
    r->flip = s->dev ? gpdev_flip_count(s->dev) : 0;
    r->next = t->cap_rec;
    t->cap_rec = r;
    cpu_add_breakpoint(t->cpu, r->ret_pc);
    cpu_set_watch(t->cpu, on_access, t);
    return true;
}

static void record_svc(crec_t *r, gtask_t *t, uint32_t imm, bool after);

/* The syscall layer's stores into the capturing task's memory, during one
 * of its syscalls: appended to that syscall in every open record. */
static void on_kernel_write(void *ctx, gmem_t *m, gaddr_t addr, const void *src, uint32_t len)
{
    gtask_t *t = ctx;
    if (!t->mm || m != t->mm->mem || !len)
        return;
    for (crec_t *r = t->cap_rec; r; r = r->next) {
        if (!r->nsvcs)
            continue;
        struct svcout *so = &r->out[r->nsvcs - 1];
        if (so->len + 8 + len > so->cap) {
            size_t nc = so->cap ? so->cap : 256;
            while (nc < so->len + 8 + len)
                nc *= 2;
            uint8_t *nb = realloc(so->buf, nc);
            if (!nb)
                return;
            so->buf = nb;
            so->cap = nc;
        }
        memcpy(so->buf + so->len, &addr, 4);
        memcpy(so->buf + so->len + 4, &len, 4);
        memcpy(so->buf + so->len + 8, src, len);
        so->len += 8 + len;
    }
}

void capture_svc(gtask_t *t, uint32_t imm, bool after)
{
    for (crec_t *r = t->cap_rec; r; r = r->next)
        record_svc(r, t, imm, after);
    gmem_set_write_observer(after ? NULL : on_kernel_write, after ? NULL : t);
}

static void record_svc(crec_t *r, gtask_t *t, uint32_t imm, bool after)
{
    const cpu_regs_t *g = cpu_regs(t->cpu);
    if (!after) {
        if (r->nsvcs == r->cap_svcs) {
            size_t nc = r->cap_svcs ? r->cap_svcs * 2 : 8;
            uint32_t (*ns)[4] = realloc(r->svcs, nc * sizeof *ns);
            if (!ns)
                return;
            r->svcs = ns;
            struct svcout *no = realloc(r->out, nc * sizeof *no);
            if (!no)
                return;
            r->out = no;
            r->cap_svcs = nc;
        }
        r->out[r->nsvcs] = (struct svcout){ NULL, 0, 0 };
        r->svcs[r->nsvcs][0] = imm;
        r->svcs[r->nsvcs][1] = g->r[7];
        r->svcs[r->nsvcs][2] = g->r[0];
        r->svcs[r->nsvcs][3] = 0;
        r->nsvcs++;
    } else if (r->nsvcs) {
        r->svcs[r->nsvcs - 1][3] = g->r[0];
    }
}

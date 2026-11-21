// FAT32 on the SD card's first partition, linked into the tree at /boot.
// Reads long (VFAT) names; creates 8.3 names only. Write-through, one cached
// data sector and one cached FAT sector.
// shortcut: inodes are cached per directory entry and never evicted; fine for a few thousand files touched per boot.
#include "kernel.h"
#include "mm.h"
#include "sd.h"
#include "vfs.h"

#define EOC       0x0FFFFFF8u   // cluster values >= this end a chain
#define ATTR_RO   0x01
#define ATTR_VOL  0x08
#define ATTR_DIR  0x10
#define ATTR_ARCH 0x20
#define ATTR_LFN  0x0F
#define NT_LOWER_BASE 0x08
#define NT_LOWER_EXT  0x10

static struct {
    uint32_t spc, fat_start, fat_size, nfats, data_start, root_clus, nclus, hint;
} fs;

// Per-inode state. loc_lba/loc_off = where its 32-byte entry lives (0 = root
// or unlinked). old_* = previous entry during rename (link before unlink).
struct fnode {
    struct inode *ino;
    uint32_t clus;
    uint32_t loc_lba, old_lba;
    uint16_t loc_off, old_off;
    uint32_t hint_idx, hint_clus;   // last cluster index -> cluster resolved
    struct fnode *next;
};
static struct fnode *nodes;
static const struct inode_ops file_ops, dir_ops;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

// ---- sector cache ----

static uint8_t sec[512] __attribute__((aligned(4)));
static uint32_t sec_lba = ~0u;
static uint8_t fatsec[512] __attribute__((aligned(4)));
static uint32_t fatsec_lba = ~0u;

static uint8_t *bread(uint32_t lba)
{
    if (sec_lba != lba) {
        sec_lba = ~0u;
        if (sd_read(lba, sec) < 0)
            return NULL;
        sec_lba = lba;
    }
    return sec;
}

static int bwrite(void) { return sd_write(sec_lba, sec); }

static uint32_t fat_get(uint32_t c)
{
    uint32_t lba = fs.fat_start + c / 128;
    if (fatsec_lba != lba) {
        fatsec_lba = ~0u;
        if (sd_read(lba, fatsec) < 0)
            return EOC;     // I/O error reads as end of chain
        fatsec_lba = lba;
    }
    return rd32(fatsec + c % 128 * 4) & 0x0FFFFFFF;
}

static int fat_set(uint32_t c, uint32_t v)
{
    fat_get(c);
    if (fatsec_lba == ~0u)
        return -EIO;
    uint8_t *p = fatsec + c % 128 * 4;
    wr32(p, (rd32(p) & 0xF0000000) | (v & 0x0FFFFFFF));
    for (uint32_t i = 0; i < fs.nfats; i++)
        if (sd_write(fatsec_lba + i * fs.fat_size, fatsec) < 0)
            return -EIO;
    return 0;
}

static uint32_t clus_lba(uint32_t c) { return fs.data_start + (c - 2) * fs.spc; }

// Allocate a cluster, append it after prev (0 = new chain). Returns 0 if full.
static uint32_t alloc_clus(uint32_t prev, int zero)
{
    for (uint32_t n = 0; n < fs.nclus; n++) {
        uint32_t c = 2 + (fs.hint - 2 + n) % fs.nclus;
        if (fat_get(c) != 0)
            continue;
        if (fat_set(c, 0x0FFFFFFF) < 0 || (prev && fat_set(prev, c) < 0))
            return 0;
        fs.hint = c + 1 < fs.nclus + 2 ? c + 1 : 2;
        if (zero) {
            static const uint8_t z[512] __attribute__((aligned(4)));
            for (uint32_t s = 0; s < fs.spc; s++)
                if (sd_write(clus_lba(c) + s, z) < 0)
                    return 0;
            if (sec_lba - clus_lba(c) < fs.spc)
                sec_lba = ~0u;
        }
        return c;
    }
    return 0;
}

static void free_chain(uint32_t c)
{
    while (c >= 2 && c < EOC) {
        uint32_t n = fat_get(c);
        if (fat_set(c, 0) < 0)
            return;
        c = n;
    }
}

// ---- directory entries ----

struct it {                 // raw 32-byte slot iterator over a directory chain
    uint32_t clus, last, sec, ent, lba;
    int started;
};

static uint8_t *it_next(struct it *it)
{
    if (!it->started) {
        it->started = 1;
        it->sec = it->ent = 0;
    } else if (++it->ent == 16) {
        it->ent = 0;
        if (++it->sec == fs.spc) {
            it->sec = 0;
            it->last = it->clus;
            it->clus = fat_get(it->clus);
        }
    }
    if (it->clus < 2 || it->clus >= EOC)
        return NULL;
    it->last = it->clus;
    it->lba = clus_lba(it->clus) + it->sec;
    uint8_t *s = bread(it->lba);
    return s ? s + it->ent * 32 : NULL;
}

struct ent {
    char name[NAME_MAX + 1];
    uint8_t attr;
    uint32_t clus, size, lba;
    uint16_t off;
    int nlfn;
    uint32_t lfn_lba[8];
    uint16_t lfn_off[8];
};

static uint8_t lfn_sum(const uint8_t *e)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

// Next real entry (skips free slots, volume label, "." and ".."). 1 = got one, 0 = end.
static int next_ent(struct it *it, struct ent *out)
{
    char lfn[8 * 13 + 1];
    int have_lfn = 0;
    uint8_t sum = 0;
    out->nlfn = 0;
    uint8_t *e;
    while ((e = it_next(it))) {
        if (e[0] == 0)
            return 0;
        if (e[0] == 0xE5) {
            have_lfn = 0, out->nlfn = 0;
            continue;
        }
        if (e[11] == ATTR_LFN) {
            int seq = e[0] & 0x1F;
            if ((e[0] & 0x40) || !have_lfn) {   // last-in-sequence slot comes first
                memset(lfn, 0, sizeof lfn);
                have_lfn = 1, out->nlfn = 0, sum = e[13];
            }
            if (seq < 1 || seq > 8 || e[13] != sum) {
                have_lfn = 0;                    // too long for NAME_MAX, or corrupt
                continue;
            }
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int i = 0; i < 13; i++) {
                uint16_t ch = rd16(e + pos[i]);
                lfn[(seq - 1) * 13 + i] = ch == 0xFFFF ? 0 : ch < 0x80 ? (char)ch : '?';
            }
            out->lfn_lba[out->nlfn] = it->lba;
            out->lfn_off[out->nlfn++] = (uint16_t)(it->ent * 32);
            continue;
        }
        if (e[11] & ATTR_VOL) {
            have_lfn = 0, out->nlfn = 0;
            continue;
        }
        out->attr = e[11];
        out->clus = (uint32_t)rd16(e + 20) << 16 | rd16(e + 26);
        out->size = rd32(e + 28);
        out->lba = it->lba;
        out->off = (uint16_t)(it->ent * 32);
        if (have_lfn && sum == lfn_sum(e) && lfn[0] && strlen(lfn) <= NAME_MAX) {
            memcpy(out->name, lfn, strlen(lfn) + 1);
        } else {
            out->nlfn = 0;
            int n = 0;
            for (int i = 0; i < 8 && e[i] != ' '; i++) {
                char c = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
                out->name[n++] = (e[12] & NT_LOWER_BASE) ? lower(c) : c;
            }
            if (e[8] != ' ')
                out->name[n++] = '.';
            for (int i = 8; i < 11 && e[i] != ' '; i++)
                out->name[n++] = (e[12] & NT_LOWER_EXT) ? lower((char)e[i]) : (char)e[i];
            out->name[n] = 0;
        }
        have_lfn = 0;
        if (strcmp(out->name, ".") && strcmp(out->name, ".."))
            return 1;
        out->nlfn = 0;
    }
    return 0;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return lower(*a) == lower(*b);
}

static struct fnode *FN(struct inode *ino) { return ino->priv; }

static void it_start(struct it *it, struct inode *dir)
{
    memset(it, 0, sizeof *it);
    it->clus = FN(dir)->clus;
}

static struct inode *new_node(uint8_t attr, uint32_t clus, uint32_t size, uint32_t lba, uint16_t off)
{
    struct fnode *f = kmalloc(sizeof *f);
    uint32_t mode = (attr & ATTR_DIR) ? S_IFDIR | 0755 : S_IFREG | ((attr & ATTR_RO) ? 0444 : 0644);
    struct inode *ino = f ? inode_new(mode, (attr & ATTR_DIR) ? &dir_ops : &file_ops) : NULL;
    if (!ino) {
        kfree(f);
        return NULL;
    }
    f->ino = ino;
    f->clus = clus;
    f->loc_lba = lba;
    f->loc_off = off;
    f->next = nodes;
    nodes = f;
    ino->priv = f;
    ino->nlink = 1;
    ino->size = (attr & ATTR_DIR) ? 0 : size;
    return ino;
}

// Cached inode for an entry, created on first sight.
static struct inode *node_for(struct inode *dir, struct ent *e)
{
    struct fnode *f = nodes;
    while (f && !(f->loc_lba == e->lba && f->loc_off == e->off))
        f = f->next;
    struct inode *ino = f ? f->ino : new_node(e->attr, e->clus, e->size, e->lba, e->off);
    if (ino && S_ISDIR(ino->mode))
        ino->parent = dir;
    return ino;
}

static long find(struct inode *dir, const char *name, struct ent *e)
{
    struct it it;
    it_start(&it, dir);
    while (next_ent(&it, e))
        if (name_eq(e->name, name))
            return 0;
    return -ENOENT;
}

// Rewrite an inode's entry (first cluster, size). Root has no entry.
static long sync_ent(struct fnode *f)
{
    if (!f->loc_lba)
        return 0;
    uint8_t *s = bread(f->loc_lba);
    if (!s)
        return -EIO;
    uint8_t *e = s + f->loc_off;
    wr16(e + 20, f->clus >> 16);
    wr16(e + 26, f->clus);
    wr32(e + 28, (uint32_t)f->ino->size);
    return bwrite() < 0 ? -EIO : 0;
}

// ---- files ----

// Cluster holding cluster-index idx of f's chain; allocates if asked. 0 = none.
static uint32_t clus_at(struct fnode *f, uint32_t idx, int alloc)
{
    if (!f->clus) {
        if (!alloc || !(f->clus = alloc_clus(0, 0)))
            return 0;
        f->hint_idx = 0, f->hint_clus = f->clus;
    }
    uint32_t i = 0, c = f->clus;
    if (f->hint_clus && f->hint_idx <= idx)
        i = f->hint_idx, c = f->hint_clus;
    for (; i < idx; i++) {
        uint32_t n = fat_get(c);
        if (n >= EOC && (!alloc || !(n = alloc_clus(c, 0))))
            return 0;
        if (n < 2)
            return 0;   // corrupt chain
        c = n;
    }
    f->hint_idx = idx, f->hint_clus = c;
    return c;
}

static long file_read(struct inode *ino, char *buf, size_t len, uint64_t off)
{
    struct fnode *f = FN(ino);
    if (off >= ino->size)
        return 0;
    if (len > ino->size - off)
        len = ino->size - off;
    uint32_t csize = fs.spc * 512;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t c = clus_at(f, (uint32_t)(pos / csize), 0);
        uint8_t *s = c ? bread(clus_lba(c) + (uint32_t)(pos % csize) / 512) : NULL;
        if (!s)
            break;
        size_t n = 512 - pos % 512;
        if (n > len - done)
            n = len - done;
        memcpy(buf + done, s + pos % 512, n);
        done += n;
    }
    return done ? (long)done : -EIO;
}

static long file_write(struct inode *ino, const char *buf, size_t len, uint64_t off)
{
    struct fnode *f = FN(ino);
    if (off + len > 0xFFFFFFFFu)
        return -EFBIG;
    if (off > ino->size) {      // fill the gap with zeros: clusters hold stale data
        static const char z[512];
        while (ino->size < off) {
            uint64_t n = off - ino->size < sizeof z ? off - ino->size : sizeof z;
            long r = file_write(ino, z, n, ino->size);
            if (r <= 0)
                return r ? r : -ENOSPC;
        }
    }
    uint32_t csize = fs.spc * 512, first = f->clus;
    size_t done = 0;
    long err = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t c = clus_at(f, (uint32_t)(pos / csize), 1);
        if (!c) {
            err = -ENOSPC;
            break;
        }
        uint8_t *s = bread(clus_lba(c) + (uint32_t)(pos % csize) / 512);
        size_t n = 512 - pos % 512;
        if (n > len - done)
            n = len - done;
        if (!s) {
            err = -EIO;
            break;
        }
        memcpy(s + pos % 512, buf + done, n);
        if (bwrite() < 0) {
            sec_lba = ~0u;
            err = -EIO;
            break;
        }
        done += n;
    }
    if (off + done > ino->size)
        ino->size = off + done;
    if ((done || f->clus != first) && sync_ent(f) < 0 && !err)
        err = -EIO;
    return done ? (long)done : err;
}

static void file_truncate(struct inode *ino)
{
    struct fnode *f = FN(ino);
    free_chain(f->clus);
    f->clus = f->hint_clus = f->hint_idx = 0;
    ino->size = 0;
    sync_ent(f);
}

// Last reference to an unlinked file or directory: free its clusters.
static void node_release(struct inode *ino)
{
    struct fnode *f = FN(ino), **pp = &nodes;
    free_chain(f->clus);
    while (*pp != f)
        pp = &(*pp)->next;
    *pp = f->next;
    kfree(f);
}

static const struct inode_ops file_ops = {
    .read = file_read, .write = file_write, .truncate = file_truncate, .release = node_release,
};

// ---- directories ----

static long dir_lookup(struct inode *dir, const char *name, struct inode **out)
{
    struct ent e;
    long r = find(dir, name, &e);
    if (r < 0)
        return r;
    return (*out = node_for(dir, &e)) ? 0 : -ENOMEM;
}

static long dir_readdir(struct inode *dir, uint64_t idx, struct dirent *out)
{
    struct it it;
    struct ent e;
    it_start(&it, dir);
    while (next_ent(&it, &e))
        if (idx-- == 0) {
            struct inode *ino = node_for(dir, &e);
            if (!ino)
                return 0;
            out->d_ino = ino->ino;
            out->d_mode = ino->mode & S_IFMT;
            memcpy(out->d_name, e.name, strlen(e.name) + 1);
            return 1;
        }
    return 0;
}

static int special_ok(char c)
{
    for (const char *p = "!#$%&'()-@^_`{}~"; *p; p++)
        if (*p == c)
            return 1;
    return 0;
}

// 8.3 name in directory-entry form. All-lowercase parts are stored upper case
// with the NT lowercase flags, as Windows and Linux do.
static long short_name(const char *name, uint8_t out[11], uint8_t *nt)
{
    const char *dot = NULL;
    for (const char *p = name; *p; p++)
        if (*p == '.') {
            if (dot)
                return -EINVAL;
            dot = p;
        }
    size_t blen = dot ? (size_t)(dot - name) : strlen(name), elen = dot ? strlen(dot + 1) : 0;
    if (blen == 0 || blen > 8 || elen > 3 || (dot && elen == 0))
        return -ENAMETOOLONG;
    memset(out, ' ', 11);
    *nt = 0;
    for (int part = 0; part < 2; part++) {
        const char *s = part ? dot + 1 : name;
        size_t n = part ? elen : blen;
        int up = 0, lo = 0;
        for (size_t i = 0; i < n; i++) {
            char c = s[i];
            if (c >= 'a' && c <= 'z')
                lo = 1, c = (char)(c - 32);
            else if (c >= 'A' && c <= 'Z')
                up = 1;
            else if (!((c >= '0' && c <= '9') || special_ok(c)))
                return -EINVAL;
            out[(part ? 8 : 0) + i] = (uint8_t)c;
        }
        if (up && lo)
            return -EINVAL;     // mixed case needs a long name entry
        if (lo)
            *nt |= part ? NT_LOWER_EXT : NT_LOWER_BASE;
    }
    if (out[0] == 0xE5)
        out[0] = 0x05;
    return 0;
}

// Write a new entry into dir. Returns its location via lba/off.
static long add_ent(struct inode *dir, const char *name, uint8_t attr, uint32_t clus,
                    uint32_t size, uint32_t *lba, uint16_t *off)
{
    uint8_t sn[11], nt;
    struct ent e;
    long r = short_name(name, sn, &nt);
    if (r < 0)
        return r;
    if (find(dir, name, &e) == 0)
        return -EEXIST;
    struct it it;
    uint8_t *s;
    it_start(&it, dir);
    while ((s = it_next(&it)) && s[0] != 0 && s[0] != 0xE5)
        ;
    if (!s) {               // directory full: extend by one zeroed cluster
        uint32_t c = alloc_clus(it.last, 1);
        if (!c || !(s = bread(clus_lba(c))))
            return -ENOSPC;
        it.lba = clus_lba(c), it.ent = 0;
    }
    memset(s, 0, 32);
    memcpy(s, sn, 11);
    s[11] = attr;
    s[12] = nt;
    wr16(s + 20, clus >> 16);
    wr16(s + 26, clus);
    wr32(s + 28, size);
    wr16(s + 16, 0x21);     // date 1980-01-01; no RTC
    wr16(s + 24, 0x21);
    *lba = it.lba;
    *off = (uint16_t)(it.ent * 32);
    return bwrite() < 0 ? -EIO : 0;
}

// Point a directory's ".." entry at its (new) parent.
static long set_dotdot(uint32_t clus, uint32_t parent)
{
    uint8_t *s = bread(clus_lba(clus));
    if (!s)
        return -EIO;
    uint32_t p = parent == fs.root_clus ? 0 : parent;   // root is cluster 0 in ".."
    memcpy(s + 32, "..         ", 11);
    s[32 + 11] = ATTR_DIR;
    wr16(s + 32 + 20, p >> 16);
    wr16(s + 32 + 26, p);
    return bwrite() < 0 ? -EIO : 0;
}

static long dir_create(struct inode *dir, const char *name, uint32_t mode, struct inode **out)
{
    int isdir = S_ISDIR(mode);
    uint32_t clus = 0, lba;
    uint16_t off;
    if (isdir) {
        if (!(clus = alloc_clus(0, 1)))
            return -ENOSPC;
        uint8_t *s = bread(clus_lba(clus));
        if (!s)
            return -EIO;
        memcpy(s, ".          ", 11);
        s[11] = ATTR_DIR;
        wr16(s + 20, clus >> 16);
        wr16(s + 26, clus);
        if (bwrite() < 0 || set_dotdot(clus, FN(dir)->clus) < 0) {
            free_chain(clus);
            return -EIO;
        }
    }
    long r = add_ent(dir, name, isdir ? ATTR_DIR : ATTR_ARCH, clus, 0, &lba, &off);
    if (r < 0) {
        free_chain(clus);
        return r;
    }
    struct inode *ino = new_node(isdir ? ATTR_DIR : ATTR_ARCH, clus, 0, lba, off);
    if (!ino)
        return -ENOMEM;     // entry is on disk; next lookup retries
    if (isdir)
        ino->parent = dir;
    *out = ino;
    return 0;
}

// Only used by rename: new entry first, then unlink drops the old one.
static long dir_link(struct inode *dir, const char *name, struct inode *ino)
{
    if (ino->ops != &file_ops && ino->ops != &dir_ops)
        return -EXDEV;
    struct fnode *f = FN(ino);
    uint32_t lba;
    uint16_t off;
    int isdir = S_ISDIR(ino->mode);
    long r = add_ent(dir, name, isdir ? ATTR_DIR : ATTR_ARCH, f->clus, (uint32_t)ino->size, &lba, &off);
    if (r < 0)
        return r;
    if (isdir) {
        set_dotdot(f->clus, FN(dir)->clus);
        ino->parent = dir;
    }
    f->old_lba = f->loc_lba, f->old_off = f->loc_off;
    f->loc_lba = lba, f->loc_off = off;
    ino->nlink++;
    return 0;
}

static long dir_unlink(struct inode *dir, const char *name)
{
    struct ent e;
    long r = find(dir, name, &e);
    if (r < 0)
        return r;
    for (int i = 0; i <= e.nlfn; i++) {
        uint8_t *s = bread(i < e.nlfn ? e.lfn_lba[i] : e.lba);
        if (!s)
            return -EIO;
        s[i < e.nlfn ? e.lfn_off[i] : e.off] = 0xE5;
        if (bwrite() < 0)
            return -EIO;
    }
    for (struct fnode *f = nodes; f; f = f->next) {
        if (f->old_lba == e.lba && f->old_off == e.off) {
            f->old_lba = 0;         // rename: data now owned by the new entry
            f->ino->nlink--;
            return 0;
        }
        if (f->loc_lba == e.lba && f->loc_off == e.off) {
            f->loc_lba = 0;         // clusters freed on last close (node_release)
            f->ino->nlink--;
            return 0;
        }
    }
    free_chain(e.clus);             // never opened: free now
    return 0;
}

static const struct inode_ops dir_ops = {
    .lookup = dir_lookup, .create = dir_create, .link = dir_link,
    .unlink = dir_unlink, .readdir = dir_readdir, .release = node_release,
};

// ---- mount ----

static long mount_at(uint32_t part)
{
    uint8_t *b = bread(part);
    if (!b || rd16(b + 11) != 512 || !b[13] || rd16(b + 22) != 0 || !b[16])
        return -EINVAL;     // not FAT32 with 512-byte sectors
    fs.spc = b[13];
    fs.nfats = b[16];
    fs.fat_size = rd32(b + 36);
    fs.fat_start = part + rd16(b + 14);
    fs.data_start = fs.fat_start + fs.nfats * fs.fat_size;
    fs.root_clus = rd32(b + 44);
    fs.nclus = (rd32(b + 32) - (fs.data_start - part)) / fs.spc;
    fs.hint = 2;
    uint32_t info = part + rd16(b + 48);
    // FSInfo: mark the free count unknown (we don't maintain it); reuse its hint.
    if ((b = bread(info)) && rd32(b) == 0x41615252 && rd32(b + 484) == 0x61417272) {
        uint32_t h = rd32(b + 492);
        if (h >= 2 && h < fs.nclus + 2)
            fs.hint = h;
        wr32(b + 488, 0xFFFFFFFF);
        bwrite();
    }
    return 0;
}

void fat_mount_boot(void)
{
    if (sd_init() < 0)
        return;
    uint8_t *b = bread(0);
    if (!b) {
        kprintf("[FAT] cannot read sector 0\n");
        return;
    }
    // MBR partition 1 (types 0x0B/0x0C = FAT32), else a partitionless volume.
    uint32_t part = 0;
    if (rd16(b + 510) == 0xAA55 && (b[446 + 4] == 0x0B || b[446 + 4] == 0x0C))
        part = rd32(b + 446 + 8);
    if (mount_at(part) < 0) {
        kprintf("[FAT] no FAT32 filesystem on the SD card\n");
        return;
    }
    struct inode *root = new_node(ATTR_DIR, fs.root_clus, 0, 0, 0);
    if (!root || vfs_root()->ops->link(vfs_root(), "boot", root) < 0)
        panic("fat: cannot attach /boot");
    root->refs = 1;         // pinned forever, like the ramfs root
    kprintf("[FAT] /boot: FAT32 at LBA %u, %u clusters of %u bytes\n",
            part, fs.nclus, fs.spc * 512);
}

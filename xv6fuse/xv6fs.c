/*
 * xv6fs.c – Core xv6 filesystem library for the FUSE driver.
 *
 * Uses pread/pwrite on a raw image file instead of xv6's buf cache.
 * No xv6 logging: we write directly (safe because FUSE mounts single-
 * threaded with -s and we never run QEMU at the same time).
 *
 * Mirrors xv6's kernel/fs.c structure but adapted for user-space Linux.
 */

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#include "xv6fs.h"

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */
int                   g_imgfd = -1;
struct xv6_superblock g_sb;

/* ------------------------------------------------------------------ */
/*  Open / close                                                       */
/* ------------------------------------------------------------------ */
int fs_open(const char *path)
{
    char buf[BSIZE];

    g_imgfd = open(path, O_RDWR);
    if (g_imgfd < 0) return -1;

    /* Superblock lives at block 1 */
    if (pread(g_imgfd, buf, BSIZE, (off_t)BSIZE) != BSIZE) {
        close(g_imgfd); g_imgfd = -1; return -1;
    }
    memcpy(&g_sb, buf, sizeof(g_sb));

    if (g_sb.magic != FSMAGIC) {
        fprintf(stderr, "xv6fs: bad magic 0x%08x (expected 0x%08x)\n",
                g_sb.magic, FSMAGIC);
        close(g_imgfd); g_imgfd = -1; errno = EINVAL; return -1;
    }
    return 0;
}

void fs_close(void)
{
    if (g_imgfd >= 0) {
        fsync(g_imgfd);
        close(g_imgfd);
        g_imgfd = -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Raw block I/O                                                      */
/* ------------------------------------------------------------------ */
int rblock(uint32_t bno, void *buf)
{
    off_t off = (off_t)bno * BSIZE;
    if (pread(g_imgfd, buf, BSIZE, off) != BSIZE) return -EIO;
    return 0;
}

int wblock(uint32_t bno, const void *buf)
{
    off_t off = (off_t)bno * BSIZE;
    if (pwrite(g_imgfd, buf, BSIZE, off) != BSIZE) return -EIO;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Bitmap – balloc / bfree                                           */
/* ------------------------------------------------------------------ */

/*
 * balloc: find the first free bit at or after the first data block
 * (nmeta = g_sb.size - g_sb.nblocks), set it, write the bitmap block,
 * and zero the allocated block.  Returns block number or 0 on failure.
 *
 * xv6 bitmap convention: bit i in the bitmap covers block i.
 * Bit 0 of byte 0 of bitmap block bmapstart covers block 0 (boot block).
 * We start scanning from the first data block index to avoid allocating
 * metadata blocks.
 */
uint32_t balloc(void)
{
    uint8_t  buf[BSIZE];
    uint32_t first_data = g_sb.size - g_sb.nblocks; /* nmeta */

    for (uint32_t b = 0; b < g_sb.size; b += BPB) {
        uint32_t bmap_bno = BBLOCK(b, g_sb);
        if (rblock(bmap_bno, buf) < 0) return 0;

        for (uint32_t bi = 0; bi < BPB && (b + bi) < g_sb.size; bi++) {
            uint32_t block = b + bi;
            if (block < first_data) continue; /* skip metadata */

            uint8_t m = 1u << (bi % 8);
            if (!(buf[bi / 8] & m)) {
                /* free – allocate it */
                buf[bi / 8] |= m;
                wblock(bmap_bno, buf);

                /* zero the new block */
                uint8_t zero[BSIZE];
                memset(zero, 0, BSIZE);
                wblock(block, zero);
                return block;
            }
        }
    }
    return 0; /* no free block */
}

void bfree(uint32_t bno)
{
    uint8_t  buf[BSIZE];
    uint32_t bmap_bno = BBLOCK(bno, g_sb);

    rblock(bmap_bno, buf);
    uint32_t bi = bno % BPB;
    uint8_t  m  = 1u << (bi % 8);
    if (!(buf[bi / 8] & m)) {
        fprintf(stderr, "bfree: block %u already free\n", bno);
        return;
    }
    buf[bi / 8] &= ~m;
    wblock(bmap_bno, buf);
}

/* ------------------------------------------------------------------ */
/*  Inode I/O – iget / iupdate / ialloc / ifree                       */
/* ------------------------------------------------------------------ */

void iget(uint32_t inum, struct xv6_dinode *dip)
{
    uint8_t buf[BSIZE];
    uint32_t bno = IBLOCK(inum, g_sb);
    rblock(bno, buf);
    struct xv6_dinode *src = (struct xv6_dinode *)buf + (inum % IPB);
    memcpy(dip, src, sizeof(*dip));
}

void iupdate(uint32_t inum, const struct xv6_dinode *dip)
{
    uint8_t buf[BSIZE];
    uint32_t bno = IBLOCK(inum, g_sb);
    rblock(bno, buf);
    struct xv6_dinode *dst = (struct xv6_dinode *)buf + (inum % IPB);
    memcpy(dst, dip, sizeof(*dip));
    wblock(bno, buf);
}

uint32_t ialloc(int16_t type)
{
    for (uint32_t inum = 1; inum < g_sb.ninodes; inum++) {
        struct xv6_dinode dip;
        iget(inum, &dip);
        if (dip.type == 0) {
            memset(&dip, 0, sizeof(dip));
            dip.type  = type;
            dip.nlink = 0;
            dip.size  = 0;
            iupdate(inum, &dip);
            return inum;
        }
    }
    return 0; /* no free inode */
}

void ifree(uint32_t inum)
{
    struct xv6_dinode dip;
    iget(inum, &dip);
    memset(&dip, 0, sizeof(dip));
    iupdate(inum, &dip);
}

/* ------------------------------------------------------------------ */
/*  Block map – bmap                                                   */
/* ------------------------------------------------------------------ */

/*
 * bmap: return the disk block address of the fbn-th block in inode dip.
 * If alloc=1, allocate blocks as needed (direct, indirect container,
 * and indirect entries).  Writes dip and inode back when new blocks
 * are allocated.  Returns 0 on error.
 */
uint32_t bmap(struct xv6_dinode *dip, uint32_t inum, uint32_t fbn, int alloc)
{
    if (fbn >= MAXFILE) return 0;

    if (fbn < NDIRECT) {
        if (dip->addrs[fbn] == 0) {
            if (!alloc) return 0;
            uint32_t b = balloc();
            if (!b) return 0;
            dip->addrs[fbn] = b;
            iupdate(inum, dip);
        }
        return dip->addrs[fbn];
    }

    /* Indirect */
    fbn -= NDIRECT;
    if (dip->addrs[NDIRECT] == 0) {
        if (!alloc) return 0;
        uint32_t ib = balloc();
        if (!ib) return 0;
        dip->addrs[NDIRECT] = ib;
        iupdate(inum, dip);
    }

    uint32_t indirect[NINDIRECT];
    rblock(dip->addrs[NDIRECT], indirect);

    if (indirect[fbn] == 0) {
        if (!alloc) return 0;
        uint32_t b = balloc();
        if (!b) return 0;
        indirect[fbn] = b;
        wblock(dip->addrs[NDIRECT], indirect);
    }
    return indirect[fbn];
}

/* ------------------------------------------------------------------ */
/*  File I/O – readi / writei                                          */
/* ------------------------------------------------------------------ */

/*
 * readi: read n bytes at offset off from inode inum into buf.
 * Clamps at file size.  Returns bytes read, or negative errno.
 */
int readi(uint32_t inum, struct xv6_dinode *dip, void *buf, uint32_t off, uint32_t n)
{
    if (off > dip->size) return 0;
    if (off + n > dip->size) n = dip->size - off;

    uint32_t total = 0;
    uint8_t *dst   = (uint8_t *)buf;

    while (total < n) {
        uint32_t fbn = (off + total) / BSIZE;
        uint32_t bno = bmap(dip, inum, fbn, 0);
        if (!bno) {
            /* Hole – fill with zeros (xv6 doesn't create holes,
             * but be safe). */
            uint32_t chunk = BSIZE - (off + total) % BSIZE;
            if (chunk > n - total) chunk = n - total;
            memset(dst + total, 0, chunk);
            total += chunk;
            continue;
        }
        uint8_t  blk[BSIZE];
        rblock(bno, blk);
        uint32_t boff  = (off + total) % BSIZE;
        uint32_t chunk = BSIZE - boff;
        if (chunk > n - total) chunk = n - total;
        memcpy(dst + total, blk + boff, chunk);
        total += chunk;
    }
    return (int)total;
}

/*
 * writei: write n bytes at offset off into inode inum from buf.
 * Allocates blocks as needed.  Returns bytes written, or negative errno.
 */
int writei(uint32_t inum, struct xv6_dinode *dip, const void *buf, uint32_t off, uint32_t n)
{
    if (n == 0) return 0;
    if (off + n > (uint32_t)MAXFILE * BSIZE) return -EFBIG;

    const uint8_t *src   = (const uint8_t *)buf;
    uint32_t       total = 0;

    while (total < n) {
        uint32_t fbn  = (off + total) / BSIZE;
        uint32_t bno  = bmap(dip, inum, fbn, 1);
        if (!bno) return total ? (int)total : -ENOSPC;

        uint8_t  blk[BSIZE];
        uint32_t boff  = (off + total) % BSIZE;
        uint32_t chunk = BSIZE - boff;
        if (chunk > n - total) chunk = n - total;

        /* Read-modify-write for partial blocks */
        if (boff != 0 || chunk < BSIZE) rblock(bno, blk);
        memcpy(blk + boff, src + total, chunk);
        wblock(bno, blk);

        total += chunk;
    }

    if (off + n > dip->size) {
        dip->size = off + n;
        iupdate(inum, dip);
    }
    return (int)total;
}

/* ------------------------------------------------------------------ */
/*  Truncate – itrunc                                                  */
/* ------------------------------------------------------------------ */

/*
 * itrunc: change inode size to new_size.
 *
 * Shrink: free every block beyond new_size; zero the tail of the last
 *         partial block; free the indirect block when no indirect
 *         entries remain.
 * Grow:   zero-fill the gap (xv6 readi cannot handle sparse files; we
 *         allocate and zero every intervening block).
 */
int itrunc(uint32_t inum, struct xv6_dinode *dip, uint32_t new_size)
{
    if (new_size > (uint32_t)MAXFILE * BSIZE) return -EFBIG;

    if (new_size < dip->size) {
        /* --- Shrink ------------------------------------------------ */

        /* Zero the tail of the last remaining partial block */
        if (new_size > 0 && (new_size % BSIZE) != 0) {
            uint32_t fbn = new_size / BSIZE;
            uint32_t bno = bmap(dip, inum, fbn, 0);
            if (bno) {
                uint8_t  blk[BSIZE];
                uint32_t tail = new_size % BSIZE;
                rblock(bno, blk);
                memset(blk + tail, 0, BSIZE - tail);
                wblock(bno, blk);
            }
        }

        /* Free direct blocks beyond new_size */
        uint32_t first_free_fbn = (new_size + BSIZE - 1) / BSIZE;
        for (uint32_t fbn = first_free_fbn; fbn < NDIRECT; fbn++) {
            if (dip->addrs[fbn]) {
                bfree(dip->addrs[fbn]);
                dip->addrs[fbn] = 0;
            }
        }

        /* Handle indirect blocks */
        if (dip->addrs[NDIRECT]) {
            uint32_t indirect[NINDIRECT];
            rblock(dip->addrs[NDIRECT], indirect);
            int any = 0;
            for (uint32_t i = 0; i < NINDIRECT; i++) {
                uint32_t fbn = NDIRECT + i;
                if (fbn >= first_free_fbn && indirect[i]) {
                    bfree(indirect[i]);
                    indirect[i] = 0;
                } else if (indirect[i]) {
                    any = 1;
                }
            }
            if (!any) {
                bfree(dip->addrs[NDIRECT]);
                dip->addrs[NDIRECT] = 0;
            } else {
                wblock(dip->addrs[NDIRECT], indirect);
            }
        }

        dip->size = new_size;
        iupdate(inum, dip);

    } else if (new_size > dip->size) {
        /* --- Grow: zero-fill gap ----------------------------------- */
        uint32_t old_size = dip->size;
        uint8_t  zeros[BSIZE];
        memset(zeros, 0, BSIZE);

        /* Pad tail of last existing block */
        if (old_size > 0 && (old_size % BSIZE) != 0) {
            uint32_t fbn = old_size / BSIZE;
            uint32_t bno = bmap(dip, inum, fbn, 1);
            if (!bno) return -ENOSPC;
            uint8_t blk[BSIZE];
            rblock(bno, blk);
            uint32_t pad = BSIZE - (old_size % BSIZE);
            if (pad > new_size - old_size) pad = new_size - old_size;
            memset(blk + (old_size % BSIZE), 0, pad);
            wblock(bno, blk);
        }

        /* Allocate full zero blocks for the gap */
        uint32_t start_fbn = (old_size + BSIZE - 1) / BSIZE;
        uint32_t end_fbn   = new_size / BSIZE; /* exclusive */
        for (uint32_t fbn = start_fbn; fbn < end_fbn; fbn++) {
            uint32_t bno = bmap(dip, inum, fbn, 1);
            if (!bno) { dip->size = fbn * BSIZE; iupdate(inum, dip); return -ENOSPC; }
            wblock(bno, zeros);
        }

        dip->size = new_size;
        iupdate(inum, dip);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Directory helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * dirlookup: search directory dir_inum for name.
 * Returns inum of entry (>0) or 0 if not found.
 * *poff is set to the byte offset of the dirent (useful for dirlink).
 */
uint32_t dirlookup(uint32_t dir_inum, struct xv6_dinode *dir_dip,
                   const char *name, uint32_t *poff)
{
    struct xv6_dirent de;

    for (uint32_t off = 0; off < dir_dip->size; off += sizeof(de)) {
        if (readi(dir_inum, dir_dip, &de, off, sizeof(de)) != (int)sizeof(de))
            break;
        if (de.inum == 0) continue;

        /* name may not be NUL-terminated – compare up to DIRSIZ chars */
        char nm[DIRSIZ + 1];
        memcpy(nm, de.name, DIRSIZ);
        nm[DIRSIZ] = '\0';

        if (strcmp(nm, name) == 0) {
            if (poff) *poff = off;
            return de.inum;
        }
    }
    return 0;
}

/*
 * dirlink: add an entry (inum, name) to directory dir_inum.
 * Reuses the first free slot (inum==0); otherwise appends.
 * Returns 0 on success, -ENOSPC on failure.
 */
int dirlink(uint32_t dir_inum, struct xv6_dinode *dir_dip,
            const char *name, uint32_t inum)
{
    struct xv6_dirent de;
    uint32_t off;

    /* Look for a free slot */
    for (off = 0; off < dir_dip->size; off += sizeof(de)) {
        if (readi(dir_inum, dir_dip, &de, off, sizeof(de)) != (int)sizeof(de))
            break;
        if (de.inum == 0) goto found;
    }
    /* Append */
found:
    de.inum = (uint16_t)inum;
    memset(de.name, 0, DIRSIZ);
    memcpy(de.name, name, strnlen(name, DIRSIZ));

    if (writei(dir_inum, dir_dip, &de, off, sizeof(de)) != (int)sizeof(de))
        return -ENOSPC;
    return 0;
}

/*
 * dirunlink: set the inum of entry name in dir_inum to 0.
 * Returns 0 on success, -ENOENT if not found.
 */
int dirunlink(uint32_t dir_inum, struct xv6_dinode *dir_dip, const char *name)
{
    struct xv6_dirent de;
    uint32_t off;

    if (!dirlookup(dir_inum, dir_dip, name, &off)) return -ENOENT;

    readi(dir_inum, dir_dip, &de, off, sizeof(de));
    de.inum = 0;
    writei(dir_inum, dir_dip, &de, off, sizeof(de));
    return 0;
}

/*
 * dir_is_empty: returns 1 if dir only contains "." and "..".
 */
int dir_is_empty(uint32_t dir_inum, struct xv6_dinode *dir_dip)
{
    struct xv6_dirent de;
    for (uint32_t off = 0; off < dir_dip->size; off += sizeof(de)) {
        if (readi(dir_inum, dir_dip, &de, off, sizeof(de)) != (int)sizeof(de))
            break;
        if (de.inum == 0) continue;
        char nm[DIRSIZ + 1];
        memcpy(nm, de.name, DIRSIZ);
        nm[DIRSIZ] = '\0';
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Path resolution – namei / nameiparent                              */
/* ------------------------------------------------------------------ */

/* skipelem: extract the next path element from path into name[DIRSIZ+1].
 * Returns pointer past the element, or NULL at end. */
static const char *skipelem(const char *path, char *name)
{
    while (*path == '/') path++;
    if (!*path) return NULL;

    const char *s = path;
    while (*path && *path != '/') path++;
    int len = (int)(path - s);
    if (len >= DIRSIZ + 1) len = DIRSIZ;
    memcpy(name, s, (size_t)len);
    name[len] = '\0';
    return path;
}

/*
 * namex: resolve path starting from start_inum.
 * If parent=1, stop one component early and set *name to last component.
 */
static uint32_t namex(const char *path, int parent, char *name)
{
    uint32_t inum;

    if (*path == '/')
        inum = ROOTINO;
    else
        inum = ROOTINO; /* relative paths treated as absolute for simplicity */

    char elem[DIRSIZ + 1];
    const char *next;
    while ((next = skipelem(path, elem)) != NULL) {
        if (parent && *next == '\0') {
            /* Last component – return parent inum and copy name */
            if (name) {
                strncpy(name, elem, DIRSIZ);
                name[DIRSIZ] = '\0';
            }
            return inum;
        }

        struct xv6_dinode dip;
        iget(inum, &dip);
        if (dip.type != T_DIR) return 0;

        uint32_t next_inum = dirlookup(inum, &dip, elem, NULL);
        if (!next_inum) return 0;

        path  = next;
        inum  = next_inum;
        if (name) strncpy(name, elem, DIRSIZ + 1);
    }

    if (parent) return 0; /* path ended without a last component */
    return inum;
}

uint32_t namei(const char *path)
{
    return namex(path, 0, NULL);
}

uint32_t nameiparent(const char *path, char *name)
{
    return namex(path, 1, name);
}

/* ------------------------------------------------------------------ */
/*  Statfs helpers                                                     */
/* ------------------------------------------------------------------ */

uint32_t free_block_count(void)
{
    uint8_t  buf[BSIZE];
    uint32_t first_data = g_sb.size - g_sb.nblocks;
    uint32_t count      = 0;

    for (uint32_t b = 0; b < g_sb.size; b += BPB) {
        rblock(BBLOCK(b, g_sb), buf);
        for (uint32_t bi = 0; bi < BPB && (b + bi) < g_sb.size; bi++) {
            if (b + bi < first_data) continue;
            if (!(buf[bi / 8] & (1u << (bi % 8))))
                count++;
        }
    }
    return count;
}

uint32_t free_inode_count(void)
{
    uint32_t count = 0;
    for (uint32_t inum = 1; inum < g_sb.ninodes; inum++) {
        struct xv6_dinode dip;
        iget(inum, &dip);
        if (dip.type == 0) count++;
    }
    return count;
}

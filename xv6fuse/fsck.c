/*
 * fsck.c – xv6fsck: read-only consistency checker for xv6 fs.img.
 *
 * Exit 0 = CLEAN, exit 1 = INCONSISTENT.
 *
 * Checks performed:
 *  1. Superblock magic and sane layout (inodestart, bmapstart in range).
 *  2. All inode types are 0, 1, 2, or 3.
 *  3. inode sizes consistent with block addresses.
 *  4. All block addresses (direct and indirect) in the data-block range.
 *  5. No block used by more than one inode / indirect block.
 *  6. Every used block is marked allocated in the bitmap (no free-but-used).
 *  7. Every allocated bitmap bit refers to a block actually used (no leaks).
 *  8. All metadata blocks (boot, sb, log, inode, bitmap) marked used.
 *  9. No block addresses beyond the last block needed by the file size.
 * 10. Root inode (1) is a directory.
 * 11. Root "." points to itself; ".." also points to root.
 * 12. Every directory entry points to an in-use inode.
 * 13. Every in-use inode is reachable from the root.
 * 14. nlink equals the number of directory entries pointing to that inode
 *     (dirs: 1 (parent entry) + number of subdirs that point back via "..").
 *
 * Prints inodes in use and free data-block count at the end.
 */

#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "xv6fs.h"

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */
static int errors = 0;

#define ERR(fmt, ...) do { fprintf(stderr, "FSCK ERROR: " fmt "\n", ##__VA_ARGS__); errors++; } while(0)

/* Arrays indexed by block number and inode number */
static uint8_t  *block_ref_count; /* how many times each block is referenced */
static uint8_t  *bitmap_in_use;   /* 1 if the bitmap says this block is used */
static uint32_t *link_count;      /* counted reference count for each inode   */
static uint8_t  *reachable;       /* 1 if inode is reachable from root        */
static struct xv6_dinode *inodes; /* cached copy of all inodes                */

/* ------------------------------------------------------------------ */
/*  Phase helpers                                                      */
/* ------------------------------------------------------------------ */

static uint32_t first_data_block(void)
{
    return g_sb.size - g_sb.nblocks;
}

static void check_block_in_range(uint32_t bno, uint32_t inum, const char *ctx)
{
    if (bno == 0) return; /* 0 means unallocated */
    uint32_t fd = first_data_block();
    if (bno < fd || bno >= g_sb.size) {
        ERR("inode %u: %s block %u out of range [%u, %u)",
            inum, ctx, bno, fd, g_sb.size);
        return;
    }
    if (block_ref_count[bno]++ > 0) {
        ERR("inode %u: %s block %u used more than once", inum, ctx, bno);
    }
}

static void check_inode_blocks(uint32_t inum)
{
    struct xv6_dinode *dip = &inodes[inum];
    if (dip->type == 0) return;

    /* Determine how many blocks the file actually needs */
    uint32_t need_fbn = (dip->size + BSIZE - 1) / BSIZE;

    /* Direct blocks */
    for (uint32_t i = 0; i < NDIRECT; i++) {
        if (dip->addrs[i] == 0) continue;
        if (i >= need_fbn)
            ERR("inode %u: direct addr[%u]=%u beyond file size %u",
                inum, i, dip->addrs[i], dip->size);
        check_block_in_range(dip->addrs[i], inum, "direct");
    }

    /* Indirect block */
    if (dip->addrs[NDIRECT]) {
        uint32_t iblock = dip->addrs[NDIRECT];
        uint32_t fd = first_data_block();
        if (iblock < fd || iblock >= g_sb.size) {
            ERR("inode %u: indirect block %u out of range", inum, iblock);
        } else {
            if (block_ref_count[iblock]++ > 0)
                ERR("inode %u: indirect block %u used more than once", inum, iblock);

            uint32_t buf[NINDIRECT];
            if (pread(g_imgfd, buf, BSIZE, (off_t)iblock * BSIZE) != BSIZE) {
                ERR("inode %u: cannot read indirect block %u", inum, iblock);
                return;
            }
            for (uint32_t i = 0; i < NINDIRECT; i++) {
                if (buf[i] == 0) continue;
                uint32_t fbn = NDIRECT + i;
                if (fbn >= need_fbn)
                    ERR("inode %u: indirect[%u]=%u beyond file size %u",
                        inum, i, buf[i], dip->size);
                check_block_in_range(buf[i], inum, "indirect");
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Phase 2: bitmap consistency                                        */
/* ------------------------------------------------------------------ */
static void load_bitmap(void)
{
    uint8_t buf[BSIZE];
    for (uint32_t b = 0; b < g_sb.size; b += BPB) {
        uint32_t bmap_bno = BBLOCK(b, g_sb);
        if (pread(g_imgfd, buf, BSIZE, (off_t)bmap_bno * BSIZE) != BSIZE) {
            ERR("cannot read bitmap block %u", bmap_bno);
            return;
        }
        for (uint32_t bi = 0; bi < BPB && (b + bi) < g_sb.size; bi++) {
            if (buf[bi / 8] & (1u << (bi % 8)))
                bitmap_in_use[b + bi] = 1;
        }
    }
}

/* Mark metadata blocks as expected to be used */
static void mark_metadata(void)
{
    /* Boot (0) + superblock (1) */
    block_ref_count[0]++;
    block_ref_count[1]++;

    /* Log blocks: logstart .. logstart+nlog-1 */
    for (uint32_t b = g_sb.logstart; b < g_sb.logstart + g_sb.nlog; b++)
        block_ref_count[b]++;

    /* Inode blocks */
    uint32_t inode_blks = (g_sb.ninodes / IPB) + 1;
    for (uint32_t b = g_sb.inodestart; b < g_sb.inodestart + inode_blks; b++)
        block_ref_count[b]++;

    /* Bitmap blocks */
    uint32_t bmap_blks = (g_sb.size / BPB) + 1;
    for (uint32_t b = g_sb.bmapstart; b < g_sb.bmapstart + bmap_blks; b++)
        block_ref_count[b]++;
}

/* ------------------------------------------------------------------ */
/*  Phase 3: directory traversal and reachability                     */
/* ------------------------------------------------------------------ */
static void traverse_dir(uint32_t inum, uint32_t parent_inum);

static void traverse_dir(uint32_t inum, uint32_t parent_inum)
{
    if (reachable[inum]) return; /* already visited (cycle guard) */
    reachable[inum] = 1;

    struct xv6_dinode *dip = &inodes[inum];
    if (dip->type != T_DIR) return;

    struct xv6_dirent de;
    int saw_dot  = 0;
    int saw_dotdot = 0;

    for (uint32_t off = 0; off < dip->size; off += sizeof(de)) {
        int r = readi(inum, dip, &de, off, sizeof(de));
        if (r != (int)sizeof(de)) break;
        if (de.inum == 0) continue;

        char name[DIRSIZ + 1];
        memcpy(name, de.name, DIRSIZ);
        name[DIRSIZ] = '\0';

        if (de.inum >= g_sb.ninodes) {
            ERR("dir %u: entry '%s' has inum %u out of range", inum, name, de.inum);
            continue;
        }

        if (inodes[de.inum].type == 0) {
            ERR("dir %u: entry '%s' points to free inode %u", inum, name, de.inum);
            continue;
        }

        /* Count link */
        link_count[de.inum]++;

        if (strcmp(name, ".") == 0) {
            saw_dot = 1;
            if (de.inum != inum)
                ERR("dir %u: '.' points to %u (expected %u)", inum, de.inum, inum);
        } else if (strcmp(name, "..") == 0) {
            saw_dotdot = 1;
            if (de.inum != parent_inum)
                ERR("dir %u: '..' points to %u (expected %u)", inum, de.inum, parent_inum);
        } else {
            if (inodes[de.inum].type == T_DIR)
                traverse_dir(de.inum, inum);
            else
                reachable[de.inum] = 1;
        }
    }

    if (!saw_dot)    ERR("dir %u: missing '.' entry", inum);
    if (!saw_dotdot) ERR("dir %u: missing '..' entry", inum);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: xv6fsck <fs.img>\n");
        return 1;
    }

    if (fs_open(argv[1]) < 0) {
        perror("fs_open");
        return 1;
    }

    printf("xv6fsck: checking %s\n", argv[1]);
    printf("  superblock: size=%u nblocks=%u ninodes=%u nlog=%u "
           "logstart=%u inodestart=%u bmapstart=%u\n",
           g_sb.size, g_sb.nblocks, g_sb.ninodes, g_sb.nlog,
           g_sb.logstart, g_sb.inodestart, g_sb.bmapstart);

    /* ---- Phase 0: superblock sanity ---- */
    if (g_sb.magic != FSMAGIC)
        ERR("bad magic 0x%08x", g_sb.magic);

    if (g_sb.inodestart <= 1 || g_sb.inodestart >= g_sb.size)
        ERR("inodestart %u out of range", g_sb.inodestart);
    if (g_sb.bmapstart <= g_sb.inodestart || g_sb.bmapstart >= g_sb.size)
        ERR("bmapstart %u out of range", g_sb.bmapstart);
    if (g_sb.logstart < 2 || g_sb.logstart >= g_sb.inodestart)
        ERR("logstart %u out of range", g_sb.logstart);

    uint32_t total = g_sb.size;

    /* Allocate checker arrays */
    block_ref_count = calloc(total, 1);
    bitmap_in_use   = calloc(total, 1);
    link_count      = calloc(g_sb.ninodes, sizeof(uint32_t));
    reachable       = calloc(g_sb.ninodes, 1);
    inodes          = calloc(g_sb.ninodes, sizeof(struct xv6_dinode));
    if (!block_ref_count || !bitmap_in_use || !link_count || !reachable || !inodes) {
        fprintf(stderr, "xv6fsck: out of memory\n");
        return 1;
    }

    /* Load all inodes into memory */
    for (uint32_t i = 1; i < g_sb.ninodes; i++)
        iget(i, &inodes[i]);

    /* ---- Phase 1: inode types, block addresses ---- */
    for (uint32_t i = 1; i < g_sb.ninodes; i++) {
        int16_t t = inodes[i].type;
        if (t != 0 && t != T_DIR && t != T_FILE && t != T_DEVICE)
            ERR("inode %u: invalid type %d", i, t);
        if (t != 0)
            check_inode_blocks(i);
    }

    /* ---- Phase 2: metadata block references ---- */
    mark_metadata();

    /* ---- Load bitmap ---- */
    load_bitmap();

    /* Compare ref counts vs bitmap for every block */
    for (uint32_t b = 0; b < total; b++) {
        int used   = (block_ref_count[b] > 0);
        int marked = bitmap_in_use[b];

        if (used && !marked)
            ERR("block %u is used but NOT marked in bitmap", b);
        if (!used && marked)
            ERR("block %u is marked in bitmap but NOT used (leak)", b);
        if (block_ref_count[b] > 1 && b >= first_data_block())
            ERR("block %u referenced %u times", b, block_ref_count[b]);
    }

    /* ---- Phase 3: root inode ---- */
    if (inodes[ROOTINO].type != T_DIR)
        ERR("root inode %u is not a directory (type=%d)", ROOTINO, inodes[ROOTINO].type);

    /* ---- Phase 4: directory traversal (reachability + "." ".." + entries) ---- */
    traverse_dir(ROOTINO, ROOTINO);

    /* ---- Phase 5: unreachable inodes ---- */
    for (uint32_t i = 1; i < g_sb.ninodes; i++) {
        if (inodes[i].type != 0 && !reachable[i])
            ERR("inode %u (type %d) is not reachable from root", i, inodes[i].type);
    }

    /* ---- Phase 6: nlink check ---- */
    for (uint32_t i = 1; i < g_sb.ninodes; i++) {
        if (inodes[i].type == 0) continue;

        uint32_t expected = link_count[i];
        uint32_t actual   = (uint32_t)(uint16_t)inodes[i].nlink;

        /*
         * xv6 nlink convention:
         *   - Regular files: nlink = number of directory entries pointing to it.
         *   - Directories: nlink = 1 (parent entry) + number of sub-directories
         *     (each subdir adds 1 via its ".." entry).
         *
         * During traversal we count every entry including "." and "..",
         * so for dir inum: link_count = (parent entry) + ("." self) + N×".." from subdirs
         *                             = 1 + 1 + subdirs.
         * But xv6 nlink for a dir = 1 + subdirs (doesn't count ".").
         * So subtract 1 for directories to match.
         */
        if (inodes[i].type == T_DIR && expected > 0) expected--;

        if (actual != expected)
            ERR("inode %u: nlink=%u but counted %u directory entries",
                i, actual, expected);
    }

    /* ---- Summary ---- */
    uint32_t used_inodes  = 0;
    uint32_t free_data    = 0;
    for (uint32_t i = 1; i < g_sb.ninodes; i++)
        if (inodes[i].type != 0) used_inodes++;
    for (uint32_t b = first_data_block(); b < g_sb.size; b++)
        if (!bitmap_in_use[b]) free_data++;

    printf("  inodes in use: %u / %u\n", used_inodes, g_sb.ninodes - 1);
    printf("  free data blocks: %u / %u\n", free_data, g_sb.nblocks);

    free(block_ref_count);
    free(bitmap_in_use);
    free(link_count);
    free(reachable);
    free(inodes);
    fs_close();

    if (errors) {
        printf("xv6fsck: INCONSISTENT (%d error(s))\n", errors);
        return 1;
    }
    printf("xv6fsck: CLEAN\n");
    return 0;
}

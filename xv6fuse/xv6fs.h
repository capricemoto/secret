/*
 * xv6fs.h – On-disk layout structs and constants, derived verbatim from
 * the xv6-riscv kernel/fs.h (this tree).  uint replaced with uint32_t so
 * the host compiler sees fixed-width types.
 *
 * Key constants (all from this tree; never hard-coded – read from sb at
 * runtime except BSIZE, DIRSIZ, NDIRECT which are compile-time):
 *   BSIZE   = 1024
 *   DIRSIZ  = 14          (stock xv6)
 *   NDIRECT = 12
 *   FSMAGIC = 0x10203040
 */

#pragma once
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Fixed compile-time constants (from kernel/fs.h and kernel/param.h) */
/* ------------------------------------------------------------------ */
#define BSIZE    1024
#define DIRSIZ   14
#define NDIRECT  12
#define NINDIRECT (BSIZE / sizeof(uint32_t))   /* 256 */
#define MAXFILE  (NDIRECT + NINDIRECT)         /* 268 */
#define FSMAGIC  0x10203040u

/* File types (from kernel/stat.h) */
#define T_DIR    1
#define T_FILE   2
#define T_DEVICE 3

/* Root inode number */
#define ROOTINO  1

/* Derived block-layout macros – the macros take a struct xv6_superblock */
#define IPB      (BSIZE / sizeof(struct xv6_dinode))  /* inodes per block = 16 */
#define BPB      (BSIZE * 8)                          /* bitmap bits per block = 8192 */

#define IBLOCK(i, sb)  ((i) / IPB + (sb).inodestart)
#define BBLOCK(b, sb)  ((b) / BPB + (sb).bmapstart)

/* ------------------------------------------------------------------ */
/*  On-disk superblock (from kernel/fs.h)                             */
/* ------------------------------------------------------------------ */
struct xv6_superblock {
    uint32_t magic;      /* Must be FSMAGIC            */
    uint32_t size;       /* Size of fs image (blocks)  */
    uint32_t nblocks;    /* Number of data blocks       */
    uint32_t ninodes;    /* Number of inodes            */
    uint32_t nlog;       /* Number of log blocks        */
    uint32_t logstart;   /* Block number of first log block   */
    uint32_t inodestart; /* Block number of first inode block */
    uint32_t bmapstart;  /* Block number of first free map block */
};

/* ------------------------------------------------------------------ */
/*  On-disk inode (from kernel/fs.h)                                  */
/* ------------------------------------------------------------------ */
struct xv6_dinode {
    int16_t  type;                   /* File type (0 = free)        */
    int16_t  major;                  /* Major device (T_DEVICE only)*/
    int16_t  minor;                  /* Minor device (T_DEVICE only)*/
    int16_t  nlink;                  /* Number of directory entries  */
    uint32_t size;                   /* File size in bytes           */
    uint32_t addrs[NDIRECT + 1];     /* Data block addresses         */
};

/* ------------------------------------------------------------------ */
/*  On-disk directory entry (from kernel/fs.h)                        */
/* ------------------------------------------------------------------ */
struct xv6_dirent {
    uint16_t inum;
    char     name[DIRSIZ]; /* May NOT be NUL-terminated when full */
};

/* ------------------------------------------------------------------ */
/*  xv6fs.c public interface                                           */
/* ------------------------------------------------------------------ */

/* Global image fd and superblock filled by fs_open() */
extern int g_imgfd;
extern struct xv6_superblock g_sb;

/* Open the image, read and validate the superblock.
 * Returns 0 on success, -1 on error (errno set). */
int  fs_open(const char *path);
void fs_close(void);

/* Raw block I/O */
int  rblock(uint32_t bno, void *buf);
int  wblock(uint32_t bno, const void *buf);

/* Bitmap */
uint32_t balloc(void);         /* allocate a data block; returns bno or 0 */
void     bfree(uint32_t bno);

/* Inode I/O */
void iget(uint32_t inum, struct xv6_dinode *dip);
void iupdate(uint32_t inum, const struct xv6_dinode *dip);
uint32_t ialloc(int16_t type);
void     ifree(uint32_t inum);

/* Block mapping – alloc=1 allocates missing blocks */
uint32_t bmap(struct xv6_dinode *dip, uint32_t inum, uint32_t fbn, int alloc);

/* File data I/O */
int readi (uint32_t inum, struct xv6_dinode *dip,       void *buf, uint32_t off, uint32_t n);
int writei(uint32_t inum, struct xv6_dinode *dip, const void *buf, uint32_t off, uint32_t n);

/* Truncate: free blocks beyond new_size (0 = free all) */
int itrunc(uint32_t inum, struct xv6_dinode *dip, uint32_t new_size);

/* Directory operations */
/* Look up name in dir inum; set *off to byte offset of entry if found */
uint32_t dirlookup(uint32_t dir_inum, struct xv6_dinode *dir_dip, const char *name, uint32_t *off);
/* Add (inum, name) to dir; returns 0 on success, -ENOSPC */
int dirlink(uint32_t dir_inum, struct xv6_dinode *dir_dip, const char *name, uint32_t inum);
/* Remove name from dir; returns 0 on success, -ENOENT */
int dirunlink(uint32_t dir_inum, struct xv6_dinode *dir_dip, const char *name);
/* Returns 1 if dir only contains "." and ".." */
int dir_is_empty(uint32_t dir_inum, struct xv6_dinode *dir_dip);

/* Path resolution */
uint32_t namei(const char *path);
uint32_t nameiparent(const char *path, char *name); /* name buf must be DIRSIZ+1 */

/* Helpers */
uint32_t free_block_count(void);
uint32_t free_inode_count(void);

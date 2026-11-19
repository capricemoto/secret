/*
 * xv6fuse.c – FUSE driver that mounts xv6's fs.img on Linux.
 *
 * Uses libfuse3 high-level API (FUSE_USE_VERSION 31).
 * Run single-threaded (-s flag added in main) because xv6fs.c has no
 * locks.  No xv6 logging is used; we write directly to the image.
 *
 * Usage: ./xv6fuse <fs.img> <mountpoint> [fuse options]
 */

#define FUSE_USE_VERSION 31
#define _FILE_OFFSET_BITS 64

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>

#include "xv6fs.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Convert xv6 inode type to POSIX mode bits */
static mode_t itype_to_mode(int16_t type)
{
    switch (type) {
    case T_DIR:    return S_IFDIR | 0755;
    case T_FILE:   return S_IFREG | 0644;
    case T_DEVICE: return S_IFCHR | 0600;
    default:       return 0;
    }
}

/* Resolve path -> inum; return -ENOENT on failure */
static int resolve(const char *path, uint32_t *inum_out, struct xv6_dinode *dip_out)
{
    uint32_t inum = namei(path);
    if (!inum) return -ENOENT;
    if (inum_out) *inum_out = inum;
    if (dip_out)  iget(inum, dip_out);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  getattr                                                            */
/* ------------------------------------------------------------------ */
static int xv6_getattr(const char *path, struct stat *st,
                       struct fuse_file_info *fi)
{
    (void)fi;
    struct xv6_dinode dip;
    uint32_t inum;
    int r = resolve(path, &inum, &dip);
    if (r < 0) return r;

    memset(st, 0, sizeof(*st));
    st->st_ino   = inum;
    st->st_mode  = itype_to_mode(dip.type);
    st->st_nlink = (nlink_t)dip.nlink;
    st->st_size  = (off_t)dip.size;
    st->st_blksize = BSIZE;
    st->st_blocks  = ((off_t)dip.size + 511) / 512;

    if (dip.type == T_DEVICE)
        st->st_rdev = makedev(dip.major, dip.minor);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  readdir                                                            */
/* ------------------------------------------------------------------ */
static int xv6_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    (void)offset; (void)fi; (void)flags;

    struct xv6_dinode dip;
    uint32_t inum;
    int r = resolve(path, &inum, &dip);
    if (r < 0) return r;
    if (dip.type != T_DIR) return -ENOTDIR;

    struct xv6_dirent de;
    for (uint32_t off = 0; off < dip.size; off += sizeof(de)) {
        if (readi(inum, &dip, &de, off, sizeof(de)) != (int)sizeof(de)) break;
        if (de.inum == 0) continue;

        /* name may not be NUL-terminated */
        char name[DIRSIZ + 1];
        memcpy(name, de.name, DIRSIZ);
        name[DIRSIZ] = '\0';

        struct stat st;
        memset(&st, 0, sizeof(st));
        struct xv6_dinode cdip;
        iget(de.inum, &cdip);
        st.st_ino  = de.inum;
        st.st_mode = itype_to_mode(cdip.type);

        if (filler(buf, name, &st, 0, 0)) break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  open                                                               */
/* ------------------------------------------------------------------ */
static int xv6_open(const char *path, struct fuse_file_info *fi)
{
    uint32_t inum;
    struct xv6_dinode dip;
    int r = resolve(path, &inum, &dip);
    if (r < 0) return r;
    if (dip.type == T_DIR) return -EISDIR;
    fi->fh = inum;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  read                                                               */
/* ------------------------------------------------------------------ */
static int xv6_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
    (void)path;
    uint32_t inum = (uint32_t)fi->fh;
    struct xv6_dinode dip;
    iget(inum, &dip);
    return readi(inum, &dip, buf, (uint32_t)offset, (uint32_t)size);
}

/* ------------------------------------------------------------------ */
/*  write                                                              */
/* ------------------------------------------------------------------ */
static int xv6_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    (void)path;
    uint32_t inum = (uint32_t)fi->fh;
    struct xv6_dinode dip;
    iget(inum, &dip);
    int r = writei(inum, &dip, buf, (uint32_t)offset, (uint32_t)size);
    return r;
}

/* ------------------------------------------------------------------ */
/*  create                                                             */
/* ------------------------------------------------------------------ */
static int xv6_create(const char *path, mode_t mode,
                      struct fuse_file_info *fi)
{
    (void)mode;
    char name[DIRSIZ + 1];
    uint32_t parent_inum = nameiparent(path, name);
    if (!parent_inum) return -ENOENT;
    if (strlen(name) > DIRSIZ) return -ENAMETOOLONG;

    struct xv6_dinode parent_dip;
    iget(parent_inum, &parent_dip);
    if (parent_dip.type != T_DIR) return -ENOTDIR;

    /* Check if already exists */
    if (dirlookup(parent_inum, &parent_dip, name, NULL))
        return -EEXIST;

    uint32_t inum = ialloc(T_FILE);
    if (!inum) return -ENOSPC;

    struct xv6_dinode dip;
    iget(inum, &dip);
    dip.nlink = 1;
    iupdate(inum, &dip);

    /* Re-read parent (ialloc may have written to same block) */
    iget(parent_inum, &parent_dip);
    if (dirlink(parent_inum, &parent_dip, name, inum) < 0) {
        dip.nlink = 0;
        iupdate(inum, &dip);
        ifree(inum);
        return -ENOSPC;
    }

    fi->fh = inum;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  mkdir                                                              */
/* ------------------------------------------------------------------ */
static int xv6_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    char name[DIRSIZ + 1];
    uint32_t parent_inum = nameiparent(path, name);
    if (!parent_inum) return -ENOENT;
    if (strlen(name) > DIRSIZ) return -ENAMETOOLONG;

    struct xv6_dinode parent_dip;
    iget(parent_inum, &parent_dip);
    if (parent_dip.type != T_DIR) return -ENOTDIR;
    if (dirlookup(parent_inum, &parent_dip, name, NULL)) return -EEXIST;

    uint32_t inum = ialloc(T_DIR);
    if (!inum) return -ENOSPC;

    struct xv6_dinode dip;
    iget(inum, &dip);
    dip.nlink = 1; /* xv6 counts only parent->dir link (not ".") */
    iupdate(inum, &dip);

    /* Add "." and ".." */
    if (dirlink(inum, &dip, ".", inum) < 0 ||
        dirlink(inum, &dip, "..", parent_inum) < 0) {
        itrunc(inum, &dip, 0);
        ifree(inum);
        return -ENOSPC;
    }

    /* Link parent directory into new dir */
    iget(parent_inum, &parent_dip);
    if (dirlink(parent_inum, &parent_dip, name, inum) < 0) {
        itrunc(inum, &dip, 0);
        ifree(inum);
        return -ENOSPC;
    }

    /* Increment parent nlink (xv6 convention: parent gains a link for each subdir) */
    iget(parent_inum, &parent_dip);
    parent_dip.nlink++;
    iupdate(parent_inum, &parent_dip);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  unlink                                                             */
/* ------------------------------------------------------------------ */
static int xv6_unlink(const char *path)
{
    char name[DIRSIZ + 1];
    uint32_t parent_inum = nameiparent(path, name);
    if (!parent_inum) return -ENOENT;

    struct xv6_dinode parent_dip;
    iget(parent_inum, &parent_dip);

    uint32_t inum = dirlookup(parent_inum, &parent_dip, name, NULL);
    if (!inum) return -ENOENT;

    struct xv6_dinode dip;
    iget(inum, &dip);
    if (dip.type == T_DIR) return -EISDIR;

    /* Remove directory entry */
    dirunlink(parent_inum, &parent_dip, name);

    /* Decrement nlink; free inode if 0 */
    iget(inum, &dip); /* re-read after dirunlink */
    dip.nlink--;
    iupdate(inum, &dip);
    if (dip.nlink == 0) {
        itrunc(inum, &dip, 0);
        ifree(inum);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  rmdir                                                              */
/* ------------------------------------------------------------------ */
static int xv6_rmdir(const char *path)
{
    char name[DIRSIZ + 1];
    uint32_t parent_inum = nameiparent(path, name);
    if (!parent_inum) return -ENOENT;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EINVAL;

    struct xv6_dinode parent_dip;
    iget(parent_inum, &parent_dip);

    uint32_t inum = dirlookup(parent_inum, &parent_dip, name, NULL);
    if (!inum) return -ENOENT;

    struct xv6_dinode dip;
    iget(inum, &dip);
    if (dip.type != T_DIR) return -ENOTDIR;
    if (!dir_is_empty(inum, &dip)) return -ENOTEMPTY;

    /* Remove from parent */
    dirunlink(parent_inum, &parent_dip, name);

    /* Decrement parent nlink (lost the ".." back-link from subdir) */
    iget(parent_inum, &parent_dip);
    parent_dip.nlink--;
    iupdate(parent_inum, &parent_dip);

    /* Free the dir */
    iget(inum, &dip);
    dip.nlink = 0;
    iupdate(inum, &dip);
    itrunc(inum, &dip, 0);
    ifree(inum);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  truncate                                                           */
/* ------------------------------------------------------------------ */
static int xv6_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
    (void)fi;
    struct xv6_dinode dip;
    uint32_t inum;
    int r = resolve(path, &inum, &dip);
    if (r < 0) return r;
    if (dip.type != T_FILE) return -EINVAL;
    return itrunc(inum, &dip, (uint32_t)size);
}

/* ------------------------------------------------------------------ */
/*  utimens – accept and ignore (xv6 has no timestamps)               */
/* ------------------------------------------------------------------ */
static int xv6_utimens(const char *path, const struct timespec tv[2],
                       struct fuse_file_info *fi)
{
    (void)path; (void)tv; (void)fi;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  statfs                                                             */
/* ------------------------------------------------------------------ */
static int xv6_statfs(const char *path, struct statvfs *stbuf)
{
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->f_bsize   = BSIZE;
    stbuf->f_frsize  = BSIZE;
    stbuf->f_blocks  = g_sb.nblocks;
    stbuf->f_bfree   = free_block_count();
    stbuf->f_bavail  = stbuf->f_bfree;
    stbuf->f_files   = g_sb.ninodes;
    stbuf->f_ffree   = free_inode_count();
    stbuf->f_namemax = DIRSIZ;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  init / destroy                                                     */
/* ------------------------------------------------------------------ */
static void *xv6_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->use_ino    = 1;      /* use inode numbers from getattr */
    cfg->hard_remove = 1;    /* delete immediately, no hide-and-rename */
    return NULL;
}

static void xv6_destroy(void *private_data)
{
    (void)private_data;
    fs_close();
}

/* ------------------------------------------------------------------ */
/*  Operations table                                                   */
/* ------------------------------------------------------------------ */
static struct fuse_operations xv6_ops = {
    .getattr  = xv6_getattr,
    .readdir  = xv6_readdir,
    .open     = xv6_open,
    .read     = xv6_read,
    .write    = xv6_write,
    .create   = xv6_create,
    .mkdir    = xv6_mkdir,
    .unlink   = xv6_unlink,
    .rmdir    = xv6_rmdir,
    .truncate = xv6_truncate,
    .utimens  = xv6_utimens,
    .statfs   = xv6_statfs,
    .init     = xv6_init,
    .destroy  = xv6_destroy,
};

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s <fs.img> <mountpoint> [fuse options]\n",
                argv[0]);
        return 1;
    }

    const char *imgpath = argv[1];

    /* Open the image before handing control to FUSE */
    if (fs_open(imgpath) < 0) {
        perror("fs_open");
        return 1;
    }

    /* Build a new argv for fuse_main:
     *   argv[0] (program name)
     *   mountpoint  (argv[2])
     *   -s          (single-threaded; xv6fs.c has no locks)
     *   any extra fuse options the user passed
     */
    int    fuse_argc = 0;
    char **fuse_argv = malloc(sizeof(char *) * (size_t)(argc + 2));
    if (!fuse_argv) { perror("malloc"); return 1; }

    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = argv[2]; /* mountpoint */
    fuse_argv[fuse_argc++] = "-s";    /* single-threaded */
    for (int i = 3; i < argc; i++)
        fuse_argv[fuse_argc++] = argv[i];

    int ret = fuse_main(fuse_argc, fuse_argv, &xv6_ops, NULL);
    free(fuse_argv);
    return ret;
}

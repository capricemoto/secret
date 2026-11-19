# xv6fuse – FUSE driver for xv6's fs.img

Mount xv6's raw `fs.img` on Linux using FUSE3 (libfuse high-level API).

## Files

| File | Purpose |
|------|---------|
| `xv6fs.h` | On-disk structs from `kernel/fs.h` (uint→uint32_t), plus all function prototypes |
| `xv6fs.c` | Core library: block I/O, bitmap, inode CRUD, bmap, readi/writei, itrunc, directory ops, path resolution |
| `xv6fuse.c` | FUSE callbacks: getattr, readdir, open, read, write, create, mkdir, unlink, rmdir, truncate, utimens, statfs, init, destroy |
| `fsck.c` | Read-only consistency checker (xv6fsck) |
| `Makefile` | Builds both binaries using `pkg-config fuse3` |

## Build

```bash
sudo apt install build-essential libfuse3-dev fuse3 pkg-config
cd ~/xv6fuse
make
```

## Usage

```bash
# Always back up the image first
cp ~/xv6-riscv/fs.img ~/xv6-riscv/fs.img.orig

# Check the fresh image
./xv6fsck ~/xv6-riscv/fs.img

# Mount (never run QEMU at the same time!)
./xv6fuse ~/xv6-riscv/fs.img ~/xv6mnt -f   # -f = foreground

# In another terminal
ls -li ~/xv6mnt
cat ~/xv6mnt/README

# Copy files in from Linux
echo "hello from Linux" > ~/xv6mnt/hello.txt
mkdir ~/xv6mnt/docs
echo "note" > ~/xv6mnt/docs/note.txt
seq 1 20000 > /tmp/nums.txt
cp /tmp/nums.txt ~/xv6mnt/

# Unmount
fusermount3 -u ~/xv6mnt

# Check after modifications
./xv6fsck ~/xv6-riscv/fs.img

# Boot xv6 (Makefile must NOT regenerate fs.img)
cd ~/xv6-riscv
make qemu
```

## Design Notes

### No xv6 logging
xv6's on-disk log provides crash recovery for the kernel; we bypass it
and write directly with pread/pwrite.  This is safe because:
1. We mount single-threaded (`-s` in main).
2. We never run QEMU and the FUSE mount simultaneously.
3. Linux's page cache is not involved (we use O_RDWR on the raw image).

### Layout constants
All layout positions (inodestart, bmapstart, nlog) are read from the
superblock at runtime.  Only BSIZE, DIRSIZ, and NDIRECT are compiled in
(they define struct sizes, so they must match the image).

### Bitmap convention
Bit `i` of the bitmap (bit `i%8` of byte `i/8` of the bitmap block for
block `i`) is 1 when block `i` is allocated.  `balloc` starts scanning
from the first data block (`size − nblocks`) to avoid touching metadata.

### nlink convention (mirrors xv6)
- Regular files: nlink = number of hard links (directory entries).
- Directories: nlink = 1 (parent→dir entry) + number of immediate subdirectories.
  xv6 does **not** count the `"."` self-entry.

### FUSE options
`-s` (single-threaded) is inserted by `main()` automatically.
Run with `-f` to stay in the foreground and see debug messages.
Add `-d` for FUSE debug output.

## fsck checks

1. Superblock magic and sane layout  
2. Inode types are 0/1/2/3  
3. Block addresses in data-block range  
4. No block used more than once  
5. Every used block marked in bitmap  
6. Every bitmap-marked block actually used (no leaks)  
7. Metadata blocks marked used  
8. No block addresses beyond file size  
9. Root inode is a directory  
10. `"."` and `".."` correct in every directory  
11. Every directory entry points to an in-use inode  
12. Every in-use inode is reachable from root  
13. nlink equals counted directory-entry references  

Exit 0 = CLEAN, exit 1 = INCONSISTENT.

## Function glossary (for viva questions)

| Function | What it does |
|----------|-------------|
| `fs_open` | Opens the image file, reads block 1 (superblock), validates magic number |
| `rblock/wblock` | Raw 1024-byte block read/write using pread/pwrite |
| `balloc` | Scans the bitmap for a free data block, marks it used, zeros it, returns its block number |
| `bfree` | Clears the bitmap bit for a block |
| `iget` | Reads one dinode from the inode block into a struct |
| `iupdate` | Writes a dinode back (read-modify-write of the inode block) |
| `ialloc` | Finds an inode with type=0 (free), initialises it, returns its inum |
| `ifree` | Zeroes an inode (type=0 marks it free) |
| `bmap` | Maps file-block-number → disk-block-number; allocates blocks when alloc=1 |
| `readi` | Reads n bytes at offset from a file, using bmap to find each block |
| `writei` | Writes n bytes, allocating blocks as needed; updates inode size |
| `itrunc` | Frees (or allocates) blocks to change inode size; zeros partial blocks |
| `dirlookup` | Linear scan of a directory's entries to find a name |
| `dirlink` | Adds (name, inum) to a directory, reusing free slots before appending |
| `dirunlink` | Sets entry's inum to 0 (removes it) |
| `dir_is_empty` | Returns true if only "." and ".." remain |
| `namei` | Resolves a full path to an inum by walking components |
| `nameiparent` | Like namei but stops at the parent, returning the final component name |
| `free_block_count` | Counts 0-bits in bitmap for data blocks (for statfs) |
| `free_inode_count` | Counts type=0 inodes (for statfs) |

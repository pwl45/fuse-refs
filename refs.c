#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef linux
/* For pread()/pwrite() */
#define _XOPEN_SOURCE 500
#endif
#include <time.h>
#include "refs.h"
#include <assert.h>
#include <strings.h>
#include <fuse.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

// The first thing we do in init is open the "disk file".
// We use this backing file as our "disk" for all I/O in ReFS.
static int disk_fd;

// Statically allocate the main data structures in our file system.
// Global variables are not ideal, but these are the main pillars
// of our file system state.
// These in-memory representations will be kept consistent with the on-disk
// representation by frequently writing portions of them to disk_fd

static union superblock super;

static union inode *inode_table;

static struct bitmap *inode_bitmap;

static struct bitmap *data_bitmap;


/***********************/
/** Bitmap management **/
/***********************/

/**
 * Read a bitmap from disk into an in-memory representation.
 * @pre bmap points to an allocated struct bitmap, and bmap->n_bytes
 *      and bmap->n_valid_bytes are valid values
 * @return 0 on success, negative value on failure
 */
/* Convert a mode field into "ls -l" type perms field. */

static void* calloc_blocks(int numBlocks){
	return calloc(numBlocks,BLOCK_SIZE);
}
static int read_bitmap_from_disk(struct bitmap *bmap, lba_t bmap_start) {
	// Our bitmap's region of valid bits may not exactly align
	// with a"block boundary", but we must do block-aligned I/Os
	// when talking to our disk.  First "round up" our bitmap size
	// to the appropriate number of bytes, then make the I/O
	// request to read those blocks into our structure.
	size_t bmap_blocks = BYTES_TO_BLKS(bmap->n_bytes);
	int ret = read_blocks(bmap->bits, bmap_blocks, bmap_start);
	assert(ret == bmap_blocks);

	if (ret != bmap_blocks) {
		DEBUG_PRINT("Error reading bitmap (%d)", ret);
		return -EIO;
	}
	return 0;
}

/**
 * Write just the "bits" of our in-memory bitmap to disk
 * @pre bmap points to an allocated struct bitmap, and bmap->n_bytes
 *      and bmap->bits have valid values
 * @return 0 on success, negative value on failure
 */
static int write_bitmap_to_disk(struct bitmap *bmap, lba_t bmap_start) {
	size_t bmap_blocks = BYTES_TO_BLKS(bmap->n_bytes);
	int ret = write_blocks(bmap->bits, bmap_blocks, bmap_start);

	assert(ret == bmap_blocks);

	if (ret != bmap_blocks) {
		DEBUG_PRINT("Error writing bitmap (%d)", ret);
		return -EIO;
	}

	return 0;

}

/**
 * Utility function that writes the data region's bitmap in its entirety.
 */
static int sync_data_bitmap() {
	return write_bitmap_to_disk(data_bitmap, super.super.d_bitmap_start);
}

/**
 * Utility function that writes the inode table's bitmap in its entirety.
 */
static void sync_inode_bitmap() {
	write_bitmap_to_disk(inode_bitmap, super.super.i_bitmap_start);
}

/****************************/
/** Superblock management **/
/****************************/

static int write_super() {
	int ret = write_block(&super, SUPER_LBA);
	assert(ret == 1);
	return 0;
}


/** Inode / Inode Table management **/

/**
 * Write back the disk block that holds a given inode number.
 * Note that we write back more than just that inode, since we must
 * do I/O at the granularity of a 4096-byte disk block.
 */
 //Should this return 0?
static int write_inode(uint64_t inum) {
	// First, calcluate the offset of the block that contains this
	// inode. We know the start of the inode table (it's stored in
	// our super block) and we know the number of inodes per block.
	uint64_t itab_block = inum / INODES_PER_BLOCK;

	// Now, write the portion of the inode table contained in that block
	// to the corresponding lba
	return write_block(((void *)inode_table) + BLKS_TO_BYTES(itab_block),
			   super.super.i_table_start + itab_block);
}

/**
 * Write data to 4096-byte-aligned set of consecutive blocks
 * @param buf The memory buffer that has the data to be written
 * @param lba_count The number of blocks to write is (lba_count * 4096)
 * @param starting_lba The byte offset on our "disk" that we start writing
 *        is (starting_lba * 4096)
 * @return the number of blocks successfully written, or negative on error.
 */
ssize_t write_blocks(void *buf, size_t lba_count, off_t starting_lba) {
	int ret = pwrite(disk_fd, buf, BLKS_TO_BYTES(lba_count),
			 BLKS_TO_BYTES(starting_lba));
	if (ret < 0) {
		// pass along the error
		return ret;
	}

	// pwrite may return a partial success. If we don't write
	// a multiple of 4096 bytes, we have create a confusing situation.
	// it's not clear how to best handle this until we've built the rest
	// of our system
	if (BLOCK_OFFSET(ret) != 0) {
		// possibly handle incomplete write with retry?
		DEBUG_PRINT("partial write: write_blocks\n");
	}

	return BYTES_TO_BLKS(ret);
}

/**
 * Read data from a 4096-byte-aligned set of consecutive blocks
 * @param buf The memory buffer where we will store the data read
 * @param lba_count The number of blocks to read is (lba_count * 4096)
 * @param starting_lba The byte offset on our "disk" that we start reading
 *        is (starting_lba * 4096)
 * @return the number of blocks successfully read, or negative on error.

 */
ssize_t read_blocks(void *buf, size_t lba_count, off_t starting_lba) {
	int ret = pread(disk_fd, buf, BLKS_TO_BYTES(lba_count),
			BLKS_TO_BYTES(starting_lba));

	if (ret < 0) {
		// pass along the error
		return ret;
	}

	// pread may return partial success.
	// for read, the user could just ignore the data that
	// isn't "acknowleged", but we may want to revisit this
	// depending on our implementation
	if (BLOCK_OFFSET(ret) != 0) {
		// possibly handle incomplete read with retry?
		DEBUG_PRINT("partial write: write_blocks\n");
	}

	return BYTES_TO_BLKS(ret);
}

/**
 * Allocates an unused data block, and returns the lba of that data block
 * Since this requires knowing the data region start, we access the global
 * reference to the superblock.
 */
static lba_t reserve_data_block() {
	uint64_t index = first_unset_bit(data_bitmap);
	set_bit(data_bitmap, index);
	// convert our bit number to the actual LBA by adding the data
	// region's starting offset
	return super.super.d_region_start + index;
}


/**
 * Allocates an unused inode, and returns a pointer to that inode via an
 * index into the inode table (which is also the inode's number).
 */
static int reserve_inode() {
	uint64_t inum = first_unset_bit(inode_bitmap);
	set_bit(inode_bitmap, inum);
	inode_table[inum].inode.flags = INODE_IN_USE;
	inode_table[inum].inode.n_links = 0;
	inode_table[inum].inode.size = 0;
	inode_table[inum].inode.blocks = 0;
	return inum;
}

/**
 * allocates a new directory inode by setting the bitmap bits and
 * initializing the inode's type field in the inode_table
 */
static int reserve_dir_inode() {
	uint64_t inum = reserve_inode();
	inode_table[inum].inode.flags |= INODE_TYPE_DIR;
	inode_table[inum].inode.n_links = 2; // . and parent dir
	return inum;
}

/**
 * allocates a new directory inode by setting the bitmap bits and
 * initializing the inode's type field in the inode_table
 */
static int reserve_reg_inode(mode_t mode) {
	int inum = reserve_inode();
	//NOTE:
	//It is probably not best practice to or the mode with two
	//However, fuse seemed to be having permissions errors on writes 
	inode_table[inum].inode.mode = mode;
	// We assume that a parent dir entry will refer to this inode
	inode_table[inum].inode.n_links = 1;
	return inum;
}


/**
 * Called only once during FS initialization:
 *   allocates inode 0 for `/`
 *   creates the data block for the contents of `/` (. and ..)
 */
static int alloc_root_dir() {
	uint64_t inum = reserve_dir_inode();

	// if this is called from an "empty" fs, it should reserve
	// inode number 0.
	assert(inum == ROOT_INUM);

	lba_t data_lba = reserve_data_block();


	// The root dir is a special case, where '.' and '..' both refer to
	// itself. We also require / have inum 0 to simplify our FS layout

	union directory_block *root_data = calloc_blocks(1);

	// init "."
	root_data->dirents[0].inum = ROOT_INUM;
	root_data->dirents[0].is_valid = 1;
	strcpy(root_data->dirents[0].path, ".");
	root_data->dirents[0].path_len = strlen(root_data->dirents[0].path);

	// init ".."
	root_data->dirents[1].inum = ROOT_INUM;
	root_data->dirents[1].is_valid = 1;
	strcpy(root_data->dirents[1].path, "..");
	root_data->dirents[1].path_len = strlen(root_data->dirents[1].path);

	// update the inode's first direct pointer to point to this data
	inode_table[inum].inode.size = 4096;
	inode_table[inum].inode.blocks = 1;
	inode_table[inum].inode.block_ptrs[0] = data_lba;
	inode_table[inum].inode.mode = S_IFDIR;

	struct fuse_context *ctx = fuse_get_context();

	inode_table[inum].inode.uid = ctx->uid;
	inode_table[inum].inode.gid = ctx->gid;

	// write the directory contents to its data block
	write_block(root_data, data_lba);

	// write the data bitmap
	sync_data_bitmap();

	// write back the inode
	write_inode(inum);

	// write back the inode bitmap
	sync_inode_bitmap();

	//free(root_data);
	return 0;
}
//Should this check that n_links is zero?
//It did in the starter code, but now it doesn't in the sample implementation
static void release_inode(struct refs_inode *ino) {
	assert(ino->flags & INODE_IN_USE);
	assert(ino->n_links <= 1);

	ino->flags = INODE_FREE;
	ino->n_links = 0;
	ino->size = 0;
	ino->blocks = 0;
	clear_bit(inode_bitmap, ino->inum);
	// don't bother"clearing" the block pointers because this inode is
	// logically free; future code should never interpret their values
}



static void alloc_inode_table(int num_inodes) {
	size_t itable_bytes = BLKS_TO_BYTES((num_inodes + (INODES_PER_BLOCK - 1)) / INODES_PER_BLOCK);

	inode_table = malloc(itable_bytes);
	assert(inode_table != NULL);

	bzero(inode_table, itable_bytes);

	// initialize each inode
	for (uint64_t i = 0; i < DEFAULT_NUM_INODES; i++) {
		inode_table[i].inode.inum = i;
		inode_table[i].inode.flags = INODE_FREE;
	}
}

static void dump_super(struct refs_superblock *sb) {
	printf("refs_superblock:\n");
	printf("\tblock_size: %"PRIu32"\n", sb->block_size);
	printf("\tnum_inodes: %"PRIu64"\n", sb->num_inodes);
	printf("\timap_start:%"LBA_PRINT"\n", sb->i_bitmap_start);
	printf("\titab_start:%"LBA_PRINT"\n", sb->i_table_start);
	printf("\tnum_d_blks:%"PRIu64"\n", sb->num_data_blocks);
	printf("\tdmap_start:%"LBA_PRINT"\n", sb->d_bitmap_start);
	printf("\tdreg_start:%"LBA_PRINT"\n", sb->d_region_start);
}

static void init_super(struct refs_superblock *sb) {
	// Disk Layout:
	// super | imap | ...inode table... | dmap | ...data blocks... |

	sb->block_size = BLOCK_SIZE;
	sb->num_inodes = DEFAULT_NUM_INODES;
	sb->num_data_blocks = DEFAULT_NUM_DATA_BLOCKS;

	size_t imap_bytes = BLOCK_ROUND_UP(sb->num_inodes);
	lba_t imap_blocks = BYTES_TO_BLKS(imap_bytes);

	lba_t itab_blocks = (DEFAULT_NUM_INODES) / (INODES_PER_BLOCK);

	size_t dmap_bytes = BLOCK_ROUND_UP(sb->num_data_blocks);
	lba_t dmap_blocks = BYTES_TO_BLKS(dmap_bytes);

	sb->i_bitmap_start = 1;
	sb->i_table_start = sb->i_bitmap_start + imap_blocks;
	sb->d_bitmap_start = sb->i_table_start + itab_blocks;
	sb->d_region_start = sb->d_bitmap_start + dmap_blocks;
}

static void* refs_init(struct fuse_conn_info *conn) {
	int ret = 0;

	// check whether we need to initialize an empty file system
	// or if we can populate our existing file system from "disk"
	if (access(DISK_PATH, F_OK) == -1) {
		// In this cond branch, we don't have an existing "file
		// system" to start from, so we initialize one from scratch.
		// Typically, a separate program would do this (e.g., mkfs)
		// but this is not a typical file system...

		printf("creating new disk\n");

		// First, create a new "disk" file from scratch
		disk_fd = open(DISK_PATH, O_CREAT | O_EXCL | O_SYNC | O_RDWR,
			       S_IRUSR | S_IWUSR);
		assert(disk_fd > 0);


		// extend our "disk" file to 10 MiB (per lab spec)
		ret = ftruncate(disk_fd, 10*1024*1024);
		assert(ret >= 0);

		// now initialize the "empty" state of all of our data
		// structures in memory, then write that state to disk

		init_super(&super.super);
		dump_super(&super.super);

		inode_bitmap = allocate_bitmap(super.super.num_inodes);
		assert(inode_bitmap != NULL);

		data_bitmap = allocate_bitmap(super.super.num_data_blocks);
		assert(inode_bitmap != NULL);

		// allocate our inode table memory and populate initial vals
		alloc_inode_table(DEFAULT_NUM_INODES);

		// allocate inode for `/`, create the directory with . and ..
		alloc_root_dir();

		// write superblock
		write_super();

		// done! now we have all of our metadata initialized and
		// written, and we can reinitialize our file system from
		// this on-disk state in future runs.
	} else {
		// In this cond. branch, we have already created an instance
		// of ReFS. Based on the saved state of our FS, we initialize
		// the in-memory state so that we can pick up where we left
		// off.

		// Step 1: open disk and read the superblock

		// Since super is statically allocated, we don't need
		// to do any memory allocation with malloc; we just
		// need to populate its contents by reading them from
		// "disk".  And since we need to access fields in our
		// super in order to know the sizes of our other
		// metadata structures, we read super first.

		disk_fd = open(DISK_PATH, O_SYNC | O_RDWR);
		assert(disk_fd > 0);

		// read superblock
		ret = read_block(&super, 0);
		dump_super(&super.super);


		// Step 2: allocate our other data structures in memory
		// and read from "disk" to populate your data structures

		// bitmaps
		inode_bitmap = allocate_bitmap(super.super.num_inodes);
		assert(inode_bitmap != NULL);

		data_bitmap = allocate_bitmap(super.super.num_data_blocks);
		assert(inode_bitmap != NULL);

		read_bitmap_from_disk(inode_bitmap,
				      super.super.i_bitmap_start);
		dump_bitmap(inode_bitmap);

		read_bitmap_from_disk(data_bitmap,
				      super.super.d_bitmap_start);
		dump_bitmap(data_bitmap);

		//inode table
		alloc_inode_table(super.super.num_inodes);
		ret = read_blocks(inode_table,
				  super.super.num_inodes / INODES_PER_BLOCK,
				  super.super.i_table_start);
		assert(ret == super.super.num_inodes / INODES_PER_BLOCK);
	}

	// before returning you should have your in-memory data structures
	// initialized, and your file system should be able to handle any
	// implemented system call

	printf("done init\n");
	return NULL;
}

void refs_destroy(void* private_data) {
	int synchronize = fsync(disk_fd); //may be necessary to "syncrhonize a file's in core state with storage device"
	if(synchronize!=0) {
		fprintf(stderr, "Errno: %d\n",errno );
		perror("Error with fsync");
	} else {
		int closeDescriptor = close(disk_fd);
		if(closeDescriptor!=0) {
			fprintf(stderr, "Errno: %d\n",errno );
			perror("Error closing file descriptor");
		}
	}
	//size_t itable_bytes = BLKS_TO_BYTES((super.super.num_inodes + (INODES_PER_BLOCK - 1)) / INODES_PER_BLOCK);
	//memset(inode_table, 0, itable_bytes);
	//write_blocks(inode_table,BYTES_TO_BLKS(itable_bytes),super.super.i_table_start);
	//TO CLEAR?
	//memset inode table, data bitmap, inode bitmap, super to zero
	//sync_ all four
	//fsync
//	free(private_data);


	free(inode_table);
	free_bitmap(data_bitmap);
	free_bitmap(inode_bitmap);
}

/*static int refs_destroy(void* private_data)
{
	printf("\n\nrefs_destroy\n\n");
	close(disk_fd);

}*/

static int dirs_in_path(const char *path){
	int cnt = 0;
	int len = strlen(path);
	for(int i=0; i<len; i++){
		if(path[i] == '/'){
			cnt++;
		}
	}
	return cnt;
}

static int inum_from_dirblock(union directory_block *dirblock, const char *path){
	char newpath[MAX_PATH_LEN];
	for(int i=0; i<DIRENTS_PER_BLOCK;i++)
	{
		strcpy(newpath,dirblock->dirents[i].path);
		if (strcmp(path, newpath)== 0 && dirblock->dirents[i].is_valid){
			return dirblock->dirents[i].inum;
		}
	}
	return -1;
}

/* static union directory_block* dirblock_from_inum(int inum){ */
/* 	//malloc? */
/* } */

static void fill_dirblock(int inum, int blocknum, union directory_block *dirblock){
	int block_ptr = inode_table[inum].inode.block_ptrs[blocknum];
	int blocks_read = read_blocks(dirblock,1,block_ptr);
	return;
}


static int inum_from_path(int inum, const char *path)
{
	union directory_block *dirblock = calloc_blocks(1);
	int num_blocks = inode_table[inum].inode.blocks;
	int res = -1;
	for(int i=0; i<num_blocks; i++)
	{
		fill_dirblock(inum, i, dirblock);
		res = inum_from_dirblock(dirblock,path);
		if(res >= 0)
			break;
	}
	free(dirblock);
	return res;
}

static int initialize_reg_inode(int parent_inum, mode_t mode) {
	int inum = reserve_reg_inode(mode);


	// The root dir is a special case, where '.' and '..' both refer to
	// itself. We also require / have inum 0 to simplify our FS layout

	// update the inode's first direct pointer to point to this data
	inode_table[inum].inode.size = 0;
	inode_table[inum].inode.blocks = 0;
	inode_table[inum].inode.aTime = time(NULL);
	inode_table[inum].inode.mTime = time(NULL);
	inode_table[inum].inode.cTime = time(NULL);

	struct fuse_context *ctx = fuse_get_context();

	inode_table[inum].inode.uid = ctx->uid;
	inode_table[inum].inode.gid = ctx->gid;

	// write back the inode
	write_inode(inum);

	// write back the inode bitmap
	sync_inode_bitmap();
	return inum;

}
//Test with bad mode?
static int initialize_dir_inode(int parent_inum, mode_t mode){
	int inum = reserve_dir_inode();
	lba_t data_lba = reserve_data_block();


	// The root dir is a special case, where '.' and '..' both refer to
	// itself. We also require / have inum 0 to simplify our FS layout

	union directory_block *dirblock = calloc_blocks(1);

	// init "."
	dirblock->dirents[0].inum = inum;
	dirblock->dirents[0].is_valid = 1;
	strcpy(dirblock->dirents[0].path, ".");
	dirblock->dirents[0].path_len = strlen(dirblock->dirents[0].path);

	// init ".."
	dirblock->dirents[1].inum = parent_inum;
	dirblock->dirents[1].is_valid = 1;
	strcpy(dirblock->dirents[1].path, "..");
	dirblock->dirents[1].path_len = strlen(dirblock->dirents[1].path);

	// update the inode's first direct pointer to point to this data
	inode_table[inum].inode.size = 4096;
	inode_table[inum].inode.blocks = 1;
	inode_table[inum].inode.block_ptrs[0] = data_lba;
	inode_table[inum].inode.aTime = time(NULL);
	inode_table[inum].inode.mTime = time(NULL);
	inode_table[inum].inode.cTime = time(NULL);
	inode_table[inum].inode.mode = S_IFDIR | mode;

	struct fuse_context *ctx = fuse_get_context();

	inode_table[inum].inode.uid = ctx->uid;
	inode_table[inum].inode.gid = ctx->gid;
	
	// write the directory contents to its data block
	write_block(dirblock, data_lba);

	// write the data bitmap
	sync_data_bitmap();

	// write back the inode
	write_inode(inum);

	// write back the inode bitmap
	sync_inode_bitmap();
	free(dirblock);
	return inum;
}

static int resolve_path(const char* path, int *parent_address, int *child_address){
	char* pathdup = strdup(path);
	int parent_inum = ROOT_INUM;
	char* curr_dirname = NULL;
	char* next_dirname = NULL;
	printf("\n\nPATH,PATHDUP:%s:%s:\n\n",path,pathdup);
	if(path == NULL){
		printf("\n\nNull path\n\n");
		return -ENOENT;
	}
	if (strcmp(pathdup,"/")==0)
	{
		printf("\n\nPATH == /\n\n");
		curr_dirname = ".";
	}
	else
	{
		curr_dirname = strtok(pathdup, "/");
		next_dirname = strtok(NULL, "/");
	}

	//This sets next_dirname to null (thus skipping the while loop) if path is "/"
	//Otherwise, it sets next_dirname to the next dir in the path,
	//Or sets it to null if curr_dirname is the last dir in the path.
	/* printf("") */
	printf("\n\nPRE STRTOK\n\n");
	printf("\n\nPOST STRTOK\n\n");

	int child_inum = inum_from_path(parent_inum,curr_dirname);
	while(next_dirname != NULL ) {
		//If a directory along the path does not exist, return no entry
		if(child_inum < 0) {
			free(pathdup);
			return -ENOENT;

		}


		//Otherwise, move along the path to the next directory
		curr_dirname = next_dirname;
		next_dirname = strtok(NULL, "/");
		parent_inum = child_inum;
		child_inum = inum_from_path(parent_inum,curr_dirname);

		printf("\nCURR_DIRNAME%s\n",curr_dirname);
		printf("\nNEXT_DIRNAME%s\n",next_dirname);

	}
	*parent_address = parent_inum;
	*child_address = child_inum;

	free(pathdup);
	printf("\n\nEND OF GETATTR\n\n");
	return 0;

}



//follow dir_entry.inum ?
static int refs_getattr(const char *path, struct stat *stbuf)
{
	printf("\n");
	printf("\n%s\n",path);
	printf("\n");
	int child_inum, parent_inum;
	resolve_path(path, &parent_inum, &child_inum);
	printf("\n\nPost resolve path: %d\n\n",child_inum);

	if(child_inum < 0){
		printf("\n\nchild_inum less than 0: %d\n\n",child_inum);
		return -ENOENT;
	}
	else{
		printf("\n\n setting stbuf: %d\n\n",child_inum);
		stbuf->st_ino = child_inum;
		/* stbuf->st_mode = inode_table[child_inum].inode.n_links; */
		stbuf->st_mode = inode_table[child_inum].inode.mode; //Needs to be changed
		stbuf->st_nlink = inode_table[child_inum].inode.n_links; //Needs to be changed
		stbuf->st_uid = 0;
		stbuf->st_gid = 0;
		stbuf->st_rdev = 0;
		stbuf->st_size = inode_table[child_inum].inode.size;
		stbuf->st_blksize = BLOCK_SIZE;
		stbuf->st_blocks = inode_table[child_inum].inode.blocks;
		//TODO: Implement modify/change times

		stbuf->st_atime = inode_table[child_inum].inode.aTime;
		stbuf->st_mtime = inode_table[child_inum].inode.mTime;
		stbuf->st_ctime = inode_table[child_inum].inode.cTime;
		printf("\n\nDirectory Found\n\n");
		return 0;
	}


}

static int refs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{

	printf("\n");
	printf("\n%s\n",path);
	printf("\n");

	int child_inum, parent_inum;
	resolve_path(path,&parent_inum,&child_inum);
	if(child_inum < 0){
		return -ENOENT;
	}

	//For some reasone readdir in FUSE is given an "offset," which is the dirent to start reading from.
	//The logic behind this eludes me. But what this means is that we have to start our filling of
	//filler at the offset'th directory. If offset is smaller than the number of directories per block,
	//Since the dirents are stored across multiple blocks with DIRENTS_PER_BLOCK
	//dir entries per block, he offset'th dirent is located at the (offset / DIRENTS_PER_BLOCK)th block,
	//and the (offset % DIRENTS_PER_BLOCK)th dirent within that block.
	int block_start = offset / DIRENTS_PER_BLOCK;
	int offset_start = offset % DIRENTS_PER_BLOCK;
	int num_blocks = inode_table[child_inum].inode.blocks;
	union directory_block  *dirblock = calloc_blocks(1);
	for(int blocknum = block_start; blocknum < num_blocks; blocknum++){
		bzero(dirblock,BLOCK_SIZE); //not sure if this zeroing out is necessary, but wanted to be safe
		fill_dirblock(child_inum,blocknum,dirblock);
		struct dir_entry *dirents = dirblock->dirents;
		int i;
		//if we're at the start block, start at the offset_start'th dirent within that block
		if(blocknum == block_start){
			i = offset_start;
		}
		else{ //otherwise, start at the 0th dirent.
			i=0;
		}
		for(; i<DIRENTS_PER_BLOCK; i++) {
		//	filler(buff, null terminating file name, address of file stat (or null), offset of next directory entry)
			struct stat stbuf;
			// FUSE only looks at the inum and mode fields
			stbuf.st_mode = inode_table[dirents[i].inum].inode.mode;
			stbuf.st_ino = dirents[i].inum;

			if(dirents[i].is_valid) {
				if(filler(buf,dirents[i].path,&stbuf,blocknum*DIRENTS_PER_BLOCK + i+1)) {
					free(dirblock);
					return 0;
				}

			}
		}
	}

	free(dirblock);
	return 0;
}



int refs_access(const char* path, int mask){
	printf("\n\nStart of refs_access: \n\n");
	printf("\n mask: \n %d", mask);
	printf("\n Path: \n %s", path);

	int ret;

	int inum,parent_inum;

	// look up the path
	ret = resolve_path(path, &parent_inum,&inum);

	if (ret < 0) {
		return -ENOENT;
	}

	// if mask == F_OK, just check existence
	if (mask == F_OK)
		return 0;

	// Strategy: cobble together all of the capailitie that
	// the user posses based on their idenitity
	// This means we compare the caller's uid/gid against the
	// uid/gid of the file (we look at "other" regardless)
	struct fuse_context *ctx = fuse_get_context();

	// assemble all allowed permissions into one trio of bits

	int allowed_mode = 0;
	printf("\n\n Got context: \n\n");
	
	// user matches
	if (ctx->uid == inode_table[inum].inode.uid) {
		allowed_mode |= inode_table[inum].inode.mode & S_IRUSR ?
			R_OK : 0;
		allowed_mode |= inode_table[inum].inode.mode & S_IWUSR ?
			W_OK : 0;
		allowed_mode |= inode_table[inum].inode.mode & S_IXUSR ?
			X_OK : 0;
	}

	// group matches
	if (ctx->gid == inode_table[inum].inode.gid) {
		allowed_mode |= inode_table[inum].inode.mode & S_IRGRP ?
			R_OK : 0;
		allowed_mode |= inode_table[inum].inode.mode & S_IWGRP ?
			W_OK : 0;
		allowed_mode |= inode_table[inum].inode.mode & S_IXGRP ?
			X_OK : 0;
	}
	printf("\n\n after uid and gid: \n\n");


	// always check "other"
	allowed_mode |= inode_table[inum].inode.mode & S_IROTH ? R_OK : 0;
	allowed_mode |= inode_table[inum].inode.mode & S_IWOTH ? W_OK : 0;
	allowed_mode |= inode_table[inum].inode.mode & S_IXOTH ? X_OK : 0;


	// confirm that if they ask for read permissions,
	// they are allowed to read, ...
	if ((mask & R_OK) && !(allowed_mode & R_OK))
		    return -EACCES;
	if ((mask & W_OK) && !(allowed_mode & W_OK))
		    return -EACCES;
	if ((mask & X_OK) && !(allowed_mode & X_OK))
		    return -EACCES;
	printf("\n\n returning 0: \n\n");

	return 0;	
}

//int create_inode()
//Returns inode of the last directory in a path, or -1 if that directory does not exist
int first_available_dirnum(int parent_inum, int *open_dirnum, int *open_blocknum){
	union directory_block *dirblock = calloc_blocks(1);
	int num_blocks = inode_table[parent_inum].inode.blocks;
	int blocknum;

	//loop through all the blocks and dirents and find the first dirent that's not populated.
	for(blocknum = 0; blocknum < num_blocks; blocknum++){
		bzero(dirblock,BLOCK_SIZE);
		fill_dirblock(parent_inum,blocknum,dirblock);
		for(int i=0;i<DIRENTS_PER_BLOCK;i++){
			if(dirblock->dirents[i].is_valid==0){
				//If is_valid==0, the dirent is open. thus, set open_dirnum to i, and open_blocknum to blocknum
				*open_dirnum = i;
				*open_blocknum = blocknum;
				free(dirblock);
				write_inode(parent_inum);
				return 0;
			}
		}
	}
	//TODO: Deal with indirect blocks, check to make sure blocks are available
	//If we reach this point, we need to allocate a new block
	if(blocknum < NUM_DIRECT)
	{
		//Allocate new block and write it
		lba_t lba = reserve_data_block();
		// update inode
		inode_table[parent_inum].inode.block_ptrs[blocknum] = lba;
		inode_table[parent_inum].inode.blocks++;
		bzero(dirblock, BLOCK_SIZE);
		write_block(dirblock,inode_table[parent_inum].inode.block_ptrs[blocknum]);

		//first avaialable dirnum is zero, since the block was just allocated and thus is empty
		*open_dirnum = 0;
		int ret = sync_data_bitmap();
		assert(ret == 0);
		//first block with an available directory is the one we just allocated
		*open_blocknum = blocknum;
		free(dirblock);
		write_inode(parent_inum);
		return 0;
	}
	write_inode(parent_inum);
	free(dirblock);
	return -1;

}
int update_parent(int parent_inum,int child_inum,char* curr_dirname, char is_dir){
	//TODO: Update to work with multiple blocks in an inode

	// if child
	if (!S_ISDIR(inode_table[parent_inum].inode.mode)) {
		// parent must be a directory!
		DEBUG_PRINT("non-directory inode when resolving %s\n",
			    curr_dirname);
		return -ENOTDIR;
	}

	int dirnum, blocknum;
	int ret = first_available_dirnum(parent_inum, &dirnum, &blocknum);
	assert(ret == 0);
	union directory_block *dirblock = calloc_blocks(1);
	fill_dirblock(parent_inum,blocknum,dirblock);



    dirblock->dirents[dirnum].is_valid = 1;
    strcpy(dirblock->dirents[dirnum].path,curr_dirname);
    dirblock->dirents[dirnum].inum = child_inum;
    dirblock->dirents[dirnum].path_len = strlen(curr_dirname);

	write_block(dirblock,inode_table[parent_inum].inode.block_ptrs[blocknum]);
	inode_table[parent_inum].inode.mTime = time(NULL);
	inode_table[parent_inum].inode.aTime = time(NULL);
	if(is_dir) {
		inode_table[parent_inum].inode.n_links++;
	}

	free(dirblock);

	return 0;
}

int refs_mkdir(const char* path, mode_t mode){
	char* pathdup = strdup(path);
	int child_inum, parent_inum;
	resolve_path(path, &parent_inum, &child_inum);
	char* curr_dirname = basename(pathdup);

	if(child_inum >= 0) {
				free(pathdup);
				return -EEXIST;
	}

	printf("\n CODE TO MAKE DIRECTORY. \n");

	child_inum = initialize_dir_inode(parent_inum, mode);
	assert(child_inum >= 0);
	int parent_created = update_parent(parent_inum,child_inum,curr_dirname,1);
	assert(parent_created == 0);
	free(pathdup);
	return 0;
}


int remove_child_abspath(struct refs_inode *parent, struct refs_inode *child, const char *abspath){
	int ret=0;
	char *pathdup = strdup(abspath);
	if (pathdup == NULL)
		return -ENOMEM;

	char *base = basename(pathdup);

	union directory_block *dirblock = calloc_blocks(1);

	if (dirblock == NULL){
		free(pathdup);
		return -ENOMEM;
	}

	for(int blocknum = 0; blocknum < parent->blocks; blocknum++){
		ret = read_block(dirblock, parent->block_ptrs[blocknum]);
		if(ret != 1){
			free(pathdup);
			free(dirblock);
			return -EIO;
		}
		for(int i=0; i<DIRENTS_PER_BLOCK;i++){
			if(dirblock->dirents[i].is_valid && strcmp(base, dirblock->dirents[i].path) == 0){
				//This is the directory to be removed
				ret = 0;
				dirblock->dirents[i].is_valid=0;
				ret = write_block(dirblock, parent->block_ptrs[blocknum]);
				if(ret != 1){
					free(pathdup);
					free(dirblock);
					return -EIO;
				}
				parent->n_links--;
				ret = write_inode(parent->inum);
				assert(ret==1);
				child->n_links--;
				ret = write_inode(child->inum);
				assert(ret==1);
				free(dirblock);
				free(pathdup);
				return 0;
			}
		}
	}
	//if we reach this point, we did not find the child.
	free(dirblock);
	free(pathdup);
	return -ENOENT;

}
//Taken from refs sample code
static int dir_isempty(struct refs_inode *dir) {
	union directory_block data;

	for (int b = 0; b < dir->blocks; b++) {

		int ret = read_block(&data, dir->block_ptrs[b]);
		assert(ret == 1);

		for (int d = 0; d < DIRENTS_PER_BLOCK; d++) {
			// if a valid entry is not . or .., not empty
			if (data.dirents[d].is_valid &&
			    !((strcmp(".", data.dirents[d].path) == 0) ||
			      (strcmp("..", data.dirents[d].path) == 0))) {
				return 0;
			}
		}
	}

	// if we got here, we have checked every entry in every block,
	// and the directyr is empty. return "true"
	return 1;
}

//Taken from refs sample code
static void release_data_block(lba_t lba) {
	assert(lba >= super.super.d_region_start);
	clear_bit(data_bitmap, lba - super.super.d_region_start);
}

int do_rmdir(int parent_inum, int child_inum, const char *abspath){
	struct refs_inode *parent_inode = &(inode_table[parent_inum].inode);
	struct refs_inode *child_inode = &(inode_table[child_inum].inode);
	//confirm child directory is empty with exception of . and ..
	if (!dir_isempty(child_inode)) {
		return -ENOTEMPTY;
	}

	//remove child from parent directory

	int ret = remove_child_abspath(parent_inode, child_inode, abspath);
	if(ret != 0){
		return ret;
	}

	//return child's inode

	//return path's blocks

	//clear path's inode (mark as unused)
	clear_bit(inode_bitmap,child_inum);
	for (int b = 0; b < child_inode->blocks; b++)
		release_data_block(child_inode->block_ptrs[b]);

	release_inode(child_inode);
	return 0;
}

int refs_rmdir(const char* path, mode_t mode) {
	//remove directory:
	////update inode
	////free datablocks
	////update parent's isvalid
	////update parent's numlinks
	int ret=0;
	int parent_inum, child_inum;
	/* char* pathdup = strdup(path); */
	ret = resolve_path(path, &parent_inum, &child_inum);
	/* char* curr_dirname = basename(pathdup); */
	if(ret != 0){
		return ret;
	}
	if(child_inum < 0)
		return -ENOENT;
	struct refs_inode *child_inode = &(inode_table[child_inum].inode);
	if (!S_ISDIR(child_inode->mode)) {
			return -ENOTDIR;
		}
	printf("\n REMOVING DIRECTORY. \n");

	/* struct refs_inode *parent_inode = &inode_table[parent_inum].inode; */

	//Update inode bitmap, inode table, parent inode
	//Remove child inode, set to false
	//Check child is a directory
	//check man 7 inode to see S_IFDIR.
	/* if (!S_IFDIR(child_inode->mode)) */
	/* 	return -ENOTDIR; */
	return do_rmdir(parent_inum, child_inum, path);

}
int refs_open(const char* path, struct fuse_file_info* fi){
	int req_mode = 0;
	/* if ((fi->flags & O_RDONLY) == O_RDONLY) */
		req_mode |= R_OK;
	/* if ((fi->flags & O_WRONLY) == O_WRONLY) */
		req_mode |= W_OK;
	/* if ((fi->flags & O_RDWR) == O_RDWR) */
		/* req_mode |= R_OK | W_OK; */

	return refs_access(path,req_mode);
}


int refs_chmod(const char* path, mode_t mode){
	int child_inum, parent_inum;
	int ret = resolve_path(path, &parent_inum, &child_inum);
	if(ret != 0)
		return ret;
	inode_table[child_inum].inode.mode = mode;
	return 0;

}

int refs_release(){
	return 0;
}
int refs_fgetattr(){
	return 0;
	//return getattr
}
// int release_direct_blocks(struct refs_inode *child_inode){
// 	int num_blocks;
// 	if(child_inode->blocks <= NUM_DIRECT) {
// 		num_blocks = child_inode->blocks;
// 	} else {
// 		num_blocks = NUM_DIRECT;
// 	}
//
// 	refs_truncate()
// 	for(int i=0; i<num_blocks;i++) {
// 		release_data_block(child_inode->block_ptrs[i]);
// 	}
//
// }
// int release_indirect_blocks(struct refs_inode *child_inode){
//
// }


/**
 * If the file does not already exist, make an empty inode for a regular
 * file.
 */
 //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
 ///NOTE: FROM BILL JANNENS CODE
static int refs_mknod(const char* abspath, mode_t mode, dev_t rdev) {

	int parent_inum = 0;
	int child_inum = 0;

	int ret = resolve_path(abspath, &parent_inum, &child_inum);


	// error if the file exists
	if (child_inum >= 0)
		return -EEXIST;

	if (ret != 0) {
			DEBUG_PRINT("%s failed to resolve parent path %s\n",
				    __func__, abspath);
			return ret;
		}

	// create a new inode for the child
 child_inum = initialize_reg_inode(parent_inum, mode);
 char *duplicate = strdup(abspath);
 if (duplicate == NULL)
	 return -ENOMEM;
 char *base = basename(duplicate);

 ret = update_parent(parent_inum,child_inum,base,0);

	// allocate a new entry in the parent's directory contents

	free(duplicate);
	return ret;
}

int refs_create(const char* path, mode_t mode,
		       struct fuse_file_info *fi) {
	return refs_mknod(path, S_IFREG | mode, 0);
}

int do_truncate(int child_inum, off_t size)
{

	struct refs_inode *child_inode = &inode_table[child_inum].inode;

	//Allocate new blocks if size > child_inode.blocks*4096
	//Free blocks if size <= (child_inode.blocks-1)*4096

	//Division rounding up
	int block_count = BYTES_TO_BLKS((BLOCK_ROUND_UP(size)));
	if(block_count == child_inode->blocks){
		//If we have the right number of blocks, adjust size and return
		child_inode->size = size;
	}
	else if(block_count < child_inode->blocks){
		//Free some blocks
		lba_t *indirect_block;
		if(child_inode->blocks > NUM_DIRECT) {
			indirect_block = malloc_blocks(1);
			// child_inode->block_ptrs[child_inode->blocks] = lba;
			read_block(indirect_block,child_inode->block_ptrs[NUM_DIRECT]);
		}

		while(child_inode->blocks > block_count){
			if(child_inode->blocks <= NUM_DIRECT){
				release_data_block(child_inode->block_ptrs[child_inode->blocks-1]);
				child_inode->blocks--;
			}else{ 

				int indirect_num = child_inode->blocks-1-NUM_DIRECT;
				release_data_block(indirect_block[indirect_num]);
				if(indirect_num == 0) {
					release_data_block(child_inode->block_ptrs[child_inode->blocks-1]);
				}
				child_inode->blocks--;
			}
		}
		child_inode->size = size;
	}
	else{
		//allocate new blocks
		union directory_block *empty_block = calloc_blocks(1);
		while(child_inode->blocks < block_count){
			if(child_inode->blocks < NUM_DIRECT){
				lba_t lba = reserve_data_block();
				// update inode
				child_inode->block_ptrs[child_inode->blocks] = lba;
				write_block(empty_block,child_inode->block_ptrs[child_inode->blocks]);
				child_inode->blocks++;
			}else{
				if(child_inode->blocks < NUM_DIRECT+1){
					lba_t indirect_block_lba = reserve_data_block();
					child_inode->block_ptrs[child_inode->blocks] = indirect_block_lba;
					/* write_block(empty_block,indirect_block_lba); */
				}
					lba_t* indirect_block = calloc_blocks(1);
					read_block(indirect_block,child_inode->block_ptrs[NUM_DIRECT]);
					int indirect_index = child_inode->blocks - NUM_DIRECT;

					lba_t inside_indirect = reserve_data_block();
					write_block(empty_block,inside_indirect);
					indirect_block[indirect_index] = inside_indirect;
					write_block(indirect_block,child_inode->block_ptrs[NUM_DIRECT]);
					child_inode->blocks++;
					free(indirect_block);
			}
		}
		free(empty_block);
		child_inode->size = size;
	}
	int ret = sync_data_bitmap();
	assert(ret == 0);

	write_inode(child_inum);

	return 0;

	
}

int refs_truncate(const char* path, off_t size){
	printf("\n\nTRUNCATE\n\n");
	//resolve path to find the inode
	//set inode.size to size
	//allocate or free blocks as necessary
	int ret = refs_access(path,W_OK);
	if (ret != 0){
		return ret;
	}
	int child_inum, parent_inum;
	ret = resolve_path(path,&parent_inum,&child_inum);
	if(ret != 0)
		return ret;
	return do_truncate(child_inum, size);
}

int do_write(int child_inum, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi){
	struct refs_inode *child_inode = &inode_table[child_inum].inode;
	int curr_block = BYTES_TO_BLKS(offset);
	int curr_byte = offset % BLOCK_SIZE;
	//Deal with indirect later
	char *block_buffer = calloc_blocks(1);
	int ret;
	if(curr_block < NUM_DIRECT){
		//Create some block
		//Write that block to disk using write_block

		//Read in the starting block
		ret = read_block(block_buffer,child_inode->block_ptrs[curr_block]);
		assert(ret == 1);
///////////////////////////////////////////////////////////////////////////////////
		//Write to the starting block starting at offset curr_byte
		int copy_size;
		if(size > BLOCK_SIZE - curr_byte){
			copy_size = BLOCK_SIZE-curr_byte;
		}
		else{
			copy_size = size;
		}
		memcpy(block_buffer + curr_byte,buf,copy_size);
		write_block(block_buffer,child_inode->block_ptrs[curr_block]);
		free(block_buffer);
		sync_data_bitmap();
		int remaining_size = size - copy_size;
		if(remaining_size == 0){

			return copy_size;
		}
		else if(remaining_size > 0){
			return copy_size + do_write(child_inum, buf + copy_size, remaining_size, offset + copy_size, fi);
		}
		else {
			printf("Error: This should never be reached. Math error somewhere");
			return -ENOSYS;
		}
	}
	else{
		//Write to indirect block
		//Only have one indirect block
		//So we can assume that the indirect block is stored at block_ptrs[NUM_DIRECT]
		lba_t *indirect_block = calloc_blocks(1);
		ret = read_block(indirect_block,child_inode->block_ptrs[NUM_DIRECT]);
		lba_t indirect_lba = indirect_block[curr_block-NUM_DIRECT];
		int copy_size;
		if(size > BLOCK_SIZE - curr_byte){
			copy_size = BLOCK_SIZE-curr_byte;
		}
		else{
			copy_size = size;
		}
		read_block(block_buffer,indirect_lba);
		memcpy(block_buffer + curr_byte,buf,copy_size);
		write_block(block_buffer,indirect_lba);
		free(indirect_block);
		free(block_buffer);
		sync_data_bitmap();
		int remaining_size = size - copy_size;
		if(remaining_size == 0){
			return copy_size;
		}
		else if(remaining_size > 0){
			//NOTE: Recursion likely adds some overhead but made implementation easier
			return copy_size + do_write(child_inum, buf + copy_size, remaining_size, offset + copy_size, fi);
		}
		else {
			printf("Error: This should never be reached. Math error somewhere");
			return -ENOSYS;
		}
	}
}

int refs_write(const char* path, const char *buf, size_t size, off_t offset, struct fuse_file_info* fi){
	int ret = refs_access(path, W_OK);
	if(ret != 0){
		return ret;
	}
	int child_inum,parent_inum;
	ret = resolve_path(path,&parent_inum,&child_inum);
	if(ret != 0)
		return -ENOENT;
	if(offset + size > inode_table[child_inum].inode.size){
		ret = refs_truncate(path,offset + size);
		if(ret != 0){
			return ret;
		}
	}
	return do_write(child_inum, buf, size, offset, fi);
}

int do_read(int child_inum, char *buf, size_t size, off_t offset, struct fuse_file_info* fi){
	struct refs_inode *child_inode = &inode_table[child_inum].inode;
	if(child_inum < 0)
		return -ENOENT;
	int curr_block = BYTES_TO_BLKS(offset);
	int curr_byte = offset % BLOCK_SIZE;
	//Deal with indirect later

	char *block_buffer = calloc_blocks(1);
	if(curr_block < NUM_DIRECT){
		//TODO: Put loop in to write more than one block at a time;
		//Create some block
		//Write that block to disk using write_block

		//Read in the starting block
		int ret = read_block(block_buffer,child_inode->block_ptrs[curr_block]);
		assert(ret == 1);
		int copy_size;
		//TODO: also factor in how much as been read
		if(size > BLOCK_SIZE - curr_byte){
			copy_size = BLOCK_SIZE-curr_byte;
		}
		else{
			copy_size = size;
		}
///////////////////////////////////////////////////////////////////////////////////
		//Write to the starting block starting at offset curr_byte
		memcpy(buf,block_buffer+curr_byte,copy_size);
		free(block_buffer);
		int remaining_size = size-copy_size;
		if(remaining_size == 0){
			return copy_size;
		}
		else if(remaining_size > 0){
			//NOTE: Recursion likely adds some overhead but made implementation easier
			return copy_size + do_read(child_inum, buf + copy_size, remaining_size, offset + copy_size, fi);
		}
		else {
			printf("Error: This should never be reached. Math error somewhere");
			return -ENOSYS;
		}

	}
	else{
		lba_t *indirect_block = calloc_blocks(1);
		int ret = read_block(indirect_block,child_inode->block_ptrs[NUM_DIRECT]);
		assert(ret==1);
		lba_t indirect_lba = indirect_block[curr_block-NUM_DIRECT];
		ret = read_block(block_buffer,indirect_lba);
		int copy_size;
		if(size > BLOCK_SIZE - curr_byte){
			copy_size = BLOCK_SIZE-curr_byte;
		}
		else{
			copy_size = size;
		}

		memcpy(buf,block_buffer+curr_byte,copy_size);
		free(indirect_block);
		//TODO: factor out before if
		free(block_buffer);
		int remaining_size = size-copy_size;
		if(remaining_size == 0){
			return copy_size;
		}
		else if(remaining_size > 0){
			return copy_size + do_read(child_inum, buf + copy_size, remaining_size, offset + copy_size, fi);
		}
		else {
			printf("Error: This should never be reached. Math error somewhere");
			return -ENOSYS;
		}

	}
}

int refs_read(const char* path, char *buf, size_t size, off_t offset, struct fuse_file_info* fi){
	int ret = refs_access(path, R_OK);
	if(ret != 0){
		return ret;
	}

	int child_inum,parent_inum;
	ret = resolve_path(path,&parent_inum,&child_inum);
	if(ret != 0){
		return -ENOENT;
	}
	return do_read(child_inum, buf, size, offset, fi);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int do_unlink(int parent_inum,int child_inum, const char *path) {
	struct refs_inode *parent_inode = &inode_table[parent_inum].inode;
	struct refs_inode *child_inode = &inode_table[child_inum].inode;
	int ret = remove_child_abspath(parent_inode,child_inode,path);
	if(ret)
		return ret;
	if(child_inode->n_links > 0)
		return 0;
	ret = do_truncate(child_inum,0);
	
	
	clear_bit(inode_bitmap,child_inum);
	release_inode(child_inode);
	return 0;
}

int refs_unlink(const char *path){
	int parent_inum,child_inum;
	int ret = resolve_path(path,&parent_inum,&child_inum);
	if(ret!=0)
		return ret;
	if(S_ISDIR(inode_table[child_inum].inode.mode)) {
		return -EISDIR;
	}

	do_unlink(parent_inum,child_inum,path);
	//resolve pathdup
	//get parent of child inode
	//remove child inode

	return 0;
}


//Close, fgetattr, and ____ are very short
// You should implement the functions that you need, but do so in a
// way that lets you incrementally test.
static struct fuse_operations refs_operations = {
	.init		= refs_init,
	.destroy    = refs_destroy,
	.getattr	= refs_getattr,
       	.fgetattr	= NULL,
	.access		= refs_access,
	.readlink	= NULL,
	.readdir	= refs_readdir,
	.mknod		= refs_mknod,
	.create		= refs_create,
	.mkdir		= refs_mkdir,
	.symlink	= NULL,
	.unlink		= refs_unlink,
	.rmdir		= refs_rmdir,
	.rename		= NULL,
	.link		= NULL,
	.chmod		= refs_chmod,
	.chown		= NULL,
	.truncate	= refs_truncate,
	.utimens	= NULL,
	.open		= NULL,
	.read		= refs_read,
	.write		= refs_write,
	.statfs		= NULL,
	.release	= refs_release,
	.fsync		= NULL,
#ifdef HAVE_SETXATTR
	.setxattr	= NULL,
	.getxattr	= NULL,
	.listxattr	= NULL,
	.removexattr	= NULL,
#endif
};



int main(int argc, char *argv[]) {
	INODE_SIZE_CHECK;
	DIRENT_SIZE_CHECK;
	umask(0);
	return fuse_main(argc, argv, &refs_operations, NULL);
}


#define _GNU_SOURCE
#define FUSE_USE_VERSION 34

#include <errno.h>
#include <fuse_lowlevel.h>
#include <fcntl.h>
#include <float.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct errc_inode {
	char name[NAME_MAX];
	struct stat st;
	int memfd;
};

#define ERRC_MIN_INO 3
static int ino_unused = ERRC_MIN_INO;
#define ERRC_MAX_INO 100
static struct errc_inode inodes[ERRC_MAX_INO];
#define INODE(k) (&inodes[k])

static uid_t exim_uid = 93;
static gid_t exim_gid = 93;

static void errc_init(void *userdata, struct fuse_conn_info *conn)
{
	strcpy(INODE(1)->name, ".");
	INODE(1)->st = (struct stat){
	    .st_mode = S_IFDIR | 0755,
	    .st_uid = exim_uid,
	    .st_gid = exim_gid,
	};
	strcpy(INODE(2)->name, "..");
	INODE(2)->st = (struct stat){
	    .st_mode = S_IFDIR | 0755,
	    .st_uid = exim_uid,
	    .st_gid = exim_gid,
	};

	for (int ino = 0; ino < ERRC_MAX_INO; ino++) {
		INODE(ino)->memfd = -1;
	}
}

static void errc_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	if (ino < ERRC_MIN_INO || ino >= ERRC_MAX_INO) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	struct errc_inode *inode = INODE(ino);
	int err = 0, namelen = strlen(inode->name);
	if (namelen > 2 && inode->name[namelen-2] == '-' && inode->name[namelen-1] == 'D')
		err = EIO;

	fuse_reply_err(req, err);
}

static void errc_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	if (parent != FUSE_ROOT_ID) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	if (strlen(name) >= NAME_MAX) {
		fuse_reply_err(req, ENAMETOOLONG);
		return;
	}

	for (int ino = ERRC_MIN_INO; ino < ERRC_MAX_INO; ino++) {
		if (strcmp(INODE(ino)->name, name) == 0) {
			fuse_reply_entry(req, &(struct fuse_entry_param){.attr = INODE(ino)->st,
									 .attr_timeout = DBL_MAX,
									 .entry_timeout = DBL_MAX,
									 .ino = ino});
			return;
		}
	}

	fuse_reply_err(req, ENOENT);
}

static void errc_mknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev)
{
	if (!S_ISREG(mode) || parent != FUSE_ROOT_ID) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	if (strlen(name) >= NAME_MAX) {
		fuse_reply_err(req, ENAMETOOLONG);
		return;
	}

	for (int ino = ERRC_MIN_INO; ino < ERRC_MAX_INO; ino++) {
		if (strcmp(INODE(ino)->name, name) == 0) {
			fuse_reply_err(req, EEXIST);
			return;
		}
	}

	int ino = ino_unused++;
	struct errc_inode *inode = INODE(ino);
	strcpy(inode->name, name);
	inode->st.st_ino = ino;
	inode->st.st_mode = mode;
	inode->st.st_uid = exim_uid;
	inode->st.st_gid = exim_gid;
	inode->memfd = memfd_create(name, MFD_CLOEXEC);
	fuse_reply_entry(req, &(struct fuse_entry_param){.attr = inode->st,
							 .attr_timeout = DBL_MAX,
							 .entry_timeout = DBL_MAX,
							 .ino = ino});
}

static void errc_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off,
		       struct fuse_file_info *fi)
{
	if (ino < ERRC_MIN_INO || ino >= ERRC_MAX_INO) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	ssize_t ret = pwrite(INODE(ino)->memfd, buf, size, off);
	if (ret < 0)
		fuse_reply_err(req, errno);
	else
		fuse_reply_write(req, ret);

	if (off + size > INODE(ino)->st.st_size)
		INODE(ino)->st.st_size = off + size;
}

static void errc_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	if (ino < ERRC_MIN_INO || ino >= ino_unused) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	fuse_reply_open(req, fi);
}

static void errc_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	if (ino >= ino_unused) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	fuse_reply_attr(req, &INODE(ino)->st, DBL_MAX);
}

static void errc_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
		      struct fuse_file_info *fi)
{
	if (ino < ERRC_MIN_INO || ino >= ino_unused) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	size_t mapping_length = MIN(INODE(ino)->st.st_size - off, size);
	void *addr = mmap(NULL, mapping_length, PROT_READ, MAP_PRIVATE, INODE(ino)->memfd, off);
	if (addr == MAP_FAILED) {
		fuse_reply_err(req, errno);
		return;
	}
	fuse_reply_buf(req, addr, mapping_length);
	munmap(addr, mapping_length);
}

static void errc_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	if (parent != FUSE_ROOT_ID) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	if (strlen(name) >= NAME_MAX) {
		fuse_reply_err(req, ENAMETOOLONG);
		return;
	}

	int err = ENOENT;
	for (int ino = ERRC_MIN_INO; ino < ERRC_MAX_INO; ino++) {
		if (strcmp(INODE(ino)->name, name) == 0) {
			INODE(ino)->name[0] = '\0';
			err = 0;
			break;
		}
	}
	fuse_reply_err(req, err);
}

static void errc_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set,
			 struct fuse_file_info *fi)
{
	if (ino < ERRC_MIN_INO || ino >= ino_unused) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	struct errc_inode *inode = INODE(ino);
	if (((to_set & FUSE_SET_ATTR_SIZE) && !S_ISREG(inode->st.st_mode)) ||
	    ((to_set & FUSE_SET_ATTR_MODE) &&
	     (inode->st.st_mode & S_IFMT) != (attr->st_mode & S_IFMT))) {
		fuse_reply_err(req, EPERM);
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	if (to_set & FUSE_SET_ATTR_MODE)
		inode->st.st_mode = attr->st_mode;
	if (to_set & FUSE_SET_ATTR_UID)
		inode->st.st_uid = attr->st_uid;
	if (to_set & FUSE_SET_ATTR_GID)
		inode->st.st_gid = attr->st_gid;
	if (to_set & FUSE_SET_ATTR_SIZE)
		inode->st.st_size = attr->st_size;
	if (to_set & FUSE_SET_ATTR_ATIME)
		inode->st.st_atim = attr->st_atim;
	if (to_set & FUSE_SET_ATTR_ATIME_NOW)
		inode->st.st_atim = now;
	if (to_set & FUSE_SET_ATTR_MTIME)
		inode->st.st_mtim = attr->st_mtim;
	if (to_set & FUSE_SET_ATTR_MTIME_NOW)
		inode->st.st_mtim = now;
	if (to_set & FUSE_SET_ATTR_CTIME)
		inode->st.st_ctim = attr->st_ctim;

	// #define FUSE_SET_ATTR_FORCE	(1 << 9)
	// #define FUSE_SET_ATTR_KILL_SUID	(1 << 11)
	// #define FUSE_SET_ATTR_KILL_SGID	(1 << 12)
	// #define FUSE_SET_ATTR_FILE	(1 << 13)
	// #define FUSE_SET_ATTR_KILL_PRIV	(1 << 14)
	// #define FUSE_SET_ATTR_OPEN	(1 << 15)
	// #define FUSE_SET_ATTR_TIMES_SET	(1 << 16)
	// #define FUSE_SET_ATTR_TOUCH	(1 << 17)

	fuse_reply_attr(req, &inode->st, DBL_MAX);
}

static void errc_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname,  unsigned int flags)
{
	if (flags != 0) {
		fuse_reply_err(req, EINVAL); //fixme
		return;
	}

	if (parent != FUSE_ROOT_ID || newparent != FUSE_ROOT_ID) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	if (strlen(name) >= NAME_MAX || strlen(newname) >= NAME_MAX) {
		fuse_reply_err(req, ENAMETOOLONG);
		return;
	}

	int err = ENOENT;
	for (int ino = ERRC_MIN_INO; ino < ERRC_MAX_INO; ino++) {
		if (strcmp(INODE(ino)->name, name) == 0) {
			strcpy(INODE(ino)->name, newname);
			err = 0;
			break;
		}
	}

	fuse_reply_err(req, err);
}

static struct fuse_lowlevel_ops errc_ll_oper = {
    .init = errc_init,
    //    .destroy = errc_destroy,
    .lookup = errc_lookup,
    .getattr = errc_getattr,
    .mknod = errc_mknod,
    .unlink = errc_unlink,
    .open = errc_open,
    .read = errc_read,
    .write = errc_write,
    .flush = errc_flush,
    .setattr = errc_setattr,
	.rename = errc_rename,
};

// c&p from example/hello_ll.c
int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_session *se;
	struct fuse_cmdline_opts opts;
	struct fuse_loop_config config;
	int ret = -1;

	if (fuse_parse_cmdline(&args, &opts) != 0)
		return 1;
	if (opts.show_help) {
		printf("usage: %s [options] <mountpoint>\n\n", argv[0]);
		fuse_cmdline_help();
		fuse_lowlevel_help();
		ret = 0;
		goto err_out1;
	} else if (opts.show_version) {
		printf("FUSE library version %s\n", fuse_pkgversion());
		fuse_lowlevel_version();
		ret = 0;
		goto err_out1;
	}

	if (opts.mountpoint == NULL) {
		printf("usage: %s [options] <mountpoint>\n", argv[0]);
		printf("       %s --help\n", argv[0]);
		ret = 1;
		goto err_out1;
	}

	se = fuse_session_new(&args, &errc_ll_oper, sizeof(errc_ll_oper), NULL);
	if (se == NULL)
		goto err_out1;

	if (fuse_set_signal_handlers(se) != 0)
		goto err_out2;

	if (fuse_session_mount(se, opts.mountpoint) != 0)
		goto err_out3;

	fuse_daemonize(opts.foreground);

	/* Block until ctrl+c or fusermount -u */
	if (opts.singlethread)
		ret = fuse_session_loop(se);
	else {
		config.clone_fd = opts.clone_fd;
		config.max_idle_threads = opts.max_idle_threads;
		ret = fuse_session_loop_mt(se, &config);
	}

	fuse_session_unmount(se);
err_out3:
	fuse_remove_signal_handlers(se);
err_out2:
	fuse_session_destroy(se);
err_out1:
	free(opts.mountpoint);
	fuse_opt_free_args(&args);

	return ret ? 1 : 0;
}

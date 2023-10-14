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
#include <sys/stat.h>
#include <unistd.h>

struct errc_inode {
	char name[NAME_MAX];
	mode_t st_mode;
	nlink_t st_nlink;
	uint64_t nlookup;
	int memfd;
};

#define ERRC_MIN_INO 3
static int ino_unused = ERRC_MIN_INO;
#define ERRC_MAX_INO 100
static struct errc_inode inodes[ERRC_MAX_INO];
#define INODE(k) (&inodes[k])

static void errc_init(void *userdata, struct fuse_conn_info *conn)
{
	strcpy(INODE(1)->name, ".");
	INODE(1)->st_mode = S_IFDIR | 0755;
	INODE(1)->nlookup = 2;
	strcpy(INODE(2)->name, "..");
	INODE(2)->st_mode = S_IFDIR | 0755;
	INODE(2)->nlookup = 2;

	for (int ino = 0; ino < ERRC_MAX_INO; ino++) {
		INODE(ino)->memfd = -1;
	}
}

static void errc_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	int err = 0;
	if (INODE(ino)->nlookup == 0) {
		/* Assume that flush to network storage yields error here. */
		err = EIO;
	}
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
			INODE(ino)->nlookup++;
			fuse_reply_entry(req,
					 &(struct fuse_entry_param){
					     .attr = (struct stat){.st_ino = ino, .st_mode = INODE(ino)->st_mode},
					     .attr_timeout = DBL_MAX,
					     .entry_timeout = DBL_MAX,
					     .ino = ino});
			return;
		}
	}

	fuse_reply_err(req, ENOENT);
}

static void errc_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
	INODE(ino)->nlookup -= nlookup;
	fuse_reply_none(req);
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

	int ino = ino_unused++;
	struct errc_inode *inode = INODE(ino);
	strcpy(inode->name, name);
	inode->st_mode = mode;
	inode->nlookup = 1;
	inode->memfd = memfd_create(name, MFD_CLOEXEC);
	fuse_reply_entry(
	    req, &(struct fuse_entry_param){.attr = (struct stat){.st_ino = ino, .st_mode = mode},
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
}

static struct fuse_lowlevel_ops errc_ll_oper = {
    .init = errc_init,
    //    .destroy = errc_destroy,
    .lookup = errc_lookup,
    .forget = errc_forget,
    //    .getattr = errc_getattr,
    .mknod = errc_mknod,
    //    .unlink = errc_unlink,
    .open = errc_open,
    //    .read = errc_read,
    .write = errc_write,
    .flush = errc_flush,
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

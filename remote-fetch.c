#define _GNU_SOURCE     /* Needed to get O_LARGEFILE definition */
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FAN_ERRNO_BITS
#define FAN_ERRNO_BITS 8
#define FAN_ERRNO_SHIFT (32 - FAN_ERRNO_BITS)
#define FAN_ERRNO_MASK ((1 << FAN_ERRNO_BITS) - 1)
#define FAN_DENY_ERRNO(err) \
	(FAN_DENY | ((((__u32)(err)) & FAN_ERRNO_MASK) << FAN_ERRNO_SHIFT))
#endif

#ifndef FAN_PRE_ACCESS
#define FAN_PRE_ACCESS 0x00080000
#endif

#ifndef FAN_PRE_MODIFY
#define FAN_PRE_MODIFY 0x00100000
#endif

#ifndef FAN_EVENT_INFO_TYPE_RANGE
#define FAN_EVENT_INFO_TYPE_RANGE	6
struct fanotify_event_info_range {
	struct fanotify_event_info_header hdr;
	__u32 count;
	__u64 offset;
};
#endif

#define FAN_EVENTS (FAN_PRE_ACCESS | FAN_PRE_MODIFY)

#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))

static const char *srcpath;
static const char *dstpath;
static int pagesize;
static bool use_sendfile = false;

#define __free(func) __attribute__((cleanup(func)))

static void close_dir(DIR **dir)
{
	if (*dir == NULL)
		return;
	closedir(*dir);
}

static void close_fd(int *fd)
{
	if (*fd < 0)
		return;
	close(*fd);
}

static void freep(void *ptr)
{
	void *real_ptr = *(void **)ptr;
	if (real_ptr == NULL)
		return;
	free(real_ptr);
}

static int strip_dstpath(char *path)
{
	size_t remaining;

	if (strlen(path) <= strlen(dstpath)) {
		fprintf(stderr, "'%s' not in the path '%s'", path, dstpath);
		return -1;
	}

	if (strncmp(path, dstpath, strlen(dstpath))) {
		fprintf(stderr, "path '%s' doesn't start with the source path '%s'\n",
			path, dstpath);
		return -1;
	}

	remaining = strlen(path) - strlen(dstpath);
	memmove(path, path + strlen(dstpath), remaining);

	/* strip any leading / in order to make it easier to concat. */
	while (*path == '/') {
		if (remaining == 0) {
			fprintf(stderr, "you gave us a weird ass string\n");
			return -1;
		}
		remaining--;
		memmove(path, path + 1, remaining);
	}
	path[remaining] = '\0';
	return 0;
}

/*
 * Dup a path, make sure there's a trailing '/' to make path concat easier.
 */
static char *pathdup(const char *orig)
{
	char *ret;
	size_t len = strlen(orig);

	/* Easy path, we have a trailing '/'. */
	if (orig[len - 1] == '/')
		return strdup(orig);

	ret = malloc((len + 2) * sizeof(char));
	if (!ret)
		return ret;

	memcpy(ret, orig, len);
	ret[len] = '/';
	len++;
	ret[len] = '\0';
	return ret;
}

static char *get_relpath(int fd)
{
	char procfd_path[PATH_MAX];
	char abspath[PATH_MAX];
	ssize_t path_len;
	int ret;

	/* readlink doesn't NULL terminate. */
	memset(abspath, 0, sizeof(abspath));

	snprintf(procfd_path, sizeof(procfd_path), "/proc/self/fd/%d", fd);
	path_len = readlink(procfd_path, abspath, sizeof(abspath) - 1);
	if (path_len < 0) {
		perror("readlink");
		return NULL;
	}

	printf("abspath is %s\n", abspath);
	ret = strip_dstpath(abspath);
	if (ret < 0)
		return NULL;

	return strdup(abspath);
}

static int copy_range(int src_fd, int fd, off_t offset, size_t count)
{
	off_t src_offset = offset;
	size_t written;
	ssize_t copied;

	if (use_sendfile)
		goto slow;

	while ((copied = copy_file_range(src_fd, &src_offset, fd, &offset,
					 count, 0)) >= 0) {
		if (copied == 0)
			return 0;

		printf("copied %ld count %d\n", copied, count);
		count -= copied;
		if (count == 0)
			return 0;
	}

	if (errno != EXDEV) {
		perror("copy_file_range");
		return -1;
	}
	use_sendfile = true;

slow:
	/* I love linux interfaces. */
	if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
		perror("seeking");
		return -1;
	}

	while ((copied = sendfile(fd, src_fd, &src_offset, count)) >= 0) {
		if (copied == 0)
			return 0;

		printf("sendfile %ld count%d\n", copied, count);
		count -= copied;
		if (count == 0)
			return 0;
	}

	perror("sendfile");
	return -1;
}

static int handle_event(int fanotify_fd, int fd, off_t offset, size_t count)
{
	char path[PATH_MAX];
	char *relpath __free(freep) = NULL;
	int src_fd __free(close_fd) = -1;
	size_t written = 0;
	ssize_t copied;
	off_t end = offset + count;
	blkcnt_t src_blocks;
	struct stat st;

	relpath = get_relpath(fd);
	if (!relpath)
		return -1;

	offset = round_down(offset, pagesize);
	end = round_up(end, pagesize);
	count = end - offset;

	printf("opening src fd\n");
	snprintf(path, sizeof(path), "%s%s", srcpath, relpath);
	src_fd = open(path, O_RDONLY);
	if (src_fd < 0) {
		fprintf(stderr, "srcpath %s relpath %s\n", srcpath, relpath);
		fprintf(stderr, "error opening file %s: %s (%d)\n", path, strerror(errno), errno);
		return -1;
	}

	if (fstat(src_fd, &st)) {
		perror("src fd is fucked");
		return -1;
	}

	src_blocks = st.st_blocks;

	if (fstat(fd, &st)) {
		perror("fd is fucked");
		return -1;
	}

	/*
	 * If we are the same size or larger (which can happen if we copy zero's
	 * instead of inserting a hole) then just assume we're full.  This is
	 * approximation can fall over, but its good enough for a PoC.
	 */
	if (st.st_blocks >= src_blocks) {
		int ret;

		snprintf(path, sizeof(path), "%s%s", dstpath, relpath);
		fanotify_mark(fanotify_fd, FAN_MARK_REMOVE,
					FAN_EVENTS, -1, path);
		if (ret < 0) {
			/* We already removed the mark, carry on. */
			if (errno == ENOENT) {
				errno = 0;
				return 0;
			}
			perror("removing fanotify mark");
			return -1;
		}
		return 0;
	}


	printf("got an event, path %s, offset %lu count %lu\n", path, offset, count);
	return copy_range(src_fd, fd, offset, count);
}

static int handle_events(int fd)
{
	const struct fanotify_event_metadata *metadata;
	struct fanotify_event_metadata buf[200];
	ssize_t len;
	char abspath[PATH_MAX];
	char procfd_path[PATH_MAX];
	struct fanotify_response response;
	int ret;

	len = read(fd, (void *)buf, sizeof(buf));
	if (len <= 0 && errno != EINTR) {
		perror("reading fanotify events");
		return -1;
	}

	printf("got something??\n");
	metadata = buf;
	while(FAN_EVENT_OK(metadata, len)) {
		off_t offset = 0;
		size_t count = 0;
		char *relpath;

		if (metadata->vers != FANOTIFY_METADATA_VERSION) {
			fprintf(stderr, "invalid metadata version, have %d, expect %d\n",
				metadata->vers, FANOTIFY_METADATA_VERSION);
			return -1;
		}
		if (metadata->fd < 0) {
			fprintf(stderr, "metadata fd is an error\n");
			return -1;
		}
		if (!(metadata->mask & FAN_EVENTS)) {
			fprintf(stderr, "metadata mask incorrect %lu\n",
				metadata->mask);
			return -1;
		}

		/*
		 * We have a specific range, load that instead of filling the
		 * entire file in.
		 */
		if (metadata->event_len > FAN_EVENT_METADATA_LEN) {
			const struct fanotify_event_info_range *range;
			printf("maybe have a info\n");
			range = (const struct fanotify_event_info_range *)(metadata + 1);
			if (range->hdr.info_type == FAN_EVENT_INFO_TYPE_RANGE) {
				count = range->count;
				offset = range->offset;
				printf("definitely have an info\n");
				if (count == 0) {
					ret = 0;
					goto next;
				}
			} else {
				printf("nope don't\n");
			}
		}

		/* We don't have a range, pre-fill the whole file. */
		if (count == 0) {
			struct stat st;

			printf("have to stat to get the thing\n");
			if (fstat(metadata->fd, &st)) {
				perror("stat() on opened file");
				return -1;
			}

			count = st.st_size;
		}

		ret = handle_event(fd, metadata->fd, offset, count);
next:
		response.fd = metadata->fd;
		if (ret)
			response.response = FAN_DENY_ERRNO(errno);
		else
			response.response = FAN_ALLOW;
		write(fd, &response, sizeof(response));
		close(metadata->fd);
		metadata = FAN_EVENT_NEXT(metadata, len);
	}

	return ret;
}

static int add_marks(const char *src, int fanotify_fd)
{
	char *path __free(freep) = NULL;
	DIR *dir __free(close_dir) = NULL;
	size_t pathlen = strlen(src) + 256;
	struct dirent *dirent;

	path = malloc(pathlen * sizeof(char));
	if (!path) {
		perror("allocating path buf");
		return -1;
	}

	dir = opendir(src);
	if (!dir) {
		fprintf(stderr, "Couldn't open directory %s: %s (%d)\n",
			src, strerror(errno), errno);
		return -1;
	}

	errno = 0;
	while ((dirent = readdir(dir)) != NULL) {
		int ret;

		if (!strcmp(dirent->d_name, ".") ||
		    !strcmp(dirent->d_name, ".."))
			continue;

		if (dirent->d_type == DT_DIR) {
			snprintf(path, pathlen, "%s%s/", src, dirent->d_name);
			ret = add_marks(path, fanotify_fd);
			if (ret)
				return ret;
		} else if (dirent->d_type == DT_REG) {
			ret = fanotify_mark(fanotify_fd, FAN_MARK_ADD,
					    FAN_EVENTS, dirfd(dir),
					    dirent->d_name);
			if (ret < 0) {
				perror("fanotify_mark");
				return -1;
			}
		}
		errno = 0;
	}
	return 0;
}

static void usage(void)
{
	fprintf(stderr, "Usage: remote-fetch <src directory> <dest directory>\n");
}

int main(int argc, char **argv)
{
	int fd __free(close_fd) = -1;
	int dirfd __free(close_fd) = -1;
	int ret;

	if (argc != 3) {
		usage();
		return 1;
	}

	pagesize = sysconf(_SC_PAGESIZE);
	if (pagesize < 0) {
		perror("sysconf");
		return 1;
	}

	srcpath = pathdup(argv[1]);
	dstpath = pathdup(argv[2]);
	if (!srcpath || !dstpath) {
		perror("allocate paths");
		return 1;
	}

	printf("src is %s dst is %s\n", srcpath, dstpath);

	dirfd = open(dstpath, O_DIRECTORY | O_RDONLY);
	if (dirfd < 0) {
		perror("open dstpath");
		return 1;
	}

	fd = fanotify_init(FAN_CLASS_PRE_CONTENT | FAN_UNLIMITED_MARKS, O_WRONLY | O_LARGEFILE);
	if (fd < 0) {
		perror("fanotify_init");
		return 1;
	}

	ret = add_marks(dstpath, fd);
	if (ret < 0)
		return 1;

	for (;;) {
		ret = handle_events(fd);
		if (ret)
			break;
	}

	return (ret < 0) ? 1 : 0;
}

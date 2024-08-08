#define _GNU_SOURCE     /* Needed to get O_LARGEFILE definition */
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define __free(func) __attribute__((cleanup(func)))

static void close_fd(int *fd)
{
	if (*fd < 0)
		return;
	close(*fd);
}

static void close_dir(DIR **dir)
{
	if (*dir == NULL)
		return;
	closedir(*dir);
}

static void freep(void *ptr)
{
	void *real_ptr = *(void **)ptr;
	if (real_ptr == NULL)
		return;
	free(real_ptr);
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

static int process_directory(DIR *srcdir, char *srcpath, char *dstpath)
{
	char *src __free(freep) = NULL;
	char *dst __free(freep) = NULL;
	size_t srclen = strlen(srcpath) + 256;
	size_t dstlen = strlen(dstpath) + 256;
	struct dirent *dirent;

	src = malloc(srclen * sizeof(char));
	dst = malloc(dstlen * sizeof(char));
	if (!src || !dst) {
		perror("allocating path buf");
		return -1;
	}

	errno = 0;
	while ((dirent = readdir(srcdir)) != NULL) {
		if (!strcmp(dirent->d_name, ".") ||
		    !strcmp(dirent->d_name, ".."))
			continue;

		if (dirent->d_type == DT_DIR) {
			DIR *nextdir __free(close_dir) = NULL;
			struct stat st;
			int ret;

			snprintf(src, srclen, "%s%s/", srcpath, dirent->d_name);
			snprintf(dst, dstlen, "%s%s/", dstpath, dirent->d_name);

			nextdir = opendir(src);
			if (!nextdir) {
				fprintf(stderr, "Couldn't open directory %s: %s (%d)\n",
					src, strerror(errno), errno);
				return -1;
			}

			if (stat(src, &st)) {
				fprintf(stderr, "Couldn't stat directory %s: %s (%d)\n",
					src, strerror(errno), errno);
				return -1;
			}

			if (mkdir(dst, st.st_mode)) {
				fprintf(stderr, "Couldn't mkdir %s: %s (%d)\n",
					dst, strerror(errno), errno);
				return -1;
			}

			ret = process_directory(nextdir, src, dst);
			if (ret)
				return ret;
		} else if (dirent->d_type == DT_REG) {
			int fd __free(close_fd) = -EBADF;
			struct stat st;

			snprintf(src, srclen, "%s%s", srcpath, dirent->d_name);
			snprintf(dst, dstlen, "%s%s", dstpath, dirent->d_name);

			if (stat(src, &st)) {
				fprintf(stderr, "Couldn't stat file %s: %s (%d)\n",
					src, strerror(errno), errno);
				return -1;
			}

			fd = open(dst, O_WRONLY|O_CREAT, st.st_mode);
			if (fd < 0) {
				fprintf(stderr, "Couldn't create file %s: %s (%d)\n",
					dst, strerror(errno), errno);
				return -1;
			}

			if (truncate(dst, st.st_size)) {
				fprintf(stderr, "Couldn't truncate file %s: %s (%d)\n",
					dst, strerror(errno), errno);
				return -1;
			}


			if (fsync(fd)) {
				fprintf(stderr, "Couldn't fsync file %s: %s (%d)\n",
					dst, strerror(errno), errno);
				return -1;
			}

			if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)) {
				fprintf(stderr, "Couldn't clear cache on file %s: %s (%d)\n",
					dst, strerror(errno), errno);
				return -1;
			}
		}
		errno = 0;
	}

	if (errno) {
		perror("readdir");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	DIR *srcdir __free(close_dir) = NULL;
	char *dstpath __free(freep) = NULL;
	char *srcpath __free(freep) = NULL;
	int ret;

	if (argc != 3) {
		fprintf(stderr, "Usage: populate <src directory> <dest directory>\n");
		return 1;
	}

	srcpath = pathdup(argv[1]);
	dstpath = pathdup(argv[2]);
	if (!dstpath || !srcpath) {
		perror("allocating paths");
		return 1;
	}

	srcdir = opendir(srcpath);
	if (!srcdir) {
		perror("open src directory");
		return 1;
	}

	ret = process_directory(srcdir, srcpath, dstpath);
	return ret ? 1 : 0;
}

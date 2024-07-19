#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* 1 MiB. */
#define FILE_SIZE (1 * 1024 * 1024)
#define PAGE_SIZE 4096

#define __free(func) __attribute__((cleanup(func)))

static void freep(void *ptr)
{
	void *real_ptr = *(void **)ptr;
	if (real_ptr == NULL)
		return;
	free(real_ptr);
}

static void close_fd(int *fd)
{
	if (*fd < 0)
		return;
	close(*fd);
}

static void unmap(void *ptr)
{
	void *real_ptr = *(void **)ptr;
	if (real_ptr == NULL)
		return;
	munmap(real_ptr, PAGE_SIZE);
}

static void print_buffer(const char *buf, off_t buf_off, off_t off, size_t len)
{
	for (int i = 0; i <= (len / 32); i++) {
		printf("%lu:", off + (i * 32));

		for (int c = 0; c < 32; c++) {
			if (!(c % 8))
				printf(" ");
			printf("%c", buf[buf_off++]);
		}
		printf("\n");
	}
}

static int validate_buffer(const char *type, const char *buf,
			   const char *pattern, off_t bufoff, off_t off,
			   size_t len)
{

	if (memcmp(buf + bufoff, pattern + off, len)) {
		printf("Buffers do not match at off %lu size %lu after %s\n",
		       off, len, type);
		printf("read buffer\n");
		print_buffer(buf, bufoff, off, len);
		printf("valid buffer\n");
		print_buffer(pattern, off, off, len);
		return 1;
	}

	return 0;
}

static int validate_range_fd(int fd, char *pattern, off_t off, size_t len)
{
	char *buf __free(freep) = NULL;
	ssize_t ret;
	size_t readin = 0;

	buf = malloc(len);
	if (!buf) {
		perror("malloc buf");
		return 1;
	}

	while ((ret = pread(fd, buf + readin, len - readin, off + readin)) > 0) {
		readin += ret;
		if (readin == len)
			break;
	}

	if (ret < 0) {
		perror("read");
		return 1;
	}

	return validate_buffer("read", buf, pattern, 0, off, len);
}

static int validate_file(const char *file, char *pattern)
{
	int fd __free(close_fd) = -EBADF;
	char *buf __free(unmap) = NULL;
	int ret;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		perror("open file");
		return 1;
	}

	/* Cycle through the file and do some random reads and validate them. */
	for (int i = 0; i < 5; i++) {
		off_t off = random() % FILE_SIZE;
		size_t len = random() % PAGE_SIZE;

		while ((off + len) > FILE_SIZE) {
			len = FILE_SIZE - off;
			if (len)
				break;
			len = random() % PAGE_SIZE;
		}

		ret = validate_range_fd(fd, pattern, off, len);
		if (ret)
			return ret;
	}

	buf = mmap(NULL, FILE_SIZE, PROT_READ|PROT_EXEC, MAP_PRIVATE, fd, 0);
	if (!buf) {
		perror("mmap");
		return 1;
	}

	/* Validate random ranges of the mmap buffer. */
	for (int i = 0; i < 5; i++) {
		off_t off = random() % FILE_SIZE;
		size_t len = random() % PAGE_SIZE;

		while ((off + len) > FILE_SIZE) {
			len = FILE_SIZE - off;
			if (len)
				break;
			len = random() % PAGE_SIZE;
		}

		ret = validate_buffer("mmap", buf, pattern, off, off, len);
		if (ret)
			return ret;
	}

	/* Now check the whole thing, one page at a time. */
	for (int i = 0; i < (FILE_SIZE / PAGE_SIZE); i++) {
		ret = validate_buffer("mmap", buf, pattern, i * PAGE_SIZE,
				      i * PAGE_SIZE, PAGE_SIZE);
		if (ret)
			return ret;
	}

	return 0;
}

static int create_file(const char *file, char *pattern)
{
	ssize_t ret;
	size_t written = 0;
	int fd __free(close_fd) = -EBADF;

	fd = open(file, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		perror("opening file");
		return 1;
	}

	while ((ret = write(fd, pattern + written, FILE_SIZE - written)) > 0) {
		written += ret;
		if (written == FILE_SIZE)
			break;
	}

	if (ret < 0) {
		perror("writing to the file");
		return 1;
	}

	return 0;
}

static void generate_pattern(char *pattern)
{
	for (int i = 0; i < (FILE_SIZE / PAGE_SIZE); i++) {
		char fill = 'a' + (i % 26);

		memset(pattern + (i * PAGE_SIZE), fill, PAGE_SIZE);
	}
}

static void usage(void)
{
	fprintf(stderr, "Usage: mmap-validate <create|validate> <file>\n");
}

int main(int argc, char **argv)
{
	char *pattern __free(freep) = NULL;

	if (argc != 3) {
		usage();
		return 1;
	}

	pattern = malloc(FILE_SIZE * sizeof(char));
	if (!pattern) {
		perror("malloc pattern");
		return 1;
	}

	generate_pattern(pattern);

	if (!strcmp(argv[1], "create"))
		return create_file(argv[2], pattern);

	if (strcmp(argv[1], "validate")) {
		usage();
		return 1;
	}

	return validate_file(argv[2], pattern);
}

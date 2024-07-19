all:
	gcc -o remote-fetch remote-fetch.c
	gcc -o populate populate.c
	gcc -o mmap-validate mmap-validate.c

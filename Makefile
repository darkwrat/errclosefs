errclosefs:
	gcc -Wall errclosefs.c `pkg-config fuse3 --cflags --libs` -o errclosefs

format:
	clang-format -i errclosefs.c

.PHONY: errclosefs

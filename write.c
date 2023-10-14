#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

int main(void)
{
	const char *s = "aaa";
	int fd = openat(AT_FDCWD, "/var/spool/exim/input/qwe", O_CREAT|O_RDWR|O_EXCL, 0640);
	if (fd < 0)
		perror("open");
	int wret = write(fd, s, 3);
	if (wret < 0)
		perror("write");
	int cret = close(fd);
	if (cret < 0)
		perror("close");
}

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

int main(void)
{
	const char *s = "aaa";
	int fd = open("/tmp/xxx/qwe", O_CREAT|O_TRUNC);
	if (fd < 0)
		perror("open");
	int wret = write(fd, s, 3);
	if (wret < 0)
		perror("write");
	int cret = close(fd);
	if (cret < 0)
		perror("cret");
}

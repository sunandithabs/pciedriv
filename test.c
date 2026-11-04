#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "edu_uapi.h"

int main(int argc, char **argv)
{
	int fd = open("/dev/edu", O_RDWR), rc = 0;
	__u32 v;

	if (fd < 0) { perror("open /dev/edu"); return 1; }
	if (argc > 1 && !strcmp(argv[1], "oops")) {
		printf("oops ioctl -> %d\n", ioctl(fd, EDU_IOC_OOPS));
		return 0;
	}
	v = 10;
	if (ioctl(fd, EDU_IOC_FACT, &v)) { perror("fact"); rc = 1; }
	else { printf("10! = %u (%s)\n", v, v == 3628800 ? "PASS" : "FAIL"); rc |= v != 3628800; }
	v = 5;
	if (ioctl(fd, EDU_IOC_FACT, &v)) { perror("fact"); rc = 1; }
	else { printf(" 5! = %u (%s)\n", v, v == 120 ? "PASS" : "FAIL"); rc |= v != 120; }
	if (ioctl(fd, EDU_IOC_DMA_TEST)) { perror("dma"); rc = 1; }
	else printf("DMA round trip: PASS\n");
	return rc;
}

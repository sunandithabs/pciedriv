#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "edu_uapi.h"

static int fails;
#define CHECK(name, cond) do { int ok_ = !!(cond); \
	printf("%-28s %s\n", name, ok_ ? "PASS" : "FAIL"); fails += !ok_; } while (0)

static __u32 fact(__u32 n) { __u32 r = 1; while (n > 1) r *= n--; return r; }

static int do_fact(int fd, __u32 n, __u32 *out)
{
	__u32 v = n;
	int rc = ioctl(fd, EDU_IOC_FACT, &v);

	*out = v;
	return rc;
}

int main(int argc, char **argv)
{
	int fd = open("/dev/edu", O_RDWR);
	struct edu_info in;
	unsigned char w[4096], r[4096];
	__u32 v, n;
	int i, bad = 0, st;

	if (fd < 0) { perror("open /dev/edu"); return 1; }
	if (argc > 1 && !strcmp(argv[1], "oops")) {
		printf("oops ioctl -> %d\n", ioctl(fd, EDU_IOC_OOPS));
		return 0;
	}

	CHECK("info ioctl", !ioctl(fd, EDU_IOC_INFO, &in) && in.buf_size == 4096);
	printf("  id=0x%08x irq=%u %s\n", in.id, in.irq, in.msi ? "MSI" : "INTx");

	for (n = 0; n <= 12; n++)
		bad += do_fact(fd, n, &v) || v != fact(n);
	CHECK("factorial 0..12", !bad);
	CHECK("factorial 20 (mod 2^32)", !do_fact(fd, 20, &v) && v == fact(20));

	CHECK("DMA round trip ioctl", !ioctl(fd, EDU_IOC_DMA_TEST));

	for (i = 0; i < 4096; i++) w[i] = (unsigned char)(i * 13 + 5);
	CHECK("write 4096 @0", pwrite(fd, w, 4096, 0) == 4096);
	memset(r, 0, sizeof(r));
	CHECK("read 4096 @0 matches", pread(fd, r, 4096, 0) == 4096 && !memcmp(w, r, 4096));
	memset(r, 0, sizeof(r));
	CHECK("read 100 @1000 matches", pread(fd, r, 100, 1000) == 100 && !memcmp(w + 1000, r, 100));
	CHECK("write clamps at end", pwrite(fd, w, 200, 4000) == 96);
	CHECK("read past end = EOF", pread(fd, r, 10, 4096) == 0);
	errno = 0;
	CHECK("write past end = ENOSPC", pwrite(fd, w, 10, 4096) < 0 && errno == ENOSPC);

	/* 4 processes hammering the factorial ioctl: exercises the mutex */
	for (i = 0; i < 4; i++)
		if (!fork()) {
			int j, f = 0, c = open("/dev/edu", O_RDWR);

			for (j = 0; j < 200; j++)
				f |= do_fact(c, 1 + j % 12, &v) || v != fact(1 + j % 12);
			_exit(f);
		}
	for (i = 0, bad = 0; i < 4; i++) { wait(&st); bad |= st; }
	CHECK("4x200 concurrent fact", !bad);

	printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
	return fails != 0;
}

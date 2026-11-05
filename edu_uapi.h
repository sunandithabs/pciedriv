#ifndef EDU_UAPI_H
#define EDU_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>

struct edu_info {
	__u32 id;        /* EDU_ID register (0xRRrr00ed) */
	__u32 irq;       /* Linux IRQ number */
	__u32 msi;       /* 1 = MSI, 0 = legacy INTx */
	__u32 buf_size;  /* device DMA window usable via read()/write() */
};

#define EDU_IOC_FACT      _IOWR('E', 1, __u32)  /* in: n, out: n! (mod 2^32) */
#define EDU_IOC_DMA_TEST  _IO('E', 2)           /* DMA round trip, 0 = ok */
#define EDU_IOC_OOPS      _IO('E', 3)           /* NULL deref, needs allow_oops=1 */
#define EDU_IOC_INFO      _IOR('E', 4, struct edu_info)
#endif

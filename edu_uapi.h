#ifndef EDU_UAPI_H
#define EDU_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define EDU_IOC_FACT      _IOWR('E', 1, __u32)  /* in: n, out: n! (mod 2^32) */
#define EDU_IOC_DMA_TEST  _IO('E', 2)           /* DMA round trip, 0 = ok */
#define EDU_IOC_OOPS      _IO('E', 3)           /* NULL deref, needs allow_oops=1 */
#endif

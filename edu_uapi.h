#ifndef EDU_UAPI_H
#define EDU_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>
struct edu_info {
	__u32 id;
	__u32 irq;
	__u32 msi;
	__u32 buf_size;
};
#define EDU_IOC_FACT      _IOWR('E', 1, __u32)
#define EDU_IOC_DMA_TEST  _IO('E', 2)
#define EDU_IOC_OOPS      _IO('E', 3)
#define EDU_IOC_INFO      _IOR('E', 4, struct edu_info)
#endif

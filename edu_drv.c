// SPDX-License-Identifier: GPL-2.0
/* PCIe driver for QEMU's "edu" device: MMIO, MSI, DMA, misc chardev. */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include "edu_uapi.h"

#define EDU_ID         0x00
#define EDU_FACT       0x08
#define EDU_STATUS     0x20
#define EDU_IRQ_STAT   0x24
#define EDU_IRQ_ACK    0x64
#define EDU_DMA_SRC    0x80
#define EDU_DMA_DST    0x88
#define EDU_DMA_CNT    0x90
#define EDU_DMA_CMD    0x98
#define EDU_ST_IRQEN   0x80
#define EDU_DMA_START  BIT(0)
#define EDU_DMA_FROM   BIT(1)   /* device -> RAM */
#define EDU_DMA_IRQ    BIT(2)
#define EDU_DEV_BUF    0x40000  /* device-internal DMA window */
#define EDU_BUF_SZ     4096

static bool allow_oops;
module_param(allow_oops, bool, 0444);
MODULE_PARM_DESC(allow_oops, "enable EDU_IOC_OOPS (crash-analysis practice)");

struct edu {
	void __iomem *regs;
	struct miscdevice misc;
	struct completion done;
	struct mutex lock;
	u8 *buf;
	dma_addr_t buf_dma;
};

static irqreturn_t edu_isr(int irq, void *data)
{
	struct edu *e = data;
	u32 st = readl(e->regs + EDU_IRQ_STAT);

	if (!st)
		return IRQ_NONE;
	writel(st, e->regs + EDU_IRQ_ACK);
	complete(&e->done);
	return IRQ_HANDLED;
}

static int edu_wait(struct edu *e)
{
	return wait_for_completion_timeout(&e->done, HZ) ? 0 : -ETIMEDOUT;
}

static int edu_dma(struct edu *e, u64 src, u64 dst, u32 cmd)
{
	reinit_completion(&e->done);
	writeq(src, e->regs + EDU_DMA_SRC);
	writeq(dst, e->regs + EDU_DMA_DST);
	writeq(EDU_BUF_SZ, e->regs + EDU_DMA_CNT);
	writeq(cmd | EDU_DMA_START | EDU_DMA_IRQ, e->regs + EDU_DMA_CMD);
	return edu_wait(e);
}

static int edu_dma_test(struct edu *e)
{
	int i, ret;

	for (i = 0; i < EDU_BUF_SZ; i++)
		e->buf[i] = (u8)(i * 7 + 1);
	ret = edu_dma(e, e->buf_dma, EDU_DEV_BUF, 0);
	if (ret)
		return ret;
	memset(e->buf, 0, EDU_BUF_SZ);
	ret = edu_dma(e, EDU_DEV_BUF, e->buf_dma, EDU_DMA_FROM);
	if (ret)
		return ret;
	for (i = 0; i < EDU_BUF_SZ; i++)
		if (e->buf[i] != (u8)(i * 7 + 1))
			return -EIO;
	return 0;
}

static long edu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct edu *e = container_of(f->private_data, struct edu, misc);
	long ret;
	u32 v;

	switch (cmd) {
	case EDU_IOC_OOPS:
		if (!allow_oops)
			return -EPERM;
		*(volatile int *)NULL = 1;
		return 0;
	case EDU_IOC_FACT:
		if (get_user(v, (u32 __user *)arg))
			return -EFAULT;
		mutex_lock(&e->lock);
		reinit_completion(&e->done);
		writel(v, e->regs + EDU_FACT);
		ret = edu_wait(e);
		if (!ret)
			ret = put_user(readl(e->regs + EDU_FACT),
				       (u32 __user *)arg);
		mutex_unlock(&e->lock);
		return ret;
	case EDU_IOC_DMA_TEST:
		mutex_lock(&e->lock);
		ret = edu_dma_test(e);
		mutex_unlock(&e->lock);
		return ret;
	}
	return -ENOTTY;
}

static const struct file_operations edu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = edu_ioctl,
};

static void edu_free_vec(void *p)
{
	pci_free_irq_vectors(p);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu *e;
	int ret;

	e = devm_kzalloc(&pdev->dev, sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	ret = pcim_iomap_regions(pdev, BIT(0), "edu");
	if (ret)
		return ret;
	e->regs = pcim_iomap_table(pdev)[0];
	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
	if (ret)
		return ret;
	e->buf = dmam_alloc_coherent(&pdev->dev, EDU_BUF_SZ, &e->buf_dma,
				     GFP_KERNEL);
	if (!e->buf)
		return -ENOMEM;

	init_completion(&e->done);
	mutex_init(&e->lock);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (ret < 0)
		return ret;
	ret = devm_add_action_or_reset(&pdev->dev, edu_free_vec, pdev);
	if (ret)
		return ret;
	ret = devm_request_irq(&pdev->dev, pci_irq_vector(pdev, 0), edu_isr,
			       0, "edu", e);
	if (ret)
		return ret;

	writel(EDU_ST_IRQEN, e->regs + EDU_STATUS);
	e->misc.minor = MISC_DYNAMIC_MINOR;
	e->misc.name = "edu";
	e->misc.fops = &edu_fops;
	ret = misc_register(&e->misc);
	if (ret)
		return ret;
	pci_set_drvdata(pdev, e);
	dev_info(&pdev->dev, "edu id=0x%08x irq=%d msi=%d\n",
		 readl(e->regs + EDU_ID), pci_irq_vector(pdev, 0),
		 pdev->msi_enabled);
	return 0;
}

static void edu_remove(struct pci_dev *pdev)
{
	misc_deregister(&((struct edu *)pci_get_drvdata(pdev))->misc);
}

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(0x1234, 0x11e8) }, { }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
	.name = "edu", .id_table = edu_ids,
	.probe = edu_probe, .remove = edu_remove,
};
module_pci_driver(edu_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QEMU edu PCIe device driver");

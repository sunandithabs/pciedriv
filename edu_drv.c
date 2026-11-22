#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/atomic.h>
#include <linux/iopoll.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include "edu_uapi.h"
#define EDU_ID         0x00
#define EDU_LIVE       0x04
#define EDU_FACT       0x08
#define EDU_STATUS     0x20
#define EDU_IRQ_STAT   0x24
#define EDU_IRQ_ACK    0x64
#define EDU_DMA_SRC    0x80
#define EDU_DMA_DST    0x88
#define EDU_DMA_CNT    0x90
#define EDU_DMA_CMD    0x98
#define EDU_ST_BUSY    BIT(0)
#define EDU_ST_IRQEN   0x80
#define EDU_IRQ_FACT   0x01
#define EDU_IRQ_DMA    0x100
#define EDU_DMA_START  BIT(0)
#define EDU_DMA_FROM   BIT(1)
#define EDU_DMA_IRQ    BIT(2)
#define EDU_DEV_BUF    0x40000
#define EDU_BUF_SZ     4096
enum { EDU_FACT_IDLE, EDU_FACT_RUNNING, EDU_FACT_DONE };
static bool allow_oops;
module_param(allow_oops, bool, 0444);
MODULE_PARM_DESC(allow_oops, "enable EDU_IOC_OOPS (crash-analysis practice)");
static unsigned int timeout_ms = 1000;
module_param(timeout_ms, uint, 0644);
MODULE_PARM_DESC(timeout_ms, "device operation timeout in ms (default 1000, 0 = fail immediately, for tests)");
struct edu {
	void __iomem *regs;
	struct miscdevice misc;
	struct completion fact_done, dma_done;
	wait_queue_head_t wq;
	spinlock_t slock;
	int fact_state;
	struct mutex lock;
	u8 *buf;
	dma_addr_t buf_dma;
	struct dentry *dbg;
	int irq;
	u32 msi;
	atomic64_t irqs, facts, dma_bytes, timeouts;
};
static irqreturn_t edu_isr(int irq, void *data)
{
	struct edu *e = data;
	u32 st = readl(e->regs + EDU_IRQ_STAT);
	unsigned long fl;
	if (!st)
		return IRQ_NONE;
	writel(st, e->regs + EDU_IRQ_ACK);
	atomic64_inc(&e->irqs);
	if (st & EDU_IRQ_FACT) {
		spin_lock_irqsave(&e->slock, fl);
		if (e->fact_state == EDU_FACT_RUNNING) {
			e->fact_state = EDU_FACT_DONE;
			spin_unlock_irqrestore(&e->slock, fl);
			wake_up_interruptible(&e->wq);
		} else {
			spin_unlock_irqrestore(&e->slock, fl);
			complete(&e->fact_done);
		}
	}
	if (st & EDU_IRQ_DMA)
		complete(&e->dma_done);
	return IRQ_HANDLED;
}
static int edu_wait(struct edu *e, struct completion *c)
{
	if (timeout_ms ? wait_for_completion_timeout(c, msecs_to_jiffies(timeout_ms))
		       : try_wait_for_completion(c))
		return 0;
	atomic64_inc(&e->timeouts);
	return -ETIMEDOUT;
}
static int edu_fact_quiesce(struct edu *e)
{
	u32 v;
	if (readl_poll_timeout(e->regs + EDU_STATUS, v, !(v & EDU_ST_BUSY),
			       10, 100000))
		return -EBUSY;
	synchronize_irq(e->irq);
	return 0;
}
static int edu_dma_quiesce(struct edu *e)
{
	u64 v;
	if (readq_poll_timeout(e->regs + EDU_DMA_CMD, v, !(v & EDU_DMA_START),
			       10, 100000))
		return -EBUSY;
	synchronize_irq(e->irq);
	return 0;
}
static int edu_dma(struct edu *e, u64 src, u64 dst, u32 len, u32 cmd)
{
	int ret;
	ret = edu_dma_quiesce(e);
	if (ret)
		return ret;
	reinit_completion(&e->dma_done);
	writeq(src, e->regs + EDU_DMA_SRC);
	writeq(dst, e->regs + EDU_DMA_DST);
	writeq(len, e->regs + EDU_DMA_CNT);
	writeq(cmd | EDU_DMA_START | EDU_DMA_IRQ, e->regs + EDU_DMA_CMD);
	ret = edu_wait(e, &e->dma_done);
	if (!ret)
		atomic64_add(len, &e->dma_bytes);
	return ret;
}
static int edu_dma_test(struct edu *e)
{
	int i, ret;
	for (i = 0; i < EDU_BUF_SZ; i++)
		e->buf[i] = (u8)(i * 7 + 1);
	ret = edu_dma(e, e->buf_dma, EDU_DEV_BUF, EDU_BUF_SZ, 0);
	if (ret)
		return ret;
	memset(e->buf, 0, EDU_BUF_SZ);
	ret = edu_dma(e, EDU_DEV_BUF, e->buf_dma, EDU_BUF_SZ, EDU_DMA_FROM);
	if (ret)
		return ret;
	for (i = 0; i < EDU_BUF_SZ; i++)
		if (e->buf[i] != (u8)(i * 7 + 1))
			return -EIO;
	return 0;
}
static long edu_fact_sync(struct edu *e, u32 __user *p)
{
	long ret;
	u32 v;
	if (get_user(v, p))
		return -EFAULT;
	mutex_lock(&e->lock);
	if (e->fact_state != EDU_FACT_IDLE) {
		ret = -EBUSY;
		goto out;
	}
	ret = edu_fact_quiesce(e);
	if (ret)
		goto out;
	reinit_completion(&e->fact_done);
	writel(v, e->regs + EDU_FACT);
	ret = edu_wait(e, &e->fact_done);
	if (!ret) {
		atomic64_inc(&e->facts);
		ret = put_user(readl(e->regs + EDU_FACT), p);
	}
out:
	mutex_unlock(&e->lock);
	return ret;
}
static long edu_fact_start(struct edu *e, u32 __user *p)
{
	unsigned long fl;
	long ret;
	u32 v;
	if (get_user(v, p))
		return -EFAULT;
	mutex_lock(&e->lock);
	if (e->fact_state != EDU_FACT_IDLE) {
		ret = -EBUSY;
		goto out;
	}
	ret = edu_fact_quiesce(e);
	if (ret)
		goto out;
	spin_lock_irqsave(&e->slock, fl);
	e->fact_state = EDU_FACT_RUNNING;
	spin_unlock_irqrestore(&e->slock, fl);
	writel(v, e->regs + EDU_FACT);
out:
	mutex_unlock(&e->lock);
	return ret;
}
static long edu_fact_result(struct edu *e, u32 __user *p)
{
	unsigned long fl;
	long ret = 0;
	int st;
	mutex_lock(&e->lock);
	spin_lock_irqsave(&e->slock, fl);
	if (e->fact_state == EDU_FACT_RUNNING &&
	    !(readl(e->regs + EDU_STATUS) & EDU_ST_BUSY))
		e->fact_state = EDU_FACT_DONE;
	st = e->fact_state;
	if (st == EDU_FACT_DONE)
		e->fact_state = EDU_FACT_IDLE;
	spin_unlock_irqrestore(&e->slock, fl);
	if (st == EDU_FACT_IDLE) {
		ret = -EINVAL;
	} else if (st == EDU_FACT_RUNNING) {
		ret = -EAGAIN;
	} else {
		atomic64_inc(&e->facts);
		ret = put_user(readl(e->regs + EDU_FACT), p);
	}
	mutex_unlock(&e->lock);
	return ret;
}
static long edu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct edu *e = container_of(f->private_data, struct edu, misc);
	long ret;
	switch (cmd) {
	case EDU_IOC_OOPS:
		if (!allow_oops)
			return -EPERM;
		*(volatile int *)NULL = 1;
		return 0;
	case EDU_IOC_FACT:
		return edu_fact_sync(e, (u32 __user *)arg);
	case EDU_IOC_FACT_START:
		return edu_fact_start(e, (u32 __user *)arg);
	case EDU_IOC_FACT_RESULT:
		return edu_fact_result(e, (u32 __user *)arg);
	case EDU_IOC_INFO: {
		struct edu_info info = {
			.id = readl(e->regs + EDU_ID),
			.irq = e->irq,
			.msi = e->msi,
			.buf_size = EDU_BUF_SZ,
		};
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ?
		       -EFAULT : 0;
	}
	case EDU_IOC_DMA_TEST:
		mutex_lock(&e->lock);
		ret = edu_dma_test(e);
		mutex_unlock(&e->lock);
		return ret;
	}
	return -ENOTTY;
}
static ssize_t edu_write(struct file *f, const char __user *ubuf, size_t n,
			 loff_t *pos)
{
	struct edu *e = container_of(f->private_data, struct edu, misc);
	int ret;
	if (*pos >= EDU_BUF_SZ)
		return -ENOSPC;
	n = min_t(size_t, n, EDU_BUF_SZ - *pos);
	if (!n)
		return 0;
	mutex_lock(&e->lock);
	ret = copy_from_user(e->buf + *pos, ubuf, n) ? -EFAULT :
	      edu_dma(e, e->buf_dma + *pos, EDU_DEV_BUF + *pos, n, 0);
	mutex_unlock(&e->lock);
	if (ret)
		return ret;
	*pos += n;
	return n;
}
static ssize_t edu_read(struct file *f, char __user *ubuf, size_t n,
			loff_t *pos)
{
	struct edu *e = container_of(f->private_data, struct edu, misc);
	int ret;
	if (*pos >= EDU_BUF_SZ)
		return 0;
	n = min_t(size_t, n, EDU_BUF_SZ - *pos);
	if (!n)
		return 0;
	mutex_lock(&e->lock);
	ret = edu_dma(e, EDU_DEV_BUF + *pos, e->buf_dma + *pos, n,
		      EDU_DMA_FROM);
	if (!ret && copy_to_user(ubuf, e->buf + *pos, n))
		ret = -EFAULT;
	mutex_unlock(&e->lock);
	if (ret)
		return ret;
	*pos += n;
	return n;
}
static __poll_t edu_poll(struct file *f, poll_table *wait)
{
	struct edu *e = container_of(f->private_data, struct edu, misc);
	poll_wait(f, &e->wq, wait);
	return READ_ONCE(e->fact_state) == EDU_FACT_DONE ?
	       EPOLLIN | EPOLLRDNORM : 0;
}
static int edu_stats_show(struct seq_file *m, void *unused)
{
	struct edu *e = m->private;
	seq_printf(m, "irqs      %lld\nfact_ops  %lld\ndma_bytes %lld\ntimeouts  %lld\n",
		   atomic64_read(&e->irqs), atomic64_read(&e->facts),
		   atomic64_read(&e->dma_bytes), atomic64_read(&e->timeouts));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(edu_stats);
static const struct file_operations edu_fops = {
	.owner = THIS_MODULE,
	.read = edu_read,
	.write = edu_write,
	.llseek = default_llseek,
	.unlocked_ioctl = edu_ioctl,
	.poll = edu_poll,
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
	writel(0x5a5a5a5a, e->regs + EDU_LIVE);
	if (readl(e->regs + EDU_LIVE) != ~0x5a5a5a5aU) {
		dev_err(&pdev->dev, "liveness check failed\n");
		return -ENODEV;
	}
	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
	if (ret)
		return ret;
	e->buf = dmam_alloc_coherent(&pdev->dev, EDU_BUF_SZ, &e->buf_dma,
				     GFP_KERNEL);
	if (!e->buf)
		return -ENOMEM;
	init_completion(&e->fact_done);
	init_completion(&e->dma_done);
	init_waitqueue_head(&e->wq);
	spin_lock_init(&e->slock);
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
	e->irq = pci_irq_vector(pdev, 0);
	e->msi = pdev->msi_enabled;
	writel(EDU_ST_IRQEN, e->regs + EDU_STATUS);
	e->misc.minor = MISC_DYNAMIC_MINOR;
	e->misc.name = "edu";
	e->misc.fops = &edu_fops;
	ret = misc_register(&e->misc);
	if (ret)
		return ret;
	pci_set_drvdata(pdev, e);
	e->dbg = debugfs_create_dir("edu", NULL);
	debugfs_create_file("stats", 0444, e->dbg, e, &edu_stats_fops);
	dev_info(&pdev->dev, "edu id=0x%08x irq=%d msi=%d\n",
		 readl(e->regs + EDU_ID), pci_irq_vector(pdev, 0),
		 pdev->msi_enabled);
	return 0;
}
static void edu_remove(struct pci_dev *pdev)
{
	struct edu *e = pci_get_drvdata(pdev);
	misc_deregister(&e->misc);
	debugfs_remove_recursive(e->dbg);
	writel(0, e->regs + EDU_STATUS);
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
MODULE_VERSION("2.1");

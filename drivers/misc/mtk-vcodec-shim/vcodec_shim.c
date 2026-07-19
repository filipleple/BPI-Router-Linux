// SPDX-License-Identifier: GPL-2.0
/*
 * MT7623 (BPI-R2) /dev/Vcodec shim
 *
 * A minimal modern (6.x) re-implementation of the MediaTek android-mtk-3.18
 * "videocodec" char device, providing exactly the /dev/Vcodec ABI that the
 * prebuilt armhf userspace (libvcodec_utility.so -> libvcodecdrv.so) needs to
 * drive the MT7623 VPU by direct register programming:
 *
 *   - mmap() of the VDEC register banks (0x16000000 + 0x16020000..0x16028fff),
 *   - mmap() of driver-allocated DMA-coherent working buffers,
 *   - the _IOW('M', nr) ioctl family (HW lock, wait-ISR, buffer alloc, ...),
 *   - VDEC power via an SMI-larb device link (same pattern as the lima fix).
 *
 * Decode completion is detected by POLLING the decode-done register, because
 * the MT7623/MT2701 VDEC GIC SPI is not published anywhere (see the project
 * briefing). This keeps first light independent of the unknown interrupt.
 *
 * The M4U (IOMMU) userspace ABI the same blob needs lives in m4u_shim.c and is
 * registered by this module's probe (one struct device, one dma domain).
 *
 * Scope: decode-only PoC. Encode / secure / EMI-bandwidth / DVFS ioctls are
 * accepted as no-op success so the userspace state machine proceeds.
 *
 * State lifetime: HW-lock ownership and DMA buffers are tied to the open file,
 * so a client that crashes (e.g. vcodec_probe's fork-isolated children) can no
 * longer wedge the HW lock or leak buffers until reboot -- .release cleans up.
 */

#define pr_fmt(fmt) "vcodec-shim: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>
#include <linux/device.h>

#include "vcodec_shim_abi.h"
#include "vcodec_shim_priv.h"

/* VDEC register windows userspace is allowed to mmap (physical). */
#define VDEC_SYS_PA	0x16000000UL
#define VDEC_SYS_SZ	0x00001000UL
#define VDEC_BLK_PA	0x16020000UL
#define VDEC_BLK_SZ	0x00009000UL	/* 0x16020000..0x16028fff */

/* Decode-done status lives at VDEC_MISC + 0xA4 (== block base + 0xA4 == 41*4),
 * bit16 = done; clear by toggling 0x11/~0x10 like the android dec_isr(). */
#define VDEC_DONE_OFF	0xA4
#define VDEC_DONE_BIT	BIT(16)

/* Default HW-lock timeout when userspace passes u4TimeoutMs == 0 (ms). */
#define HW_LOCK_DEFAULT_MS	2000

struct vcodec_buf {
	struct list_head node;
	dma_addr_t dma;		/* == physical on MT7623 (no IOMMU attach) */
	void *cpu;
	size_t size;
};

/* Per-open state. mmap/alloc/free operate on the buffers of *this* fd; the HW
 * lock is owned by the fd (see hw_owner in struct vcodec_shim). */
struct vcodec_file {
	struct vcodec_shim *vc;
	struct mutex buf_lock;
	struct list_head bufs;
};

static struct vcodec_shim *g_vc;	/* single instance (probe/remove) */

/* ------------------------------------------------------------ open/release */

static int vcodec_open(struct inode *inode, struct file *file)
{
	struct vcodec_file *vf;

	if (!g_vc)
		return -ENODEV;

	vf = kzalloc(sizeof(*vf), GFP_KERNEL);
	if (!vf)
		return -ENOMEM;
	vf->vc = g_vc;
	mutex_init(&vf->buf_lock);
	INIT_LIST_HEAD(&vf->bufs);
	file->private_data = vf;
	return 0;
}

static void vcodec_drop_hw_lock(struct vcodec_shim *vc, struct file *file,
				bool warn)
{
	bool release = false;

	spin_lock(&vc->hw_owner_lock);
	if (vc->hw_owner == file) {
		vc->hw_owner = NULL;
		release = true;
	}
	spin_unlock(&vc->hw_owner_lock);

	if (release) {
		if (warn)
			dev_warn(vc->dev,
				 "releasing HW lock held by exiting client\n");
		up(&vc->hw_sem);
	}
}

static int vcodec_release(struct inode *inode, struct file *file)
{
	struct vcodec_file *vf = file->private_data;
	struct vcodec_shim *vc = vf->vc;
	struct vcodec_buf *b, *tmp;

	/* Free the HW lock if this client died holding it. */
	vcodec_drop_hw_lock(vc, file, true);

	/* Free any DMA buffers this fd allocated but did not free. */
	mutex_lock(&vf->buf_lock);
	list_for_each_entry_safe(b, tmp, &vf->bufs, node) {
		list_del(&b->node);
		dma_free_coherent(vc->dev, b->size, b->cpu, b->dma);
		kfree(b);
	}
	mutex_unlock(&vf->buf_lock);

	kfree(vf);
	return 0;
}

/* ------------------------------------------------------------------ mmap */

static int vcodec_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vcodec_file *vf = file->private_data;
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long pa = vma->vm_pgoff << PAGE_SHIFT;
	struct vcodec_buf *b;

	/* 1) register banks -> direct noncached physical mapping */
	if ((pa >= VDEC_SYS_PA && pa + len <= VDEC_SYS_PA + VDEC_SYS_SZ) ||
	    (pa >= VDEC_BLK_PA && pa + len <= VDEC_BLK_PA + VDEC_BLK_SZ)) {
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, len,
				    vma->vm_page_prot))
			return -EAGAIN;
		return 0;
	}

	/* 2) a DMA buffer this fd allocated -> dma_mmap_coherent */
	mutex_lock(&vf->buf_lock);
	list_for_each_entry(b, &vf->bufs, node) {
		if (pa == (unsigned long)b->dma && len <= PAGE_ALIGN(b->size)) {
			int ret;

			vma->vm_pgoff = 0;	/* map from buffer start */
			ret = dma_mmap_coherent(vf->vc->dev, vma, b->cpu, b->dma,
						b->size);
			mutex_unlock(&vf->buf_lock);
			return ret;
		}
	}
	mutex_unlock(&vf->buf_lock);

	dev_err(vf->vc->dev, "mmap rejected: pa=0x%lx len=0x%lx\n", pa, len);
	return -EINVAL;
}

/* ------------------------------------------------------------- ioctl bits */

static long vcodec_lockhw(struct vcodec_shim *vc, struct file *file,
			  unsigned long arg)
{
	VAL_HW_LOCK_T lock;
	unsigned long ms;
	int ret;

	if (copy_from_user(&lock, (void __user *)arg, sizeof(lock)))
		return -EFAULT;

	ms = lock.u4TimeoutMs ? lock.u4TimeoutMs : HW_LOCK_DEFAULT_MS;
	ret = down_timeout(&vc->hw_sem, msecs_to_jiffies(ms));
	if (ret) {
		dev_warn_ratelimited(vc->dev, "LOCKHW timeout %lums\n", ms);
		return ret;		/* -ETIME */
	}

	spin_lock(&vc->hw_owner_lock);
	vc->hw_owner = file;
	spin_unlock(&vc->hw_owner_lock);
	return 0;
}

static long vcodec_unlockhw(struct vcodec_shim *vc, struct file *file)
{
	bool release = false;

	spin_lock(&vc->hw_owner_lock);
	if (vc->hw_owner == file) {
		vc->hw_owner = NULL;
		release = true;
	}
	spin_unlock(&vc->hw_owner_lock);

	if (!release) {
		/* UNLOCKHW without (or not owning) the lock: don't touch the
		 * semaphore count -- a spurious up() would corrupt it. */
		dev_warn_ratelimited(vc->dev, "UNLOCKHW without ownership\n");
		return 0;
	}
	up(&vc->hw_sem);
	return 0;
}

static long vcodec_waitisr(struct vcodec_shim *vc, unsigned long arg)
{
	VAL_ISR_T isr;
	unsigned long ms, deadline;
	u32 done = 0;

	if (copy_from_user(&isr, (void __user *)arg, sizeof(isr)))
		return -EFAULT;

	ms = isr.u4TimeoutMs ? isr.u4TimeoutMs : 100;
	deadline = jiffies + msecs_to_jiffies(ms);

	do {
		done = readl_relaxed(vc->vdec_base + VDEC_DONE_OFF);
		if (done & VDEC_DONE_BIT) {
			/* clear interrupt, mirroring android dec_isr() */
			u32 v = readl_relaxed(vc->vdec_base + VDEC_DONE_OFF);

			writel_relaxed(v | 0x11, vc->vdec_base + VDEC_DONE_OFF);
			v = readl_relaxed(vc->vdec_base + VDEC_DONE_OFF);
			writel_relaxed(v & ~0x10, vc->vdec_base + VDEC_DONE_OFF);

			isr.u4IrqStatusNum = 1;
			isr.u4IrqStatus[0] = done;
			if (copy_to_user((void __user *)arg, &isr, sizeof(isr)))
				return -EFAULT;
			return 0;
		}
		usleep_range(50, 200);
	} while (time_before(jiffies, deadline));

	dev_warn_ratelimited(vc->dev, "WAITISR poll timeout %lums (st=0x%08x)\n",
			     ms, done);
	/* Return -2 exactly like the android videocodec driver's
	 * VAL_RESULT_INVALID_ISR path: the blob treats a negative return as
	 * "no ISR / retry". Returning 0 here would fake a decode-done and make
	 * the blob read garbage register state -- indistinguishable from bad
	 * programming. Keep the failure loud and distinguishable. */
	return -2;
}

static long vcodec_alloc(struct vcodec_file *vf, unsigned long arg)
{
	struct vcodec_shim *vc = vf->vc;
	VAL_MEMORY_T m;
	struct vcodec_buf *b;

	if (copy_from_user(&m, (void __user *)arg, sizeof(m)))
		return -EFAULT;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->size = (size_t)m.u4MemSize;
	b->cpu = dma_alloc_coherent(vc->dev, b->size, &b->dma, GFP_KERNEL);
	if (!b->cpu) {
		kfree(b);
		return -ENOMEM;
	}

	mutex_lock(&vf->buf_lock);
	list_add(&b->node, &vf->bufs);
	mutex_unlock(&vf->buf_lock);

	/* userspace mmaps using the physical address it gets back here */
	m.pvMemPa_ext64 = 0;
	m.pvMemVa_ext64 = 0;
	m.pvMemPa = (VAL_VOID_T *)(uintptr_t)b->dma;
	m.pvMemVa = (VAL_VOID_T *)(uintptr_t)b->dma;	/* opaque to userspace */
	if (copy_to_user((void __user *)arg, &m, sizeof(m)))
		return -EFAULT;

	dev_dbg(vc->dev, "ALLOC %zu -> pa 0x%llx\n", b->size, (u64)b->dma);
	return 0;
}

static long vcodec_free(struct vcodec_file *vf, unsigned long arg)
{
	struct vcodec_shim *vc = vf->vc;
	VAL_MEMORY_T m;
	struct vcodec_buf *b, *tmp;
	dma_addr_t pa;

	if (copy_from_user(&m, (void __user *)arg, sizeof(m)))
		return -EFAULT;
	pa = (dma_addr_t)(uintptr_t)m.pvMemPa;

	mutex_lock(&vf->buf_lock);
	list_for_each_entry_safe(b, tmp, &vf->bufs, node) {
		if (b->dma == pa) {
			list_del(&b->node);
			mutex_unlock(&vf->buf_lock);
			dma_free_coherent(vc->dev, b->size, b->cpu, b->dma);
			kfree(b);
			return 0;
		}
	}
	mutex_unlock(&vf->buf_lock);
	dev_warn(vc->dev, "FREE: unknown pa 0x%llx\n", (u64)pa);
	return 0;
}

static long vcodec_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vcodec_file *vf = file->private_data;
	struct vcodec_shim *vc = vf->vc;

	switch (cmd) {
	case VCODEC_LOCKHW:
		return vcodec_lockhw(vc, file, arg);
	case VCODEC_UNLOCKHW:
		return vcodec_unlockhw(vc, file);
	case VCODEC_WAITISR:
		return vcodec_waitisr(vc, arg);
	case VCODEC_ALLOC_NON_CACHE_BUFFER:
		return vcodec_alloc(vf, arg);
	case VCODEC_FREE_NON_CACHE_BUFFER:
		return vcodec_free(vf, arg);
	case VCODEC_GET_CORE_NUMBER: {
		int n = 1;

		if (copy_to_user((void __user *)arg, &n, sizeof(n)))
			return -EFAULT;
		return 0;
	}
	/* accepted as no-op success: lock-init, thread id, power/EMI/DVFS, MB */
	case VCODEC_INITHWLOCK:
	case VCODEC_DEINITHWLOCK:
	case VCODEC_SET_THREAD_ID:
	case VCODEC_INC_PWR_USER:
	case VCODEC_DEC_PWR_USER:
	case VCODEC_INC_ENC_EMI_USER:
	case VCODEC_DEC_ENC_EMI_USER:
	case VCODEC_INC_DEC_EMI_USER:
	case VCODEC_DEC_DEC_EMI_USER:
	case VCODEC_GET_CPU_LOADING_INFO:
	case VCODEC_GET_CORE_LOADING:
	case VCODEC_SET_CPU_OPP_LIMIT:
	case VCODEC_MB:
		return 0;
	default:
		/* Be permissive: unknown 'M' ioctls succeed so the userspace
		 * state machine isn't aborted. Logged for triage. */
		if (_IOC_TYPE(cmd) == MFV_IOC_MAGIC) {
			dev_info_ratelimited(vc->dev,
				"unhandled ioctl nr=0x%x -> success\n",
				_IOC_NR(cmd));
			return 0;
		}
		return -ENOTTY;
	}
}

static const struct file_operations vcodec_fops = {
	.owner		= THIS_MODULE,
	.open		= vcodec_open,
	.release	= vcodec_release,
	.mmap		= vcodec_mmap,
	.unlocked_ioctl	= vcodec_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,	/* NULL on 32-bit; harmless */
};

/* -------------------------------------------------------------- probe/remove */

static int vcodec_larb_link(struct vcodec_shim *vc)
{
	struct device_node *np;
	struct platform_device *larb_pdev;
	int ret;

	np = of_parse_phandle(vc->dev->of_node, "mediatek,larb", 0);
	if (!np) {
		dev_err(vc->dev, "missing mediatek,larb phandle\n");
		return -EINVAL;
	}
	larb_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!larb_pdev)
		return -EPROBE_DEFER;
	if (!platform_get_drvdata(larb_pdev)) {	/* larb driver not bound yet */
		put_device(&larb_pdev->dev);
		return -EPROBE_DEFER;
	}
	vc->larb_dev = &larb_pdev->dev;

	/* Same approach as the lima MT7623 fix: model the larb as a PM
	 * supplier and hold it resumed for our bound lifetime so the VPU
	 * register block (behind larb1 + MT2701_POWER_DOMAIN_VDEC) is on. */
	vc->larb_link = device_link_add(vc->dev, vc->larb_dev,
					DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);
	if (!vc->larb_link) {
		dev_err(vc->dev, "failed to link SMI larb\n");
		put_device(vc->larb_dev);
		return -EINVAL;
	}
	ret = pm_runtime_resume_and_get(vc->larb_dev);
	if (ret) {
		dev_err(vc->dev, "failed to power SMI larb: %d\n", ret);
		device_link_del(vc->larb_link);
		put_device(vc->larb_dev);
		return ret;
	}
	return 0;
}

static int vcodec_probe(struct platform_device *pdev)
{
	struct vcodec_shim *vc;
	int ret;

	vc = devm_kzalloc(&pdev->dev, sizeof(*vc), GFP_KERNEL);
	if (!vc)
		return -ENOMEM;
	vc->dev = &pdev->dev;
	sema_init(&vc->hw_sem, 1);
	spin_lock_init(&vc->hw_owner_lock);

	ret = dma_set_mask_and_coherent(vc->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/*
	 * ALLOC_MVA maps a scattered user buffer and requires the IOMMU to
	 * coalesce it into a single IOVA (nents==1) so the MVA is contiguous
	 * for the VPU. dma_map_sg() only merges segments up to the device's
	 * max_seg_size, which defaults to 64KB -- so any buffer larger than
	 * 64KB (e.g. H.264 work/frame buffers) splits into nents>1 and
	 * ALLOC_MVA fails with -ENOMEM. Lift the cap to the full 32-bit space
	 * so the whole buffer merges into one IOVA regardless of size.
	 */
	dma_set_max_seg_size(vc->dev, DMA_BIT_MASK(32));

	vc->vdec_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vc->vdec_base))
		return PTR_ERR(vc->vdec_base);

	ret = vcodec_larb_link(vc);
	if (ret)
		return ret;

	vc->mdev.minor = MISC_DYNAMIC_MINOR;
	vc->mdev.name = "Vcodec";	/* -> /dev/Vcodec */
	vc->mdev.fops = &vcodec_fops;
	ret = misc_register(&vc->mdev);
	if (ret) {
		dev_err(vc->dev, "misc_register failed: %d\n", ret);
		goto err_larb;
	}

	platform_set_drvdata(pdev, vc);
	g_vc = vc;

	/* Register the /proc/M4U_device front-end on the same struct device so
	 * the blob's IOMMU mappings share this driver's dma domain. */
	ret = vcodec_m4u_init(vc);
	if (ret) {
		dev_err(vc->dev, "M4U front-end init failed: %d\n", ret);
		goto err_misc;
	}

	dev_info(vc->dev, "/dev/Vcodec ready (vdec@%pa, larb powered)\n",
		 &pdev->resource[0].start);
	return 0;

err_misc:
	g_vc = NULL;
	misc_deregister(&vc->mdev);
err_larb:
	pm_runtime_put(vc->larb_dev);
	device_link_del(vc->larb_link);
	put_device(vc->larb_dev);
	return ret;
}

static void vcodec_remove(struct platform_device *pdev)
{
	struct vcodec_shim *vc = platform_get_drvdata(pdev);

	vcodec_m4u_exit();
	misc_deregister(&vc->mdev);
	pm_runtime_put(vc->larb_dev);
	device_link_del(vc->larb_link);
	put_device(vc->larb_dev);
	g_vc = NULL;
}

static const struct of_device_id vcodec_of_ids[] = {
	{ .compatible = "mediatek,mt7623-vcodec-shim" },
	{}
};
MODULE_DEVICE_TABLE(of, vcodec_of_ids);

static struct platform_driver vcodec_driver = {
	.probe	= vcodec_probe,
	.remove	= vcodec_remove,
	.driver	= {
		.name		= "mtk-vcodec-shim",
		.of_match_table	= vcodec_of_ids,
	},
};
module_platform_driver(vcodec_driver);

MODULE_DESCRIPTION("MT7623 /dev/Vcodec shim for the prebuilt MTK VPU userspace");
MODULE_LICENSE("GPL");

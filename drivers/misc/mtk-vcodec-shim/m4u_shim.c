// SPDX-License-Identifier: GPL-2.0
/*
 * MT7623 M4U (/proc/M4U_device) shim
 *
 * The prebuilt VPU userspace allocates its decoder working buffers with plain
 * malloc()/memalign() -- so physically SCATTERED pages -- and then asks
 * MediaTek's M4U (the gen1 IOMMU in front of the VDEC/VENC larbs) to map them
 * and hand back a single contiguous "MVA" the hardware masters with. Mainline
 * exposes that IOMMU through the generic framework (MTK_IOMMU_V1), not through
 * MediaTek's legacy /proc/M4U_device + MTK_M4U_T_* ioctl ABI that libm4u.so
 * speaks. This file provides exactly that ABI and bridges it to the dma-mapping
 * API on the vcodec shim's struct device.
 *
 * How the bridge works (see docs/vpu/m4u-notes.md for the full recovery):
 *
 *   ALLOC_MVA: pin_user_pages() the scattered range, build an sg_table that
 *   keeps the sub-page start offset, then dma_map_sgtable(). When the vcodec
 *   node has an "iommus" property, MTK_IOMMU_V1 has attached an arm_iommu
 *   mapping to the device, and arm_iommu_map_sg() coalesces the page-aligned
 *   segments into ONE contiguous IOVA -> nents==1 -> that IOVA is the MVA.
 *   Without "iommus" (bypass / first board bring-up) dma_map_sgtable() returns
 *   physical addresses and only sub-page (single physically-contiguous) ranges
 *   collapse to nents==1; a scattered multi-page buffer yields nents>1 and we
 *   fail LOUDLY so the missing translation is unmissable in dmesg.
 *
 *   Either way the contract is identical: require nents==1, MVA =
 *   sg_dma_address(first sg) (which already carries the sub-page offset).
 *
 *   CACHE_SYNC does dma_sync on the pin found by VA; DEALLOC unmaps+unpins.
 *   Every other request word is accepted as success (the real work happens at
 *   map time / at IOMMU attach). CONSTRUCT/DECONSTRUCT carry a NULL arg.
 *
 * Registered by the vcodec shim's probe so both device nodes share one struct
 * device (hence one dma / IOMMU domain).
 */

#define pr_fmt(fmt) "m4u-shim: " fmt

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/iommu.h>

#include "vcodec_m4u_abi.h"
#include "vcodec_shim_priv.h"

/* One mapped, pinned userspace buffer. */
struct m4u_pin {
	struct list_head node;
	unsigned long va;		/* original userspace VA (sub-page ok) */
	unsigned int size;
	unsigned int port;
	dma_addr_t mva;
	struct page **pages;
	unsigned int npages;
	struct sg_table sgt;
};

/* Per-open M4U client (one MTKM4UDrv instance). */
struct m4u_client {
	struct list_head pins;
	struct mutex lock;
};

static struct vcodec_shim *g_vc;	/* set by vcodec_m4u_init() */
static struct proc_dir_entry *g_proc;

/* ---------------------------------------------------------------- helpers */

/* Find the pin whose VA range covers [va, va+size). */
static struct m4u_pin *m4u_find_by_va(struct m4u_client *c, unsigned long va)
{
	struct m4u_pin *p;

	list_for_each_entry(p, &c->pins, node)
		if (va >= p->va && va < p->va + p->size)
			return p;
	return NULL;
}

static struct m4u_pin *m4u_find_by_mva(struct m4u_client *c, dma_addr_t mva)
{
	struct m4u_pin *p;

	list_for_each_entry(p, &c->pins, node)
		if (p->mva == mva)
			return p;
	return NULL;
}

static void m4u_pin_free(struct m4u_pin *p)
{
	dma_unmap_sgtable(g_vc->dev, &p->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(&p->sgt);
	unpin_user_pages_dirty_lock(p->pages, p->npages, true);
	kvfree(p->pages);
	kfree(p);
}

/* ----------------------------------------------------------- ALLOC / FREE */

static long m4u_alloc_mva(struct m4u_client *c, void __user *uarg)
{
	struct m4u_module_gen1 a;
	struct m4u_pin *p;
	unsigned long start, off;
	long pinned;
	int ret;

	if (copy_from_user(&a, uarg, sizeof(a)))
		return -EFAULT;
	if (!a.buf_addr || !a.buf_size)
		return -EINVAL;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->va = a.buf_addr;
	p->size = a.buf_size;
	p->port = a.port;

	off = offset_in_page(p->va);
	start = p->va & PAGE_MASK;
	p->npages = DIV_ROUND_UP(off + p->size, PAGE_SIZE);

	p->pages = kvmalloc_array(p->npages, sizeof(*p->pages), GFP_KERNEL);
	if (!p->pages) {
		ret = -ENOMEM;
		goto err_free;
	}

	pinned = pin_user_pages_fast(start, p->npages,
				     FOLL_WRITE | FOLL_LONGTERM, p->pages);
	if (pinned < 0) {
		ret = pinned;
		goto err_pages;
	}
	if (pinned != p->npages) {
		/* partial pin: only the first `pinned` pages are held */
		unpin_user_pages(p->pages, pinned);
		ret = -EFAULT;
		goto err_pages;
	}

	ret = sg_alloc_table_from_pages(&p->sgt, p->pages, p->npages, off,
					p->size, GFP_KERNEL);
	if (ret)
		goto err_unpin;

	ret = dma_map_sgtable(g_vc->dev, &p->sgt, DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto err_sgt;

	/* The whole point of the M4U: the VPU masters ONE contiguous address.
	 * With translation on, arm_iommu_map_sg merged the scattered pages into
	 * a single IOVA. In bypass mode only a physically-contiguous range
	 * survives as nents==1; anything else means we genuinely need the IOMMU
	 * turned on (add "iommus" to the vcodec DT node). */
	if (p->sgt.nents != 1) {
		dev_err(g_vc->dev,
			"ALLOC_MVA: buffer not contiguous after map (port=%u va=0x%lx size=%u npages=%u nents=%u) -- enable IOMMU translation on the vcodec node\n",
			p->port, p->va, p->size, p->npages, p->sgt.nents);
		ret = -ENOMEM;
		goto err_unmap;
	}

	p->mva = sg_dma_address(p->sgt.sgl);	/* carries the sub-page offset */

	mutex_lock(&c->lock);
	list_add(&p->node, &c->pins);
	mutex_unlock(&c->lock);

	/* Blob reads the MVA back from mva_start (0x0c) only. */
	if (put_user((u32)p->mva, (u32 __user *)((char __user *)uarg +
						  M4U_GEN1_MVA_OFFSET))) {
		mutex_lock(&c->lock);
		list_del(&p->node);
		mutex_unlock(&c->lock);
		ret = -EFAULT;
		goto err_unmap;
	}

	dev_dbg(g_vc->dev, "ALLOC_MVA port=%u va=0x%lx size=%u -> mva=0x%08x\n",
		p->port, p->va, p->size, (u32)p->mva);
	return 0;

err_unmap:
	dma_unmap_sgtable(g_vc->dev, &p->sgt, DMA_BIDIRECTIONAL, 0);
err_sgt:
	sg_free_table(&p->sgt);
err_unpin:
	unpin_user_pages(p->pages, p->npages);
err_pages:
	kvfree(p->pages);
err_free:
	kfree(p);
	return ret;
}

static long m4u_dealloc_mva(struct m4u_client *c, void __user *uarg)
{
	struct m4u_module_gen1 a;
	struct m4u_pin *p;

	if (copy_from_user(&a, uarg, sizeof(a)))
		return -EFAULT;

	mutex_lock(&c->lock);
	p = a.mva_start ? m4u_find_by_mva(c, a.mva_start)
			: m4u_find_by_va(c, a.buf_addr);
	if (p)
		list_del(&p->node);
	mutex_unlock(&c->lock);

	if (!p) {
		dev_warn(g_vc->dev, "DEALLOC_MVA: unknown va=0x%x mva=0x%x\n",
			 a.buf_addr, a.mva_start);
		return 0;
	}
	m4u_pin_free(p);
	return 0;
}

static long m4u_query_mva(struct m4u_client *c, void __user *uarg)
{
	struct m4u_module_gen1 a;
	struct m4u_pin *p;
	u32 mva = 0;

	if (copy_from_user(&a, uarg, sizeof(a)))
		return -EFAULT;

	mutex_lock(&c->lock);
	p = m4u_find_by_va(c, a.buf_addr);
	if (p)
		mva = (u32)(p->mva + (a.buf_addr - p->va));
	mutex_unlock(&c->lock);

	return put_user(mva, (u32 __user *)((char __user *)uarg +
					    M4U_GEN1_MVA_OFFSET));
}

/* ------------------------------------------------------------- CACHE_SYNC */

static void m4u_sync_one(struct m4u_pin *p, u32 type)
{
	switch (type) {
	case M4U_CACHE_CLEAN_BY_RANGE:
	case M4U_CACHE_CLEAN_ALL:
		dma_sync_sgtable_for_device(g_vc->dev, &p->sgt, DMA_TO_DEVICE);
		break;
	case M4U_CACHE_INVALID_BY_RANGE:
	case M4U_CACHE_INVALID_ALL:
		dma_sync_sgtable_for_cpu(g_vc->dev, &p->sgt, DMA_FROM_DEVICE);
		break;
	case M4U_CACHE_FLUSH_BY_RANGE:
	case M4U_CACHE_FLUSH_ALL:
	default:
		dma_sync_sgtable_for_device(g_vc->dev, &p->sgt,
					    DMA_BIDIRECTIONAL);
		break;
	}
}

static long m4u_cache_sync(struct m4u_client *c, void __user *uarg)
{
	struct m4u_cache_gen1 a;
	struct m4u_pin *p;
	bool all;

	if (copy_from_user(&a, uarg, sizeof(a)))
		return -EFAULT;

	all = a.sync_type >= M4U_CACHE_CLEAN_ALL;

	mutex_lock(&c->lock);
	if (all) {
		list_for_each_entry(p, &c->pins, node)
			m4u_sync_one(p, a.sync_type);
	} else {
		p = m4u_find_by_va(c, a.va);
		if (p)
			m4u_sync_one(p, a.sync_type);
		else
			dev_warn_ratelimited(g_vc->dev,
				"CACHE_SYNC: no pin for va=0x%x\n", a.va);
	}
	mutex_unlock(&c->lock);
	return 0;
}

/* -------------------------------------------------------------- proc fops */

static int m4u_open(struct inode *inode, struct file *file)
{
	struct m4u_client *c;

	if (!g_vc)
		return -ENODEV;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	INIT_LIST_HEAD(&c->pins);
	mutex_init(&c->lock);
	file->private_data = c;
	return 0;
}

static int m4u_release(struct inode *inode, struct file *file)
{
	struct m4u_client *c = file->private_data;
	struct m4u_pin *p, *tmp;

	list_for_each_entry_safe(p, tmp, &c->pins, node) {
		list_del(&p->node);
		m4u_pin_free(p);
	}
	kfree(c);
	return 0;
}

static long m4u_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct m4u_client *c = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case MTK_M4U_T_ALLOC_MVA:
		return m4u_alloc_mva(c, uarg);
	case MTK_M4U_T_DEALLOC_MVA:
		return m4u_dealloc_mva(c, uarg);
	case MTK_M4U_T_QUERY_MVA:
		return m4u_query_mva(c, uarg);
	case MTK_M4U_T_CACHE_SYNC:
		return m4u_cache_sync(c, uarg);
	case MTK_M4U_T_CACHE_FLUSH_ALL: {
		struct m4u_pin *p;

		mutex_lock(&c->lock);
		list_for_each_entry(p, &c->pins, node)
			m4u_sync_one(p, M4U_CACHE_FLUSH_ALL);
		mutex_unlock(&c->lock);
		return 0;
	}
	/*
	 * Everything below is accepted as success. Port config / TLB / power /
	 * monitor state is either handled implicitly by the IOMMU attach and
	 * dma_map (translation, TLB) or is irrelevant to the shim. CONSTRUCT /
	 * DECONSTRUCT arrive with a NULL arg and MUST NOT be rejected for it.
	 */
	case MTK_M4U_T_M4UDrv_CONSTRUCT:
	case MTK_M4U_T_M4UDrv_DECONSTRUCT:
	case MTK_M4U_T_CONFIG_PORT:
	case MTK_M4U_T_CONFIG_PORT_ROTATOR:
	case MTK_M4U_T_POWER_ON:
	case MTK_M4U_T_POWER_OFF:
	case MTK_M4U_T_DUMP_REG:
	case MTK_M4U_T_DUMP_INFO:
	case MTK_M4U_T_DUMP_PAGETABLE:
	case MTK_M4U_T_INSERT_TLB_RANGE:
	case MTK_M4U_T_INSERT_WRAP_RANGE:
	case MTK_M4U_T_INVALID_TLB_RANGE:
	case MTK_M4U_T_INVALID_TLB_ALL:
	case MTK_M4U_T_MANUAL_INSERT_ENTRY:
	case MTK_M4U_T_RESET_MVA_RELEASE_TLB:
	case MTK_M4U_T_REGISTER_BUFFER:
	case MTK_M4U_T_MONITOR_START:
	case MTK_M4U_T_MONITOR_STOP:
	case MTK_M4U_T_ALLOC_MVA_SEC:
	case MTK_M4U_T_DEALLOC_MVA_SEC:
		return 0;
	default:
		if (_IOC_TYPE(cmd) == MTK_M4U_MAGIC) {
			dev_info_ratelimited(g_vc->dev,
				"unhandled M4U ioctl nr=0x%x -> success\n",
				_IOC_NR(cmd));
			return 0;
		}
		return -ENOTTY;
	}
}

static const struct proc_ops m4u_proc_ops = {
	.proc_open		= m4u_open,
	.proc_release		= m4u_release,
	.proc_ioctl		= m4u_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl	= m4u_ioctl,	/* 32-bit userspace; same layout */
#endif
};

/* --------------------------------------------------------------- init/exit */

int vcodec_m4u_init(struct vcodec_shim *vc)
{
	g_vc = vc;
	g_proc = proc_create("M4U_device", 0666, NULL, &m4u_proc_ops);
	if (!g_proc) {
		g_vc = NULL;
		return -ENOMEM;
	}
	dev_info(vc->dev, "/proc/M4U_device ready (%s)\n",
		 device_iommu_mapped(vc->dev) ? "IOMMU translation on"
					      : "bypass: PA=MVA, contiguous only");
	return 0;
}

void vcodec_m4u_exit(void)
{
	if (g_proc) {
		proc_remove(g_proc);
		g_proc = NULL;
	}
	g_vc = NULL;
}

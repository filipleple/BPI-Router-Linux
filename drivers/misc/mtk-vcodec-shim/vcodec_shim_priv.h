/* SPDX-License-Identifier: GPL-2.0 */
/* Shared state between the /dev/Vcodec front-end and the /proc/M4U_device
 * front-end of the MT7623 vcodec shim. Both live in one module so they share
 * a single struct device (hence one dma-mapping / IOMMU domain). */
#ifndef _VCODEC_SHIM_PRIV_H_
#define _VCODEC_SHIM_PRIV_H_

#include <linux/miscdevice.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>

struct vcodec_shim {
	struct device *dev;
	void __iomem *vdec_base;	/* ioremap of reg[0] (0x16020000) */
	struct device *larb_dev;
	struct device_link *larb_link;
	struct miscdevice mdev;

	/* HW ownership (VCODEC_LOCKHW / VCODEC_UNLOCKHW). A counting semaphore
	 * (init 1) rather than a mutex: the prebuilt userspace may lock in one
	 * thread and unlock/close in another, and we must be able to release a
	 * lock left held by a client that crashed (done in the /dev/Vcodec
	 * .release handler). Cross-task mutex_unlock() would be UB. */
	struct semaphore hw_sem;
	spinlock_t hw_owner_lock;
	struct file *hw_owner;		/* open file that holds hw_sem, or NULL */
};

/* /proc/M4U_device front-end (m4u_shim.c). */
int vcodec_m4u_init(struct vcodec_shim *vc);
void vcodec_m4u_exit(void);

#endif /* _VCODEC_SHIM_PRIV_H_ */

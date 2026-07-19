/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT7623 M4U (/proc/M4U_device) ABI — the request words and argument struct
 * layouts the prebuilt armhf userspace (libm4u.so / MTKM4UDrv) issues.
 *
 * ioctl numbers: recovered from libm4u.so literal pools and cross-checked
 * against the android-mtk-3.18 m4u driver's m4u_priv.h (magic 'g'); see
 * docs/vpu/m4u-notes.md.
 *
 * The argument structs are the GEN1 (3.10 / MT8127-era) layout the MT7623
 * blob uses, which DIFFERS from the 3.18 M4U_MOUDLE_STRUCT: the MVA is read
 * back from offset 0x0c, and the whole struct is 0x2c bytes. Do NOT "tidy"
 * these -- the offsets are the ABI contract.
 */
#ifndef _VCODEC_M4U_ABI_H_
#define _VCODEC_M4U_ABI_H_

#include <linux/types.h>
#include <linux/ioctl.h>

#define MTK_M4U_MAGIC	'g'

#define MTK_M4U_T_POWER_ON		_IOW(MTK_M4U_MAGIC, 0, int)
#define MTK_M4U_T_POWER_OFF		_IOW(MTK_M4U_MAGIC, 1, int)
#define MTK_M4U_T_DUMP_REG		_IOW(MTK_M4U_MAGIC, 2, int)
#define MTK_M4U_T_DUMP_INFO		_IOW(MTK_M4U_MAGIC, 3, int)
#define MTK_M4U_T_ALLOC_MVA		_IOWR(MTK_M4U_MAGIC, 4, int)	/* 0xc0046704 */
#define MTK_M4U_T_DEALLOC_MVA		_IOW(MTK_M4U_MAGIC, 5, int)
#define MTK_M4U_T_INSERT_TLB_RANGE	_IOW(MTK_M4U_MAGIC, 6, int)
#define MTK_M4U_T_INVALID_TLB_RANGE	_IOW(MTK_M4U_MAGIC, 7, int)
#define MTK_M4U_T_INVALID_TLB_ALL	_IOW(MTK_M4U_MAGIC, 8, int)
#define MTK_M4U_T_MANUAL_INSERT_ENTRY	_IOW(MTK_M4U_MAGIC, 9, int)
#define MTK_M4U_T_CACHE_SYNC		_IOW(MTK_M4U_MAGIC, 10, int)
#define MTK_M4U_T_CONFIG_PORT		_IOW(MTK_M4U_MAGIC, 11, int)
#define MTK_M4U_T_INSERT_WRAP_RANGE	_IOW(MTK_M4U_MAGIC, 13, int)
#define MTK_M4U_T_MONITOR_START		_IOW(MTK_M4U_MAGIC, 14, int)
#define MTK_M4U_T_MONITOR_STOP		_IOW(MTK_M4U_MAGIC, 15, int)
#define MTK_M4U_T_RESET_MVA_RELEASE_TLB	_IOW(MTK_M4U_MAGIC, 16, int)
#define MTK_M4U_T_CONFIG_PORT_ROTATOR	_IOW(MTK_M4U_MAGIC, 17, int)
#define MTK_M4U_T_QUERY_MVA		_IOW(MTK_M4U_MAGIC, 18, int)
#define MTK_M4U_T_M4UDrv_CONSTRUCT	_IOW(MTK_M4U_MAGIC, 19, int)
#define MTK_M4U_T_M4UDrv_DECONSTRUCT	_IOW(MTK_M4U_MAGIC, 20, int)
#define MTK_M4U_T_DUMP_PAGETABLE	_IOW(MTK_M4U_MAGIC, 21, int)
#define MTK_M4U_T_REGISTER_BUFFER	_IOW(MTK_M4U_MAGIC, 22, int)
#define MTK_M4U_T_CACHE_FLUSH_ALL	_IOW(MTK_M4U_MAGIC, 23, int)
#define MTK_M4U_T_ALLOC_MVA_SEC		_IOWR(MTK_M4U_MAGIC, 26, int)	/* 0xc004671a */
#define MTK_M4U_T_DEALLOC_MVA_SEC	_IOW(MTK_M4U_MAGIC, 27, int)

/*
 * ALLOC_MVA / DEALLOC_MVA / QUERY_MVA / INSERT_TLB_RANGE argument.
 * 0x2c (44) bytes. MVA is written back into mva_start (0x0c).
 */
struct m4u_module_gen1 {
	__u32 port;		/* 0x00 in  (11 = VDEC_MC_EXT for vdec allocs) */
	__u32 buf_addr;		/* 0x04 in  userspace VA (may be sub-page-aligned) */
	__u32 buf_size;		/* 0x08 in  bytes */
	__u32 mva_start;	/* 0x0c in: 0 (enabled) or -1 (disabled); OUT: MVA.
				 *      INSERT_TLB_RANGE passes range start here */
	__u32 mva_end;		/* 0x10 INSERT_TLB_RANGE: range end */
	__u32 prio;		/* 0x14 */
	__u32 extra;		/* 0x18 */
	__u32 rsv1c;		/* 0x1c */
	__u32 rsv20;		/* 0x20 */
	__u32 security;		/* 0x24 in */
	__u32 cacheable;	/* 0x28 in */
};

#define M4U_GEN1_MVA_OFFSET	0x0c	/* byte offset of mva_start in the arg */

/* CACHE_SYNC argument, 0x10 (16) bytes -- NO mva field; look up by VA. */
struct m4u_cache_gen1 {
	__u32 port;		/* 0x00 */
	__u32 sync_type;	/* 0x04 M4U_CACHE_SYNC_ENUM */
	__u32 va;		/* 0x08 */
	__u32 size;		/* 0x0c */
};

/* M4U_CACHE_SYNC_ENUM (m4u.h) */
enum {
	M4U_CACHE_CLEAN_BY_RANGE = 0,
	M4U_CACHE_INVALID_BY_RANGE = 1,
	M4U_CACHE_FLUSH_BY_RANGE = 2,
	M4U_CACHE_CLEAN_ALL = 3,
	M4U_CACHE_INVALID_ALL = 4,
	M4U_CACHE_FLUSH_ALL = 5,
};

#endif /* _VCODEC_M4U_ABI_H_ */

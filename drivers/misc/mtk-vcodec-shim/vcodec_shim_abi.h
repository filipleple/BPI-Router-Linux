/* SPDX-License-Identifier: GPL-2.0 */
/*
 * /dev/Vcodec ABI — vendored verbatim from the MediaTek android-mtk-3.18
 * videocodec driver so the prebuilt armhf userspace (libvcodec_utility.so)
 * sees byte-identical ioctl argument layouts.
 *
 * ioctl numbers: mt8173/videocodec_kernel_driver.h  (MFV_IOC_MAGIC = 'M')
 * struct/enum types: videocodec/include/val_types_public.h
 *
 * Target is 32-bit ARM (MT7623, arch/arm) with 32-bit userspace, so the
 * u64-extended unions below resolve to the same layout the blob was built for.
 * Do NOT "tidy" these structs — the offsets are the ABI contract.
 */
#ifndef _VCODEC_SHIM_ABI_H_
#define _VCODEC_SHIM_ABI_H_

#include <linux/types.h>

/* ---- base types (val_types_public.h) ---- */
typedef void			VAL_VOID_T;
typedef char			VAL_BOOL_T;
typedef unsigned char		VAL_UINT8_T;
typedef unsigned int		VAL_UINT32_T;
typedef unsigned long long	VAL_UINT64_T;
typedef unsigned long		VAL_ULONG_T;

#define IRQ_STATUS_MAX_NUM	16
#define VCODEC_THREAD_MAX_NUM	16

typedef enum _VAL_DRIVER_TYPE_T {
	VAL_DRIVER_TYPE_NONE = 0,
	VAL_DRIVER_TYPE_MP4_ENC,
	VAL_DRIVER_TYPE_MP4_DEC,
	VAL_DRIVER_TYPE_H263_ENC,
	VAL_DRIVER_TYPE_H263_DEC,
	VAL_DRIVER_TYPE_H264_ENC,
	VAL_DRIVER_TYPE_H264_DEC,
	VAL_DRIVER_TYPE_SORENSON_SPARK_DEC,
	VAL_DRIVER_TYPE_VC1_SP_DEC,
	VAL_DRIVER_TYPE_RV9_DEC,
	VAL_DRIVER_TYPE_MP1_MP2_DEC,
	VAL_DRIVER_TYPE_XVID_DEC,
	VAL_DRIVER_TYPE_DIVX4_DIVX5_DEC,
	VAL_DRIVER_TYPE_VC1_MP_WMV9_DEC,
	VAL_DRIVER_TYPE_RV8_DEC,
	VAL_DRIVER_TYPE_WMV7_DEC,
	VAL_DRIVER_TYPE_WMV8_DEC,
	VAL_DRIVER_TYPE_AVS_DEC,
	VAL_DRIVER_TYPE_DIVX_3_11_DEC,
	VAL_DRIVER_TYPE_H264_DEC_MAIN,
	VAL_DRIVER_TYPE_H264_DEC_MAIN_CABAC,
	VAL_DRIVER_TYPE_VP8_DEC,
	VAL_DRIVER_TYPE_MP2_DEC,
	VAL_DRIVER_TYPE_VP9_DEC,
	VAL_DRIVER_TYPE_VP8_ENC,
	VAL_DRIVER_TYPE_VC1_ADV_DEC,
	VAL_DRIVER_TYPE_VC1_DEC,
	VAL_DRIVER_TYPE_JPEG_ENC,
	VAL_DRIVER_TYPE_HEVC_ENC,
	VAL_DRIVER_TYPE_HEVC_DEC,
	VAL_DRIVER_TYPE_H264_ENC_LIVEPHOTO,
	VAL_DRIVER_TYPE_MAX = 0xFFFFFFFF
} VAL_DRIVER_TYPE_T;

typedef enum _VAL_MEM_TYPE_T {
	VAL_MEM_TYPE_FOR_SW = 0,
	VAL_MEM_TYPE_FOR_HW_CACHEABLE,
	VAL_MEM_TYPE_FOR_HW_CACHEABLE_MCI,
	VAL_MEM_TYPE_FOR_HW_NONCACHEABLE,
	VAL_MEM_TYPE_MAX = 0xFFFFFFFF
} VAL_MEM_TYPE_T;

typedef enum _VAL_MEM_ALIGN_T {
	VAL_MEM_ALIGN_1 = 1,
	VAL_MEM_ALIGN_MAX = 0xFFFFFFFF
} VAL_MEM_ALIGN_T;

typedef enum _VAL_MEM_CODEC_T {
	VAL_MEM_CODEC_FOR_VENC = 0,
	VAL_MEM_CODEC_FOR_VDEC,
	VAL_MEM_CODEC_MAX = 0xFFFFFFFF
} VAL_MEM_CODEC_T;

/* ---- VCODEC_WAITISR arg ---- */
typedef struct _VAL_ISR_T {
	VAL_VOID_T	*pvHandle;
	VAL_UINT32_T	u4HandleSize;
	VAL_DRIVER_TYPE_T eDriverType;
	VAL_VOID_T	*pvIsrFunction;
	VAL_VOID_T	*pvReserved;
	VAL_UINT32_T	u4ReservedSize;
	VAL_UINT32_T	u4TimeoutMs;
	VAL_UINT32_T	u4IrqStatusNum;
	VAL_UINT32_T	u4IrqStatus[IRQ_STATUS_MAX_NUM];
} VAL_ISR_T;

/* ---- VCODEC_LOCKHW / VCODEC_UNLOCKHW arg ---- */
typedef struct _VAL_HW_LOCK_T {
	VAL_VOID_T	*pvHandle;
	VAL_UINT32_T	u4HandleSize;
	VAL_VOID_T	*pvLock;
	VAL_UINT32_T	u4TimeoutMs;
	VAL_VOID_T	*pvReserved;
	VAL_UINT32_T	u4ReservedSize;
	VAL_DRIVER_TYPE_T eDriverType;
	VAL_BOOL_T	bSecureInst;
} VAL_HW_LOCK_T;

/* ---- VCODEC_ALLOC/FREE_NON_CACHE_BUFFER arg (u64-extended unions) ---- */
typedef struct _VAL_MEMORY_T {
	VAL_MEM_TYPE_T	eMemType;
	union { VAL_ULONG_T u4MemSize;       VAL_UINT64_T u4MemSize_ext64; };
	union { VAL_VOID_T *pvMemVa;         VAL_UINT64_T pvMemVa_ext64; };
	union { VAL_VOID_T *pvMemPa;         VAL_UINT64_T pvMemPa_ext64; };
	VAL_MEM_ALIGN_T	eAlignment;
	union { VAL_VOID_T *pvAlignMemVa;    VAL_UINT64_T pvAlignMemVa_ext64; };
	union { VAL_VOID_T *pvAlignMemPa;    VAL_UINT64_T pvAlignMemPa_ext64; };
	VAL_MEM_CODEC_T	eMemCodec;
	VAL_UINT32_T	i4IonShareFd;
	union { VAL_VOID_T *pIonBufhandle;   VAL_UINT64_T pIonBufhandle_ext64; };
	union { VAL_VOID_T *pvReserved;      VAL_UINT64_T pvReserved_ext64; };
	union { VAL_ULONG_T u4ReservedSize;  VAL_UINT64_T u4ReservedSize_ext64; };
} VAL_MEMORY_T;

/* ---- VCODEC_SET_THREAD_ID arg ---- */
typedef struct _VAL_VCODEC_THREAD_ID_T {
	VAL_UINT32_T	u4tid1;
	VAL_UINT32_T	u4tid2;
	VAL_UINT32_T	u4VCodecThreadNum;
	VAL_UINT32_T	u4VCodecThreadID[VCODEC_THREAD_MAX_NUM];
} VAL_VCODEC_THREAD_ID_T;

/* ---- ioctl numbers (mt8173/videocodec_kernel_driver.h) ---- */
#define MFV_IOC_MAGIC			'M'
#define VCODEC_WAITISR			_IOW(MFV_IOC_MAGIC, 0x0b, unsigned int)
#define VCODEC_LOCKHW			_IOW(MFV_IOC_MAGIC, 0x0d, unsigned int)
#define VCODEC_INC_ENC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x15, unsigned int)
#define VCODEC_DEC_ENC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x16, unsigned int)
#define VCODEC_INC_DEC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x17, unsigned int)
#define VCODEC_DEC_DEC_EMI_USER		_IOW(MFV_IOC_MAGIC, 0x18, unsigned int)
#define VCODEC_INITHWLOCK		_IOW(MFV_IOC_MAGIC, 0x20, unsigned int)
#define VCODEC_DEINITHWLOCK		_IOW(MFV_IOC_MAGIC, 0x21, unsigned int)
#define VCODEC_ALLOC_NON_CACHE_BUFFER	_IOW(MFV_IOC_MAGIC, 0x22, unsigned int)
#define VCODEC_FREE_NON_CACHE_BUFFER	_IOW(MFV_IOC_MAGIC, 0x23, unsigned int)
#define VCODEC_SET_THREAD_ID		_IOW(MFV_IOC_MAGIC, 0x24, unsigned int)
#define VCODEC_INC_PWR_USER		_IOW(MFV_IOC_MAGIC, 0x27, unsigned int)
#define VCODEC_DEC_PWR_USER		_IOW(MFV_IOC_MAGIC, 0x28, unsigned int)
#define VCODEC_GET_CPU_LOADING_INFO	_IOW(MFV_IOC_MAGIC, 0x29, unsigned int)
#define VCODEC_GET_CORE_LOADING		_IOW(MFV_IOC_MAGIC, 0x30, unsigned int)
#define VCODEC_GET_CORE_NUMBER		_IOW(MFV_IOC_MAGIC, 0x31, unsigned int)
#define VCODEC_SET_CPU_OPP_LIMIT	_IOW(MFV_IOC_MAGIC, 0x32, unsigned int)
#define VCODEC_UNLOCKHW			_IOW(MFV_IOC_MAGIC, 0x33, unsigned int)
#define VCODEC_MB			_IOW(MFV_IOC_MAGIC, 0x34, unsigned int)

#endif /* _VCODEC_SHIM_ABI_H_ */

# mtk-vcodec-shim — MT7623 /dev/Vcodec

Minimal char-device shim giving the prebuilt MediaTek armhf VPU userspace
(`libvcodec_utility.so` → `/dev/Vcodec`) what it needs to drive the MT7623N
hardware video decoder by direct register programming. Replaces the
android-mtk-3.18 `videocodec` BSP driver, which doesn't build on mainline
(removed `mtk_smi_larb_get`, no `drivers/misc/mediatek/`, no VAL/HAL framework).

See `../../../../MT7623N_VPU_RESEARCH_BRIEFING.md` (in the vpu-bringup workspace)
for how the ABI was recovered.

## Files
- `vcodec_shim_abi.h` — ioctl numbers + arg structs, vendored byte-identical from
  the android driver (the ABI contract with the blob; do not "tidy").
- `vcodec_shim.c`     — the driver.

## What it implements
- `/dev/Vcodec` (misc device).
- `mmap`: VDEC register banks (`0x16000000` + `0x16020000..0x16028fff`, noncached)
  and driver-allocated DMA-coherent buffers (`dma_mmap_coherent`).
- ioctls `_IOW('M', …)`: real LOCKHW/UNLOCKHW (mutex), WAITISR (poll), ALLOC/FREE
  buffer, GET_CORE_NUMBER=1; everything else no-op success.
- VDEC power: PM device link to `larb1` + `pm_runtime_resume_and_get` (same as the
  lima MT7623 larb fix) — pulls up `MT2701_POWER_DOMAIN_VDEC` + `CLK_VDEC_CKGEN/LARB`.
- Decode completion: **polls** `VDEC_BASE+0xA4` bit16 (the MT7623 VDEC GIC SPI is
  undocumented). No IRQ used.

## Build
`CONFIG_MTK_VCODEC_SHIM=y` is in `mt7623n_evb_fwu_defconfig`. Builds with the kernel
(uImage + `mt7623n-bananapi-bpi-r2.dtb`). DT node `vcodec@16020000` is in
`mt7623n.dtsi`.

## First-boot checks
- `dmesg | grep vcodec-shim` → "/dev/Vcodec ready (vdec@16020000, larb powered)".
- `ls -l /dev/Vcodec` exists.
- No `-EPROBE_DEFER` loop (means larb1 didn't bind — check `MTK_SMI`/scpsys).
- Push the armhf blobs, run an OMX decode / `omx_ut`.

## Likely next tweaks
- `WAITISR` currently returns `u4IrqStatus[0] = done-reg`; if the lib needs specific
  status registers post-decode, capture more from the VLD/MISC banks.
- M4U/IOMMU: the `vcodec` node has no `iommus` (M4U bypass) so `dma_alloc_coherent`
  hands out physical addresses the VPU can reach directly. If decode output is
  corrupt/faults, revisit IOMMU attach.
- To go interrupt-driven later: add `interrupts = <GIC_SPI N IRQ_TYPE_LEVEL_LOW>` to
  the DT node once N is known, and replace the poll in `vcodec_waitisr()` with a
  completion signalled from a real ISR.

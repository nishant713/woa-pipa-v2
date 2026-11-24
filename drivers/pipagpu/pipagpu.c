/*
 * pipagpu - boot-start helper for Xiaomi Pad 6 (pipa) GPU bring-up.
 *
 * Nothing powers the Adreno CX domain at boot, so qcsmmu8250 hangs the bus when it touches the
 * GPU SMMU (MMU1 @0x3DA0000). This driver has no device: its DriverEntry (boot phase, before PnP
 * starts demand-start device drivers) turns on the GCC GPU clocks, GPUCC AHB/CXO, the GPU CX GDSC
 * and the SMMU clock vote (offsets from Linux gcc-sm8250.c / gpucc-sm8250.c), then stays loaded.
 * Proven step by step by the pipapwr GPU probe.
 */
#include <ntddk.h>

#define LOG(fmt, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "pipagpu: " fmt "\n", __VA_ARGS__)

static PUCHAR Map(ULONGLONG pa, SIZE_T len)
{
    PHYSICAL_ADDRESS a; a.QuadPart = (LONGLONG)pa;
    return (PUCHAR)MmMapIoSpaceEx(a, len, PAGE_READWRITE | PAGE_NOCACHE);
}
static ULONG R(PUCHAR b, ULONG o) { return READ_REGISTER_ULONG((volatile ULONG *)(b + o)); }
static VOID  W(PUCHAR b, ULONG o, ULONG v) { WRITE_REGISTER_ULONG((volatile ULONG *)(b + o), v); }
static VOID  Set(PUCHAR b, ULONG o, ULONG bits) { W(b, o, R(b, o) | bits); KeStallExecutionProcessor(10); }

static VOID Unload(PDRIVER_OBJECT d) { UNREFERENCED_PARAMETER(d); }

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    PUCHAR gcc71, gcc52, gpucc, smmu; ULONG t, v;
    UNREFERENCED_PARAMETER(path);
    drv->DriverUnload = Unload;
    gcc71 = Map(0x00171000ull, 0x1000);
    gcc52 = Map(0x00152000ull, 0x1000);
    gpucc = Map(0x03D90000ull, 0x9000);
    smmu  = Map(0x03DA0000ull, 0x1000);
    if (!gcc71 || !gcc52 || !gpucc || !smmu) { LOG("map failed"); return STATUS_SUCCESS; }
    Set(gcc71, 0x004, 1);                          /* gcc_gpu_cfg_ahb_clk */
    Set(gcc52, 0x000, (1u << 15) | (1u << 16));    /* gcc_gpu_gpll0(_div)_clk_src votes */
    Set(gcc71, 0x00C, 1);                          /* gcc_gpu_memnoc_gfx_clk */
    Set(gcc71, 0x018, 1);                          /* gcc_gpu_snoc_dvm_gfx_clk */
    Set(gpucc, 0x1078, 1);                         /* gpu_cc_ahb_clk */
    Set(gpucc, 0x109C, 1);                         /* gpu_cc_cxo_clk */
    v = R(gpucc, 0x106C);
    W(gpucc, 0x106C, v & ~1u);                     /* GPU CX GDSC: clear SW_COLLAPSE */
    for (t = 0; t < 1000 && !(R(gpucc, 0x1540) & 0x80000000u); t++) KeStallExecutionProcessor(10);
    Set(gpucc, 0x5000, 1);                         /* gpu_cc_hlos1_vote_gpu_smmu_clk */
    LOG("GPU CX on: gdscr %08X -> %08X hw_ctrl %08X (%lu0 us); gpu smmu sCR0=%08X IDR0=%08X",
        v, R(gpucc, 0x106C), R(gpucc, 0x1540), t, R(smmu, 0), R(smmu, 0x20));
    /* Experiment: GX GDSC (GPU_CC_GX_GDSCR 0x100C). Normally the GMU owns it; qcdxkm8250 seems to
       touch GX registers and stall the bus. Clear SW_COLLAPSE and poll PWR_ON (bit31) with a timeout;
       this only succeeds if the GFX rail is already up. Never touches GX registers itself. */
    v = R(gpucc, 0x100C);
    W(gpucc, 0x100C, v & ~1u);
    for (t = 0; t < 1000 && !(R(gpucc, 0x100C) & 0x80000000u); t++) KeStallExecutionProcessor(10);
    LOG("GX gdscr %08X -> %08X (%lu0 us) %s", v, R(gpucc, 0x100C), t,
        (R(gpucc, 0x100C) & 0x80000000u) ? "ON" : "did not power up (GFX rail off?)");
    MmUnmapIoSpace(gcc71, 0x1000); MmUnmapIoSpace(gcc52, 0x1000); MmUnmapIoSpace(gpucc, 0x9000); MmUnmapIoSpace(smmu, 0x1000);
    return STATUS_SUCCESS;
}

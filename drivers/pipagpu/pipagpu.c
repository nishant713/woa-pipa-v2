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


/* ---- RPMh: vote the GFX rail (ARC "gfx.lvl") through an apps-RSC active TCS ----
 * cmd-db (reserved mem 0x80820000, sm8250.dtsi aop_cmd_db_mem) gives the ARC address and level
 * table; apps RSC DRV2 @0x18220000, TCS block +0xD00, TCS stride 0x2A0, cmd stride 0x14
 * (rpmh-rsc.c, sm8250 tcs-config: ACTIVE 2 first). One write, response required, then poll. */
#pragma pack(push, 1)
typedef struct { USHORT slv, hdr_off, data_off, cnt, ver, rsvd[3]; } RSC_HDR;
typedef struct { UCHAR id[8]; ULONG prio[2]; ULONG addr; USHORT len, off; } ENT_HDR;
#pragma pack(pop)

static BOOLEAN CmdDbFind(PUCHAR db, const char *name, USHORT want, ULONG *addr, ULONG *lvl)
{
    PUCHAR data = db + 8 + 8 * sizeof(RSC_HDR) + 8;
    ULONG i, j, k;
    if (db[4] != 0xDB || db[5] != 0x30 || db[6] != 0x03 || db[7] != 0x0C) { LOG("cmd-db: bad magic %02X%02X%02X%02X", db[4], db[5], db[6], db[7]); return FALSE; }
    for (i = 0; i < 8; i++) {
        RSC_HDR *h = (RSC_HDR *)(db + 8 + i * sizeof(RSC_HDR));
        if (!h->cnt) continue;
        for (j = 0; j < h->cnt; j++) {
            ENT_HDR *e = (ENT_HDR *)(data + h->hdr_off + j * sizeof(ENT_HDR));
            for (k = 0; k < 8 && name[k] && e->id[k] == (UCHAR)name[k]; k++);
            if (k < 8 && (name[k] || e->id[k])) continue;
            {
                USHORT *v = (USHORT *)(data + h->data_off + e->off);
                ULONG n = e->len / 2, best = n ? n - 1 : 0;
                for (k = 0; k < n; k++) if (v[k] >= want) { best = k; break; }
                LOG("cmd-db: %s addr=%05X levels=%lu pick hlvl %lu (vlvl %u)", name, e->addr, n, best, n ? v[best] : 0);
                *addr = e->addr; *lvl = best; return TRUE;
            }
        }
    }
    LOG("cmd-db: %s not found", name); return FALSE;
}


static VOID RpmhWrite(PUCHAR tcs, const char *name, ULONG addr, ULONG lvl)
{
    ULONG id, t;
    PUCHAR c;
    for (id = 0; id < 2; id++) if (R(tcs + id * 0x2A0, 0x18)) break;       /* DRV_STATUS != 0 -> idle */
    if (id == 2) { LOG("rpmh: no idle active TCS for %s", name); return; }
    c = tcs + id * 0x2A0;
    W(c, 0x30, 8 | (1u << 8) | (1u << 16));     /* MSGID: len 8, resp req, write */
    W(c, 0x34, addr);
    W(c, 0x38, lvl);
    W(c, 0x10, 1);
    W(c, 0x1C, 1);
    W(c, 0x14, 0);
    W(c, 0x14, 1u << 16);
    W(c, 0x14, (1u << 16) | (1u << 24));
    for (t = 0; t < 10000 && !(R(tcs, 0x04) & (1u << id)); t++) KeStallExecutionProcessor(10);
    LOG("rpmh: tcs%lu %s=%lu irq_status=%08X cmd_status=%08X (%lu0 us)%s", id, name, lvl,
        R(tcs, 0x04), R(c, 0x3C), t, t >= 10000 ? " TIMEOUT" : "");
    /* leave the TCS exactly as the PEP's RPMh survey expects a quiescent one: no enable, no
       wait-for-completion, no AMC mode, no pending IRQ (a leftover WAIT bit marks it "configured") */
    W(c, 0x1C, 0); W(c, 0x10, 0); W(c, 0x14, 0);
    W(c, 0x30, 0); W(c, 0x34, 0); W(c, 0x38, 0);
    W(tcs, 0x08, 1u << id);
    LOG("rpmh: tcs%lu after cleanup ctrl=%08X en=%08X wait=%08X status=%08X irq_en=%08X irq_st=%08X", id,
        R(c, 0x14), R(c, 0x1C), R(c, 0x10), R(c, 0x18), R(tcs, 0x00), R(tcs, 0x04));
}

static VOID VoteGfxRail(VOID)
{
    PUCHAR io = NULL, rsc = Map(0x18220000ull, 0x4000), tcs;
    PUCHAR db = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 0x20000, 'ugpP');
    ULONG addr, lvl, t, dbpa = 0x80820000, dblen = 0;
    {   /* AOP msg RAM dictionary (downstream "qcom,cmd-db" reg 0xC3F000C): cmd-db base, size */
        PUCHAR dict = Map(0x0C3F0000ull, 0x1000);
        if (dict) { dbpa = R(dict, 0xC); dblen = R(dict, 0x10); MmUnmapIoSpace(dict, 0x1000); }
        LOG("cmd-db: dictionary base=%08X size=%X", dbpa, dblen);
        if (!dbpa || dbpa == 0xFFFFFFFF) dbpa = 0x80820000;
    }
    io = Map(dbpa, 0x20000);
    if (!io || !rsc || !db) { LOG("rpmh: map/alloc failed io=%p rsc=%p", io, rsc); goto out; }
    /* copy with aligned 32-bit reads: device mappings fault on the unaligned accesses parsing needs */
    if (!dblen || dblen > 0x20000) dblen = 0x20000;
    RtlZeroMemory(db, 0x20000);
    for (t = 0; t < dblen; t += 4) *(ULONG *)(db + t) = R(io, t);
    tcs = rsc + 0xD00;
    /* MX must be >= GFX: raise MX to NOM first (ARC aggregates max, so this only raises), then GFX to SVS */
    if (CmdDbFind(db, "mx.lvl", 256, &addr, &lvl)) RpmhWrite(tcs, "mx.lvl", addr, lvl);
    if (CmdDbFind(db, "gfx.lvl", 128, &addr, &lvl)) RpmhWrite(tcs, "gfx.lvl", addr, lvl);
out:
    if (io) MmUnmapIoSpace(io, 0x20000);
    if (db) ExFreePoolWithTag(db, 'ugpP');
    if (rsc) MmUnmapIoSpace(rsc, 0x4000);
}

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
    /* (after the CX GDSC is up, or the branch registers do not hold) remaining GPUCC CX branches fed by AHB/CXO (gpucc-sm8250.c): the GPU SMMU's implementation
       page (0x3DA6000) stalled qcsmmu's write with only the above on */
    Set(gpucc, 0x107C, 1);                         /* gpu_cc_crc_ahb_clk */
    Set(gpucc, 0x1088, 1);                         /* gpu_cc_cx_apb_clk */
    Set(gpucc, 0x108C, 1);                         /* gpu_cc_cx_snoc_dvm_clk */
    Set(gpucc, 0x1004, 1);                         /* gpu_cc_cxo_aon_clk */
    LOG("CX branches: crc_ahb=%08X cx_apb=%08X cx_snoc_dvm=%08X cxo_aon=%08X",
        R(gpucc, 0x107C), R(gpucc, 0x1088), R(gpucc, 0x108C), R(gpucc, 0x1004));
    /* 0x3DA6000 (GPU SMMU "implementation defined 1" page) is NOT readable from HLOS: a read hangs the CPU */
    LOG("GPU CX on: gdscr %08X -> %08X hw_ctrl %08X (%lu0 us); gpu smmu sCR0=%08X IDR0=%08X",
        v, R(gpucc, 0x106C), R(gpucc, 0x1540), t, R(smmu, 0), R(smmu, 0x20));
    VoteGfxRail();
    /* GX GDSC (GPU_CC_GX_GDSCR 0x100C, CFG 0x1010, clamp 0x1508 - gpucc-sm8250.c gpu_gx_gdsc).
       Never touches GPU core registers itself. */
    v = R(gpucc, 0x100C);
    W(gpucc, 0x1508, R(gpucc, 0x1508) & ~1u);               /* deassert GMEM clamp (CLAMP_IO) */
    W(gpucc, 0x1508, R(gpucc, 0x1508) | 0x10u); KeStallExecutionProcessor(1);   /* AON_RESET: GMEM reset pulse */
    W(gpucc, 0x1508, R(gpucc, 0x1508) & ~0x10u);
    W(gpucc, 0x100C, v & ~1u);
    for (t = 0; t < 1000 && !(R(gpucc, 0x100C) & 0x80000000u) && !(R(gpucc, 0x1010) & 0x10000u); t++)
        KeStallExecutionProcessor(10);
    if ((R(gpucc, 0x100C) & 0x80000000u) || (R(gpucc, 0x1010) & 0x10000u)) {
        LOG("GX ON: gdscr %08X -> %08X cfg %08X clamp %08X (%lu0 us)", v, R(gpucc, 0x100C), R(gpucc, 0x1010), R(gpucc, 0x1508), t);
    } else {
        W(gpucc, 0x100C, v);                                 /* give up: restore collapse + clamp */
        W(gpucc, 0x1508, R(gpucc, 0x1508) | 1u);
        LOG("GX did not power up: gdscr %08X cfg %08X (%lu0 us) - restored", R(gpucc, 0x100C), R(gpucc, 0x1010), t);
    }
    MmUnmapIoSpace(gcc71, 0x1000); MmUnmapIoSpace(gcc52, 0x1000); MmUnmapIoSpace(gpucc, 0x9000); MmUnmapIoSpace(smmu, 0x1000);
    return STATUS_SUCCESS;
}

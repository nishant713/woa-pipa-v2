/*
 * pipabatt - battery miniclass for Xiaomi Pad 6 (pipa).
 *
 * Reads the PM8150B fuel gauge (FG gen4, SPMI SID 2 @0x4000) and charger (@0x1000) through the
 * SPMI PMIC arbiter v5 EE0 observer channel (read-only, same as Linux spmi-pmic-arb.c), and reports
 * them to Windows via battc.sys. Register meanings from pipa-mainline drivers/power/supply/qcom_fg.c
 * and qcom_pm8150b_charger.c. Binds to the PIPA\PM8150B_BATT child PDO created by pipapwr.
 */
#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>
#include <ntstrsafe.h>

#define LOG(fmt, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "pipabatt: " fmt "\n", __VA_ARGS__)

#define SPMI_CORE     0x0c440000ull
#define SPMI_CORE_SZ  0x1100
#define SPMI_OBS      0x0e600000ull
#define SPMI_OBS_SZ   0x10000

#define DESIGN_MWH    34300     /* energy-full-design 34.3 Wh (pipa-mainline dts) */
#define DESIGN_MV     3870      /* nominal */
#define POLL_MS       15000

typedef struct _BATT_CTX {
    WDFDEVICE dev;
    PVOID     classHandle;
    PUCHAR    core, obs;
    WDFTIMER  timer;
    WDFSPINLOCK lock;
    ULONG     tag;
    BOOLEAN   notifyArmed;
} BATT_CTX, *PBATT_CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BATT_CTX, BattCtx)

typedef struct _BATT_SAMPLE {
    BOOLEAN ok, online;
    ULONG   socPermille;   /* 0..1000 */
    ULONG   mV;
    LONG    mA;            /* positive = discharging (PMIC convention) */
    LONG    tempDeciC;
    UCHAR   chgState;      /* BATTERY_CHARGER_STATUS_1 [2:0] */
} BATT_SAMPLE;

static ULONG RR(PUCHAR b, ULONG o) { return READ_REGISTER_ULONG((volatile ULONG *)(b + o)); }
static VOID  RW(PUCHAR b, ULONG o, ULONG v) { WRITE_REGISTER_ULONG((volatile ULONG *)(b + o), v); }

static PUCHAR MapPhys(ULONGLONG pa, SIZE_T len)
{
    PHYSICAL_ADDRESS a; a.QuadPart = (LONGLONG)pa;
    return (PUCHAR)MmMapIoSpaceEx(a, len, PAGE_READWRITE | PAGE_NOCACHE);
}

static LONG SpmiApid(PBATT_CTX c, USHORT ppid)
{
    ULONG n, v;
    for (n = 0; 0x900 + 4 * n < SPMI_CORE_SZ; n++) {
        v = RR(c->core, 0x900 + 4 * n);
        if (v && ((v >> 8) & 0xFFF) == ppid) return (LONG)n;
    }
    return -1;
}

static NTSTATUS SpmiRead(PBATT_CTX c, UCHAR sid, USHORT addr, UCHAR *buf, ULONG len)
{
    LONG apid = SpmiApid(c, (USHORT)((sid << 8) | (addr >> 8)));
    ULONG off, st = 0, i, d, t;
    if (apid < 0 || len < 1 || len > 4) return STATUS_NOT_FOUND;
    off = 0x80 * (ULONG)apid;
    RW(c->obs, off + 0x00, (15u << 27) | ((addr & 0xFFu) << 4) | (len - 1));
    for (t = 0; t < 1000; t++) { st = RR(c->obs, off + 0x08); if (st) break; KeStallExecutionProcessor(1); }
    if (!(st & 1) || (st & 0xE)) return STATUS_IO_DEVICE_ERROR;
    d = RR(c->obs, off + 0x18);
    for (i = 0; i < len; i++) buf[i] = (UCHAR)(d >> (8 * i));
    return STATUS_SUCCESS;
}

static VOID Sample(PBATT_CTX c, BATT_SAMPLE *s)
{
    UCHAR b[4] = { 0 }; NTSTATUS st; ULONG raw;
    RtlZeroMemory(s, sizeof(*s));
    WdfSpinLockAcquire(c->lock);
    st = SpmiRead(c, 2, 0x4009, b, 2);                       /* BATT_MONOTONIC_SOC */
    if (NT_SUCCESS(st)) {
        raw = b[0] < b[1] ? b[0] : b[1];
        s->socPermille = raw <= 1 ? 0 : ((raw - 1) * 1000 + 126) / 253;
        if (s->socPermille > 1000) s->socPermille = 1000;
        s->ok = TRUE;
    }
    if (NT_SUCCESS(SpmiRead(c, 2, 0x41A0, b, 2)))             /* voltage, 122.07 uV/LSB */
        s->mV = (ULONG)(((ULONGLONG)(b[1] << 8 | b[0]) * 122070) / 1000000);
    if (NT_SUCCESS(SpmiRead(c, 2, 0x41A2, b, 2)))             /* current, 488.281 uA/LSB, + = discharge */
        s->mA = (LONG)(((LONGLONG)(SHORT)(b[1] << 8 | b[0]) * 488281) / 1000000);
    if (NT_SUCCESS(SpmiRead(c, 2, 0x4288, b, 2)))             /* ADC_RR batt temp, degC */
        s->tempDeciC = (LONG)(SHORT)(b[1] << 8 | b[0]) * 10;
    if (NT_SUCCESS(SpmiRead(c, 2, 0x1310, b, 1)))             /* USBIN_INT_RT_STS: plugin bit4 */
        s->online = (b[0] & 0x10) != 0;
    if (NT_SUCCESS(SpmiRead(c, 2, 0x1006, b, 1)))
        s->chgState = b[0] & 7;
    WdfSpinLockRelease(c->lock);
}

/* ---------------- battery class callbacks ---------------- */

static NTSTATUS QueryTag(PVOID ctx, PULONG tag)
{
    PBATT_CTX c = (PBATT_CTX)ctx;
    if (c->tag == BATTERY_TAG_INVALID) return STATUS_NO_SUCH_DEVICE;
    *tag = c->tag;
    return STATUS_SUCCESS;
}

static NTSTATUS CopyOut(PVOID buf, ULONG bufLen, PVOID src, ULONG srcLen, PULONG ret)
{
    *ret = srcLen;
    if (bufLen < srcLen) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(buf, src, srcLen);
    return STATUS_SUCCESS;
}

static NTSTATUS QueryInformation(PVOID ctx, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level, LONG atRate,
                                 PVOID buf, ULONG bufLen, PULONG ret)
{
    PBATT_CTX c = (PBATT_CTX)ctx;
    BATT_SAMPLE s;
    UNREFERENCED_PARAMETER(atRate);
    *ret = 0;
    if (tag != c->tag) return STATUS_NO_SUCH_DEVICE;
    switch (level) {
    case BatteryInformation: {
        BATTERY_INFORMATION bi;
        RtlZeroMemory(&bi, sizeof(bi));
        bi.Capabilities = BATTERY_SYSTEM_BATTERY;
        RtlCopyMemory(bi.Chemistry, "LION", 4);
        bi.Technology = 1;                       /* rechargeable */
        bi.DesignedCapacity = DESIGN_MWH;
        bi.FullChargedCapacity = DESIGN_MWH;
        bi.DefaultAlert1 = DESIGN_MWH * 7 / 100;
        bi.DefaultAlert2 = DESIGN_MWH * 4 / 100;
        bi.CriticalBias = 0;
        bi.CycleCount = 0;
        return CopyOut(buf, bufLen, &bi, sizeof(bi), ret);
    }
    case BatteryTemperature: {
        ULONG k;                                  /* tenths of a Kelvin */
        Sample(c, &s);
        k = (ULONG)(s.tempDeciC + 2731);
        return CopyOut(buf, bufLen, &k, sizeof(k), ret);
    }
    case BatteryDeviceName:       { static const WCHAR n[] = L"pipa Battery";   return CopyOut(buf, bufLen, (PVOID)n, sizeof(n), ret); }
    case BatteryManufactureName:  { static const WCHAR n[] = L"Xiaomi";         return CopyOut(buf, bufLen, (PVOID)n, sizeof(n), ret); }
    case BatteryUniqueID:         { static const WCHAR n[] = L"PIPA-PM8150B-FG"; return CopyOut(buf, bufLen, (PVOID)n, sizeof(n), ret); }
    case BatteryGranularityInformation: {
        BATTERY_REPORTING_SCALE sc = { DESIGN_MWH, DESIGN_MWH / 1000 + 1 };
        return CopyOut(buf, bufLen, &sc, sizeof(sc), ret);
    }
    case BatteryEstimatedTime: {
        ULONG t = BATTERY_UNKNOWN_TIME;
        Sample(c, &s);
        if (s.ok && !s.online && s.mA > 20 && s.mV) {
            /* remaining mWh / discharge mW * 3600 */
            ULONGLONG mWh = (ULONGLONG)s.socPermille * DESIGN_MWH / 1000, mW = (ULONGLONG)s.mA * s.mV / 1000;
            if (mW) t = (ULONG)(mWh * 3600 / mW);
        }
        return CopyOut(buf, bufLen, &t, sizeof(t), ret);
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static NTSTATUS QueryStatus(PVOID ctx, ULONG tag, PBATTERY_STATUS bs)
{
    PBATT_CTX c = (PBATT_CTX)ctx;
    BATT_SAMPLE s;
    if (tag != c->tag) return STATUS_NO_SUCH_DEVICE;
    Sample(c, &s);
    RtlZeroMemory(bs, sizeof(*bs));
    if (!s.ok) {
        bs->Capacity = BATTERY_UNKNOWN_CAPACITY; bs->Voltage = BATTERY_UNKNOWN_VOLTAGE; bs->Rate = BATTERY_UNKNOWN_RATE;
        return STATUS_SUCCESS;
    }
    if (s.online) {
        bs->PowerState = BATTERY_POWER_ON_LINE;
        if (s.chgState <= 4 && s.mA < 0) bs->PowerState |= BATTERY_CHARGING;   /* trickle..taper and current flowing in */
    } else {
        bs->PowerState = BATTERY_DISCHARGING;
    }
    bs->Capacity = (ULONG)((ULONGLONG)s.socPermille * DESIGN_MWH / 1000);
    bs->Voltage  = s.mV ? s.mV : BATTERY_UNKNOWN_VOLTAGE;
    bs->Rate     = (LONG)(-(LONGLONG)s.mA * (LONGLONG)(s.mV ? s.mV : DESIGN_MV) / 1000);   /* mW, + = charging */
    return STATUS_SUCCESS;
}

static NTSTATUS SetStatusNotify(PVOID ctx, ULONG tag, PBATTERY_NOTIFY n)
{
    PBATT_CTX c = (PBATT_CTX)ctx;
    UNREFERENCED_PARAMETER(n);
    if (tag != c->tag) return STATUS_NO_SUCH_DEVICE;
    c->notifyArmed = TRUE;    /* the poll timer notifies battc periodically; battc re-checks the thresholds */
    return STATUS_SUCCESS;
}

static NTSTATUS DisableStatusNotify(PVOID ctx)
{
    ((PBATT_CTX)ctx)->notifyArmed = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS SetInformation(PVOID ctx, ULONG tag, BATTERY_SET_INFORMATION_LEVEL level, PVOID buf)
{
    UNREFERENCED_PARAMETER(ctx); UNREFERENCED_PARAMETER(tag); UNREFERENCED_PARAMETER(level); UNREFERENCED_PARAMETER(buf);
    return STATUS_NOT_SUPPORTED;
}

/* ---------------- WDF plumbing ---------------- */

EVT_WDF_DRIVER_DEVICE_ADD BattDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE BattPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE BattRelease;
EVT_WDF_TIMER BattTimer;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS BattIoctlPre;

VOID BattTimer(WDFTIMER t)
{
    PBATT_CTX c = BattCtx(WdfTimerGetParentObject(t));
    if (c->classHandle) BatteryClassStatusNotify(c->classHandle);
}

NTSTATUS BattIoctlPre(WDFDEVICE dev, PIRP irp)
{
    PBATT_CTX c = BattCtx(dev);
    NTSTATUS st;
    if (c->classHandle) {
        st = BatteryClassIoctl(c->classHandle, irp);
        if (st != STATUS_NOT_SUPPORTED) return st;   /* battc completed it */
    }
    IoSkipCurrentIrpStackLocation(irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(dev, irp);
}

NTSTATUS BattPrepare(WDFDEVICE dev, WDFCMRESLIST raw, WDFCMRESLIST xlat)
{
    PBATT_CTX c = BattCtx(dev);
    BATTERY_MINIPORT_INFO mi;
    BATT_SAMPLE s;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(raw); UNREFERENCED_PARAMETER(xlat);
    if (!c->core) c->core = MapPhys(SPMI_CORE, SPMI_CORE_SZ);
    if (!c->obs)  c->obs  = MapPhys(SPMI_OBS, SPMI_OBS_SZ);
    if (!c->core || !c->obs) { LOG("map failed"); return STATUS_INSUFFICIENT_RESOURCES; }
    LOG("spmi arb version %08X", RR(c->core, 0));
    Sample(c, &s);
    LOG("ok=%u soc=%lu.%lu%% %lu mV %ld mA %ld.%ld C online=%u chg=%u", s.ok, s.socPermille / 10, s.socPermille % 10,
        s.mV, s.mA, s.tempDeciC / 10, s.tempDeciC % 10, s.online, s.chgState);
    if (!s.ok) return STATUS_DEVICE_NOT_READY;
    c->tag = 1;
    if (!c->classHandle) {
        RtlZeroMemory(&mi, sizeof(mi));
        mi.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
        mi.MinorVersion = BATTERY_CLASS_MINOR_VERSION;
        mi.Context = c;
        mi.QueryTag = QueryTag;
        mi.QueryInformation = QueryInformation;
        mi.SetInformation = SetInformation;
        mi.QueryStatus = QueryStatus;
        mi.SetStatusNotify = SetStatusNotify;
        mi.DisableStatusNotify = DisableStatusNotify;
        mi.Pdo = WdfDeviceWdmGetPhysicalDevice(dev);
        mi.DeviceName = NULL;
        st = BatteryClassInitializeDevice(&mi, &c->classHandle);
        LOG("BatteryClassInitializeDevice st=%08X", st);
        if (!NT_SUCCESS(st)) { c->classHandle = NULL; return st; }
    }
    WdfTimerStart(c->timer, WDF_REL_TIMEOUT_IN_MS(2000));
    return STATUS_SUCCESS;
}

NTSTATUS BattRelease(WDFDEVICE dev, WDFCMRESLIST xlat)
{
    PBATT_CTX c = BattCtx(dev);
    UNREFERENCED_PARAMETER(xlat);
    WdfTimerStop(c->timer, TRUE);
    if (c->classHandle) { BatteryClassUnload(c->classHandle); c->classHandle = NULL; }
    if (c->core) { MmUnmapIoSpace(c->core, SPMI_CORE_SZ); c->core = NULL; }
    if (c->obs)  { MmUnmapIoSpace(c->obs, SPMI_OBS_SZ); c->obs = NULL; }
    return STATUS_SUCCESS;
}

NTSTATUS BattDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_TIMER_CONFIG tc;
    WDFDEVICE dev;
    PBATT_CTX c;
    NTSTATUS st;
    UCHAR fn = IRP_MJ_DEVICE_CONTROL;
    UNREFERENCED_PARAMETER(drv);

    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_BATTERY);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = BattPrepare;
    pnp.EvtDeviceReleaseHardware = BattRelease;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    st = WdfDeviceInitAssignWdmIrpPreprocessCallback(init, BattIoctlPre, IRP_MJ_DEVICE_CONTROL, &fn, 1);
    if (!NT_SUCCESS(st)) return st;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, BATT_CTX);
    st = WdfDeviceCreate(&init, &a, &dev);
    if (!NT_SUCCESS(st)) return st;
    c = BattCtx(dev);
    c->dev = dev;
    c->tag = BATTERY_TAG_INVALID;

    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = dev;
    st = WdfSpinLockCreate(&a, &c->lock);
    if (!NT_SUCCESS(st)) return st;

    WDF_TIMER_CONFIG_INIT_PERIODIC(&tc, BattTimer, POLL_MS);
    WDF_OBJECT_ATTRIBUTES_INIT(&a); a.ParentObject = dev;   /* DISPATCH: KMDF rejects periodic passive timers (C00000BB) */
    tc.AutomaticSerialization = FALSE;
    st = WdfTimerCreate(&tc, &a, &c->timer);
    LOG("device add st=%08X", st);
    return st;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG cfg;
    WDF_DRIVER_CONFIG_INIT(&cfg, BattDeviceAdd);
    return WdfDriverCreate(drv, path, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}

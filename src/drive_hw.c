/* What a disk says about itself: make, model, serial, bus, and SMART health.
 *
 * Windows: the volume is mapped to its physical disk, then
 *   - IOCTL_STORAGE_QUERY_PROPERTY (StorageDeviceProperty): vendor, model,
 *     firmware, serial, bus. Works without administrator rights.
 *   - IOCTL_DISK_GET_DRIVE_GEOMETRY_EX: disk size.
 *   - StorageDeviceTemperatureProperty (Windows 10+): temperature.
 *   - IOCTL_STORAGE_PREDICT_FAILURE: the drive's own failure prediction and,
 *     for ATA drives, the SMART attribute table (no administrator needed).
 *   - NVMe drives: the health log page through the protocol-specific query.
 *   - ATA drives, as a fallback: SMART READ DATA (needs administrator).
 * USB enclosures often do not pass SMART through; the note says so.
 *
 * Linux (for tests and curiosity): model, vendor, serial and size from sysfs.
 */
#include "brodalf.h"
#include "internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void hw_clear(bd_drive_hw *hw)
{
    memset(hw, 0, sizeof(*hw));
    hw->disk_bytes = -1;
    hw->temperature_c = -1;
    hw->power_on_hours = -1;
    hw->power_cycles = -1;
    hw->reallocated_sectors = -1;
    hw->pending_sectors = -1;
    hw->uncorrectable_sectors = -1;
    hw->percent_used = -1;
}

/* Copy trimming spaces (ATA strings are space-padded). */
static void copy_trim(char *out, size_t cap, const char *in, size_t n)
{
    while (n && (*in == ' ' || *in == '\t')) { in++; n--; }
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '\n' || in[n - 1] == '\0')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

static void set_health(bd_drive_hw *hw, int predicted_failure)
{
    if (!hw->smart) return;
    if (predicted_failure) snprintf(hw->health, sizeof(hw->health), "failing");
    else if (hw->reallocated_sectors > 0 || hw->pending_sectors > 0 || hw->uncorrectable_sectors > 0 ||
             hw->percent_used >= 100)
        snprintf(hw->health, sizeof(hw->health), "warning");
    else if (!hw->health[0])
        snprintf(hw->health, sizeof(hw->health), "good");
}

/* The 512-byte ATA SMART data block: a 2-byte revision, then 30 attributes
 * of 12 bytes (id, flags[2], value, worst, raw[6], reserved). */
static int parse_ata_smart(bd_drive_hw *hw, const unsigned char *data)
{
    int found = 0;
    for (int i = 0; i < 30; i++) {
        const unsigned char *a = data + 2 + i * 12;
        if (a[0] == 0) continue;
        int64_t raw = 0;
        for (int b = 5; b >= 0; b--) raw = raw << 8 | a[5 + b];
        found = 1;
        switch (a[0]) {
        case 5: hw->reallocated_sectors = raw; break;
        case 9: hw->power_on_hours = raw & 0xFFFFFFFF; break;
        case 12: hw->power_cycles = raw & 0xFFFFFFFF; break;
        case 190: if (hw->temperature_c < 0) hw->temperature_c = (int)(raw & 0xFF); break;
        case 194: hw->temperature_c = (int)(raw & 0xFF); break;
        case 197: hw->pending_sectors = raw & 0xFFFFFFFF; break;
        case 198: hw->uncorrectable_sectors = raw & 0xFFFFFFFF; break;
        case 231: if (hw->percent_used < 0 && a[3] <= 100) hw->percent_used = 100 - a[3]; break; /* SSD life left */
        default: break;
        }
    }
    return found;
}

/* The NVMe SMART / health information log page (log id 2). */
static void parse_nvme_health(bd_drive_hw *hw, const unsigned char *log)
{
    int kelvin = log[1] | log[2] << 8;
    if (kelvin > 0) hw->temperature_c = kelvin - 273;
    hw->percent_used = log[5];
    uint64_t cycles = 0, hours = 0, errors = 0;
    for (int b = 7; b >= 0; b--) {
        cycles = cycles << 8 | log[112 + b];
        hours = hours << 8 | log[128 + b];
        errors = errors << 8 | log[160 + b];
    }
    hw->power_cycles = (int64_t)cycles;
    hw->power_on_hours = (int64_t)hours;
    hw->uncorrectable_sectors = (int64_t)errors; /* media and data integrity errors */
    hw->smart = 1;
    if (log[0] != 0) snprintf(hw->health, sizeof(hw->health), "warning"); /* critical warning bits */
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

/* Declared here so older SDK and MinGW headers are not a problem. */
typedef struct {
    DWORD ProtocolType;
    DWORD DataType;
    DWORD ProtocolDataRequestValue;
    DWORD ProtocolDataRequestSubValue;
    DWORD ProtocolDataOffset;
    DWORD ProtocolDataLength;
    DWORD FixedProtocolReturnData;
    DWORD ProtocolDataRequestSubValue2;
    DWORD ProtocolDataRequestSubValue3;
    DWORD ProtocolDataRequestSubValue4;
} bd_protocol_data;

typedef struct {
    WORD Index;
    SHORT Temperature;
    SHORT OverThreshold;
    SHORT UnderThreshold;
    BOOLEAN OverThresholdChangable;
    BOOLEAN UnderThresholdChangable;
    BOOLEAN EventGenerated;
    BYTE Reserved0;
    DWORD Reserved1;
} bd_temperature_info;

typedef struct {
    DWORD Version;
    DWORD Size;
    SHORT CriticalTemperature;
    SHORT WarningTemperature;
    WORD InfoCount;
    BYTE Reserved0[2];
    DWORD Reserved1[2];
    bd_temperature_info TemperatureInfo[1];
} bd_temperature_descriptor;

#define BD_PROP_DEVICE 0
#define BD_PROP_PROTOCOL_SPECIFIC 50
#define BD_PROP_TEMPERATURE 55
#define BD_PROTOCOL_NVME 3
#define BD_NVME_LOG_PAGE 2
#define BD_NVME_HEALTH_LOG 2

static const char *bus_name(int t)
{
    static const char *names[] = {"", "SCSI", "ATAPI", "ATA", "FireWire", "SSA", "Fibre Channel", "USB", "RAID",
                                  "iSCSI", "SAS", "SATA", "SD card", "MMC", "Virtual", "Virtual disk file",
                                  "Storage Spaces", "NVMe", "SCM", "UFS"};
    return t >= 0 && t < (int)(sizeof(names) / sizeof(names[0])) ? names[t] : "";
}

static void narrow_into(const wchar_t *w, char *out, size_t cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL)) out[0] = '\0';
}

static int query_property(HANDLE h, DWORD id, const void *extra, DWORD extra_len, void *out, DWORD out_len)
{
    unsigned char in[sizeof(STORAGE_PROPERTY_QUERY) + sizeof(bd_protocol_data)];
    memset(in, 0, sizeof(in));
    STORAGE_PROPERTY_QUERY *q = (STORAGE_PROPERTY_QUERY *)in;
    q->PropertyId = (STORAGE_PROPERTY_ID)id;
    q->QueryType = PropertyStandardQuery;
    DWORD in_len = sizeof(STORAGE_PROPERTY_QUERY);
    if (extra) {
        memcpy(q->AdditionalParameters, extra, extra_len);
        in_len = (DWORD)(offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) + extra_len);
    }
    DWORD got = 0;
    return DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, in, in_len, out, out_len, &got, NULL) ? (int)got : -1;
}

static void read_identity(HANDLE h, bd_drive_hw *hw, int *bus_type)
{
    unsigned char buf[4096];
    memset(buf, 0, sizeof(buf));
    int got = query_property(h, BD_PROP_DEVICE, NULL, 0, buf, sizeof(buf));
    if (got < (int)sizeof(STORAGE_DEVICE_DESCRIPTOR)) return;
    STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)buf;
    DWORD offs[4] = {d->VendorIdOffset, d->ProductIdOffset, d->ProductRevisionOffset, d->SerialNumberOffset};
    char *outs[4] = {hw->vendor, hw->model, hw->firmware, hw->serial};
    size_t caps[4] = {sizeof(hw->vendor), sizeof(hw->model), sizeof(hw->firmware), sizeof(hw->serial)};
    for (int i = 0; i < 4; i++)
        if (offs[i] && offs[i] < (DWORD)got) copy_trim(outs[i], caps[i], (const char *)buf + offs[i], strnlen((const char *)buf + offs[i], (size_t)got - offs[i]));
    *bus_type = (int)d->BusType;
    snprintf(hw->bus, sizeof(hw->bus), "%s", bus_name(*bus_type));
    /* Some drives put the whole name in the product field. */
    if (hw->vendor[0] && strncmp(hw->model, hw->vendor, strlen(hw->vendor)) == 0) {
        char tmp[128];
        copy_trim(tmp, sizeof(tmp), hw->model + strlen(hw->vendor), strlen(hw->model + strlen(hw->vendor)));
        snprintf(hw->model, sizeof(hw->model), "%s", tmp);
    }
}

static void read_temperature(HANDLE h, bd_drive_hw *hw)
{
    unsigned char buf[512];
    memset(buf, 0, sizeof(buf));
    if (query_property(h, BD_PROP_TEMPERATURE, NULL, 0, buf, sizeof(buf)) < (int)sizeof(bd_temperature_descriptor)) return;
    bd_temperature_descriptor *t = (bd_temperature_descriptor *)buf;
    if (t->InfoCount > 0 && t->TemperatureInfo[0].Temperature > -100 && t->TemperatureInfo[0].Temperature < 150)
        hw->temperature_c = t->TemperatureInfo[0].Temperature;
}

static int read_nvme_health(HANDLE h, bd_drive_hw *hw)
{
    /* As in Microsoft's sample: one buffer holds the query on the way in
     * and STORAGE_PROTOCOL_DATA_DESCRIPTOR (Version, Size, protocol data)
     * followed by the log on the way out. */
    enum { HEADER = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters), LOG = 512 };
    size_t len = HEADER + sizeof(bd_protocol_data) + LOG;
    unsigned char *buf = calloc(1, len);
    if (!buf) return 0;
    STORAGE_PROPERTY_QUERY *q = (STORAGE_PROPERTY_QUERY *)buf;
    q->PropertyId = (STORAGE_PROPERTY_ID)BD_PROP_PROTOCOL_SPECIFIC;
    q->QueryType = PropertyStandardQuery;
    bd_protocol_data *p = (bd_protocol_data *)(buf + HEADER);
    p->ProtocolType = BD_PROTOCOL_NVME;
    p->DataType = BD_NVME_LOG_PAGE;
    p->ProtocolDataRequestValue = BD_NVME_HEALTH_LOG;
    p->ProtocolDataOffset = sizeof(bd_protocol_data);
    p->ProtocolDataLength = LOG;
    DWORD got = 0;
    int ok = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf, (DWORD)len, buf, (DWORD)len, &got, NULL)) {
        bd_protocol_data *r = (bd_protocol_data *)(buf + 2 * sizeof(DWORD));
        size_t start = 2 * sizeof(DWORD) + r->ProtocolDataOffset;
        if (r->ProtocolDataOffset >= sizeof(bd_protocol_data) && r->ProtocolDataLength >= LOG && start + LOG <= len) {
            parse_nvme_health(hw, buf + start);
            ok = 1;
        }
    }
    free(buf);
    return ok;
}

/* SMART READ DATA through the legacy SMART ioctl. Needs a read/write
 * handle, which means administrator. */
static int read_ata_smart_admin(HANDLE h, bd_drive_hw *hw, BYTE drive_no)
{
    unsigned char out[sizeof(SENDCMDOUTPARAMS) + 512];
    SENDCMDINPARAMS in;
    memset(&in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    in.cBufferSize = 512;
    in.irDriveRegs.bFeaturesReg = READ_ATTRIBUTES;
    in.irDriveRegs.bSectorCountReg = 1;
    in.irDriveRegs.bSectorNumberReg = 1;
    in.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
    in.irDriveRegs.bCylHighReg = SMART_CYL_HI;
    in.irDriveRegs.bDriveHeadReg = (BYTE)(0xA0 | ((drive_no & 1) << 4));
    in.irDriveRegs.bCommandReg = SMART_CMD;
    in.bDriveNumber = drive_no;
    DWORD got = 0;
    if (!DeviceIoControl(h, SMART_RCV_DRIVE_DATA, &in, sizeof(in) - 1, out, sizeof(out), &got, NULL)) return 0;
    SENDCMDOUTPARAMS *o = (SENDCMDOUTPARAMS *)out;
    if (!parse_ata_smart(hw, o->bBuffer)) return 0;
    hw->smart = 1;
    return 1;
}

/* The physical disk number for a volume path like "E:\". */
static int disk_number(const wchar_t *volume_path, DWORD *number)
{
    wchar_t guid[MAX_PATH];
    if (!GetVolumeNameForVolumeMountPointW(volume_path, guid, MAX_PATH)) return -1;
    size_t n = wcslen(guid);
    if (n && guid[n - 1] == L'\\') guid[n - 1] = 0; /* open the volume, not its root folder */
    HANDLE v = CreateFileW(guid, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (v == INVALID_HANDLE_VALUE) return -1;
    int rc = -1;
    STORAGE_DEVICE_NUMBER dn;
    DWORD got = 0;
    if (DeviceIoControl(v, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &dn, sizeof(dn), &got, NULL)) {
        *number = dn.DeviceNumber;
        rc = 0;
    } else {
        /* A volume spread over disks: take the first. */
        unsigned char buf[sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT)];
        if (DeviceIoControl(v, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof(buf), &got, NULL) &&
            ((VOLUME_DISK_EXTENTS *)buf)->NumberOfDiskExtents > 0) {
            *number = ((VOLUME_DISK_EXTENTS *)buf)->Extents[0].DiskNumber;
            rc = 0;
        }
    }
    CloseHandle(v);
    return rc;
}

int bd_drive_hw_read(const char *root, bd_drive_hw *hw)
{
    hw_clear(hw);
    int n = MultiByteToWideChar(CP_UTF8, 0, root, -1, NULL, 0);
    if (n <= 0 || n > MAX_PATH) return -1;
    wchar_t wroot[MAX_PATH + 2], volume[MAX_PATH + 1];
    MultiByteToWideChar(CP_UTF8, 0, root, -1, wroot, MAX_PATH);
    for (wchar_t *p = wroot; *p; p++) if (*p == L'/') *p = L'\\';
    size_t len = wcslen(wroot);
    if (len && wroot[len - 1] != L'\\') { wroot[len] = L'\\'; wroot[len + 1] = 0; }
    if (!GetVolumePathNameW(wroot, volume, MAX_PATH)) return -1;

    int known = 0;
    wchar_t vname[MAX_PATH + 1], fs[MAX_PATH + 1];
    DWORD vserial = 0;
    if (GetVolumeInformationW(volume, vname, MAX_PATH, &vserial, NULL, NULL, fs, MAX_PATH)) {
        narrow_into(vname, hw->volume_name, sizeof(hw->volume_name));
        narrow_into(fs, hw->filesystem, sizeof(hw->filesystem));
        snprintf(hw->volume_serial, sizeof(hw->volume_serial), "%04lX-%04lX", (unsigned long)(vserial >> 16),
                 (unsigned long)(vserial & 0xFFFF));
        known = 1;
    }
    if (GetDriveTypeW(volume) == DRIVE_REMOTE) {
        snprintf(hw->note, sizeof(hw->note), "a network share; the disk behind it cannot be read");
        snprintf(hw->bus, sizeof(hw->bus), "Network");
        return known ? 0 : -1;
    }
    DWORD disk;
    if (disk_number(volume, &disk) != 0) {
        snprintf(hw->note, sizeof(hw->note), "Windows did not say which disk this is");
        return known ? 0 : -1;
    }
    wchar_t dev_path[64];
    swprintf(dev_path, 64, L"\\\\.\\PhysicalDrive%lu", (unsigned long)disk);
    HANDLE h = CreateFileW(dev_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(hw->note, sizeof(hw->note), "cannot open disk %lu", (unsigned long)disk);
        return known ? 0 : -1;
    }
    int bus = 0;
    read_identity(h, hw, &bus);
    if (hw->model[0] || hw->serial[0]) known = 1;
    DISK_GEOMETRY_EX geo;
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof(geo), &got, NULL))
        hw->disk_bytes = geo.DiskSize.QuadPart;
    read_temperature(h, hw);

    int predicted = 0;
    STORAGE_PREDICT_FAILURE pf;
    memset(&pf, 0, sizeof(pf));
    if (DeviceIoControl(h, IOCTL_STORAGE_PREDICT_FAILURE, NULL, 0, &pf, sizeof(pf), &got, NULL)) {
        predicted = pf.PredictFailure != 0;
        hw->smart = 1;
        parse_ata_smart(hw, pf.VendorSpecific);
    }
    if (bus == 17) read_nvme_health(h, hw);
    CloseHandle(h);

    if (hw->power_on_hours < 0 && bus != 17) {
        /* Try the administrator-only way. */
        HANDLE rw = CreateFileW(dev_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (rw != INVALID_HANDLE_VALUE) {
            read_ata_smart_admin(rw, hw, (BYTE)disk);
            CloseHandle(rw);
        } else if (bus != 7) {
            snprintf(hw->note, sizeof(hw->note), "run BRODALF as administrator once to read this drive's SMART details");
        }
    }
    if (!hw->smart && bus == 7)
        snprintf(hw->note, sizeof(hw->note), "this USB enclosure does not pass SMART health data through");
    set_health(hw, predicted);
    return known ? 0 : -1;
}

#elif defined(__linux__)
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static int read_sys(const char *dir, const char *name, char *out, size_t cap)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    copy_trim(out, cap, buf, n);
    return out[0] ? 0 : -1;
}

int bd_drive_hw_read(const char *root, bd_drive_hw *hw)
{
    hw_clear(hw);
    struct stat st;
    if (stat(root, &st) != 0) return -1;
    char link[128], dev[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
    if (!realpath(link, dev)) return -1;
    char probe[PATH_MAX + 16];
    snprintf(probe, sizeof(probe), "%s/partition", dev);
    struct stat ps;
    if (stat(probe, &ps) == 0) { char *slash = strrchr(dev, '/'); if (slash) *slash = '\0'; }
    char device[PATH_MAX + 16], size[32];
    snprintf(device, sizeof(device), "%s/device", dev);
    read_sys(device, "vendor", hw->vendor, sizeof(hw->vendor));
    read_sys(device, "model", hw->model, sizeof(hw->model));
    read_sys(device, "rev", hw->firmware, sizeof(hw->firmware));
    if (read_sys(device, "serial", hw->serial, sizeof(hw->serial)) != 0) read_sys(dev, "serial", hw->serial, sizeof(hw->serial));
    if (read_sys(dev, "size", size, sizeof(size)) == 0) hw->disk_bytes = strtoll(size, NULL, 10) * 512;
    snprintf(hw->bus, sizeof(hw->bus), "%s", strstr(dev, "/usb") ? "USB" : strstr(dev, "/nvme") ? "NVMe" : strstr(dev, "/ata") ? "SATA" : "");
    snprintf(hw->note, sizeof(hw->note), "SMART health is read on Windows");
    return hw->model[0] || hw->serial[0] ? 0 : -1;
}

#else
int bd_drive_hw_read(const char *root, bd_drive_hw *hw)
{
    (void)root;
    hw_clear(hw);
    return -1;
}
#endif

/* Exposed for tests: parse a raw SMART block into hw. */
int bd_drive_hw_parse_ata(bd_drive_hw *hw, const unsigned char data[512], int predicted_failure)
{
    hw_clear(hw);
    hw->smart = parse_ata_smart(hw, data);
    set_health(hw, predicted_failure);
    return hw->smart ? 0 : -1;
}

int bd_drive_hw_parse_nvme(bd_drive_hw *hw, const unsigned char log[512])
{
    hw_clear(hw);
    parse_nvme_health(hw, log);
    set_health(hw, 0);
    return 0;
}

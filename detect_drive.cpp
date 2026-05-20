#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <devioctl.h>
#include <ntddstor.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

#define MAX_DRIVES 64

static void trim_str(char *s)
{
    char *start = s;
    while (*start && (unsigned char)*start <= ' ') start++;
    char *end = start + strlen(start);
    while (end > start && (unsigned char)end[-1] <= ' ') end--;
    *end = 0;
    if (start != s) memmove(s, start, end - start + 1);
}

static const char *bus_type_to_str(STORAGE_BUS_TYPE bus)
{
    switch (bus)
    {
    case BusTypeUnknown:          return "Unknown";
    case BusTypeScsi:             return "SCSI";
    case BusTypeAtapi:            return "ATAPI";
    case BusTypeAta:              return "ATA";
    case BusType1394:             return "IEEE 1394 (FireWire)";
    case BusTypeSsa:              return "SSA";
    case BusTypeFibre:            return "Fibre Channel";
    case BusTypeUsb:              return "USB";
    case BusTypeRAID:             return "RAID (Intel RST)";
    case BusTypeiScsi:            return "iSCSI";
    case BusTypeSas:              return "SAS";
    case BusTypeSata:             return "SATA";
    case BusTypeSd:               return "SD Card";
    case BusTypeMmc:              return "eMMC";
    case BusTypeVirtual:          return "Virtual";
    case BusTypeFileBackedVirtual: return "File-Backed Virtual";
    case BusTypeNvme:             return "NVMe";
    case BusTypeSCM:              return "SCM (Optane)";
    case BusTypeUfs:              return "UFS";
    case BusTypeMax:              return "Max";
    case BusTypeMaxReserved:      return "Reserved";
    default:                      return "Other";
    }
}

static const char *classify_drive(BOOL penaltyOk, BOOL penaltyIncurs,
                                  STORAGE_BUS_TYPE bus, BOOL removable,
                                  const char *model)
{
    // Check model name for clues when bus type is ambiguous
    int hasNvme = (strstr(model, "NVMe") != NULL) || (strstr(model, "nvme") != NULL);
    int hasSsd = (strstr(model, "SSD") != NULL);
    int hasHdd = (strstr(model, "ST") != NULL || strstr(model, "WD") != NULL ||
                  strstr(model, "HDD") != NULL || strstr(model, "MQ") != NULL);

    if (bus == BusTypeNvme)
        return "NVMe SSD";
    if (bus == BusTypeMmc)
        return "eMMC";
    if (bus == BusTypeUfs)
        return "UFS Flash";
    if (bus == BusTypeSd)
        return "SD Card";
    if (bus == BusTypeSCM)
        return "Intel Optane (SCM)";
    if (bus == BusTypeSata)
    {
        if (penaltyOk)
            return penaltyIncurs ? "HDD (SATA)" : "SSD (SATA)";
        return "SATA Drive";
    }
    if (bus == BusTypeAta || bus == BusTypeAtapi)
    {
        if (penaltyOk)
            return penaltyIncurs ? "HDD (ATA/ATAPI)" : "SSD (ATA/ATAPI)";
        return "ATA/ATAPI Drive";
    }
    if (bus == BusTypeSas)
    {
        if (penaltyOk)
            return penaltyIncurs ? "HDD (SAS)" : "SSD (SAS)";
        return "SAS Drive";
    }
    if (bus == BusTypeScsi)
    {
        if (penaltyOk)
            return penaltyIncurs ? "HDD (SCSI)" : "SSD (SCSI)";
        return "SCSI Drive";
    }

    // RAID (Intel RST/VMD) - check model name to determine actual type
    if (bus == BusTypeRAID)
    {
        if (hasNvme)
            return "NVMe SSD (via Intel RST)";
        if (penaltyOk)
            return penaltyIncurs ? "HDD (via Intel RST)" : "SSD (via Intel RST)";
        if (hasSsd)
            return "SSD (via Intel RST)";
        if (hasHdd)
            return "HDD (via Intel RST)";
        return "Drive (via Intel RST)";
    }

    if (bus == BusTypeUsb)
    {
        if (removable)
            return "USB Flash Drive";
        return "USB External Drive";
    }
    if (removable)
        return "Removable Media";

    if (bus == BusTypeVirtual || bus == BusTypeFileBackedVirtual)
        return "Virtual Disk";
    if (bus == BusType1394)
        return "FireWire Drive";
    if (bus == BusTypeFibre)
        return "Fibre Channel Disk";
    if (bus == BusTypeiScsi)
        return "iSCSI Drive";

    if (penaltyOk)
        return penaltyIncurs ? "HDD" : "SSD";
    return "Unknown";
}

static void format_size(LARGE_INTEGER size, char *buf, int bufLen)
{
    double bytes = (double)size.QuadPart;
    if (bytes >= 1099511627776.0)
        snprintf(buf, bufLen, "%.2f TB", bytes / 1099511627776.0);
    else if (bytes >= 1073741824.0)
        snprintf(buf, bufLen, "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1048576.0)
        snprintf(buf, bufLen, "%.2f MB", bytes / 1048576.0);
    else
        snprintf(buf, bufLen, "%.0f B", bytes);
}

typedef struct {
    int diskNumber;
    STORAGE_BUS_TYPE busType;
    BOOL removable;
    BOOL penaltyIncurs;
    BOOL penaltyOk;
    char model[128];
    char serial[41];
    char revision[9];
    LARGE_INTEGER size;
} DriveInfo;

static BOOL query_via_volume(char letter, int diskNum, DriveInfo *info)
{
    char volPath[] = "\\\\.\\X:";
    volPath[4] = letter;

    HANDLE hVol = CreateFileA(volPath, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hVol == INVALID_HANDLE_VALUE)
    {
        hVol = CreateFileA(volPath, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
        if (hVol == INVALID_HANDLE_VALUE)
            return FALSE;
    }

    STORAGE_PROPERTY_QUERY query;
    DWORD ret = 0;

    ZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty;
    ZeroMemory(&penalty, sizeof(penalty));
    BOOL penaltyOk = DeviceIoControl(hVol, IOCTL_STORAGE_QUERY_PROPERTY,
                                     &query, sizeof(query),
                                     &penalty, sizeof(penalty), &ret, NULL);

    ZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE descBuf[sizeof(STORAGE_DEVICE_DESCRIPTOR) + 1024];
    ZeroMemory(descBuf, sizeof(descBuf));
    BOOL descOk = DeviceIoControl(hVol, IOCTL_STORAGE_QUERY_PROPERTY,
                                  &query, sizeof(query),
                                  descBuf, sizeof(descBuf), &ret, NULL);

    DISK_GEOMETRY_EX geo;
    ZeroMemory(&geo, sizeof(geo));
    BOOL sizeOk = DeviceIoControl(hVol, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                  NULL, 0, &geo, sizeof(geo), &ret, NULL);

    CloseHandle(hVol);

    info->diskNumber = diskNum;
    info->penaltyOk = penaltyOk;
    info->penaltyIncurs = penaltyOk ? penalty.IncursSeekPenalty : FALSE;
    info->busType = BusTypeUnknown;
    info->removable = FALSE;
    info->size.QuadPart = 0;
    info->model[0] = 0;
    info->serial[0] = 0;
    info->revision[0] = 0;

    if (descOk)
    {
        STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)descBuf;
        info->busType = desc->BusType;
        info->removable = desc->RemovableMedia;

        char vendor[41] = {0}, product[41] = {0};
        if (desc->VendorIdOffset)
            strncpy(vendor, (char*)(descBuf + desc->VendorIdOffset), 40);
        if (desc->ProductIdOffset)
            strncpy(product, (char*)(descBuf + desc->ProductIdOffset), 40);
        if (desc->SerialNumberOffset)
            strncpy(info->serial, (char*)(descBuf + desc->SerialNumberOffset), 40);
        if (desc->ProductRevisionOffset)
            strncpy(info->revision, (char*)(descBuf + desc->ProductRevisionOffset), 8);

        trim_str(vendor);
        trim_str(product);

        if (vendor[0] && product[0])
            snprintf(info->model, sizeof(info->model), "%s %s", vendor, product);
        else if (product[0])
            snprintf(info->model, sizeof(info->model), "%s", product);
        else
            snprintf(info->model, sizeof(info->model), "PhysicalDrive%d", diskNum);

        trim_str(info->serial);
    }

    if (sizeOk)
        info->size = geo.DiskSize;

    return TRUE;
}

int main(void)
{
    DriveInfo drives[MAX_DRIVES];
    BOOL seenDrive[MAX_DRIVES] = {FALSE};
    int foundDrives = 0;
    DWORD logicalDrives = GetLogicalDrives();

    for (char letter = 'A'; letter <= 'Z'; letter++)
    {
        if (!(logicalDrives & (1 << (letter - 'A'))))
            continue;

        char volPath[] = "\\\\.\\X:";
        volPath[4] = letter;

        HANDLE hVol = CreateFileA(volPath, 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (hVol == INVALID_HANDLE_VALUE)
            continue;

        VOLUME_DISK_EXTENTS extents;
        DWORD bytesRet = 0;
        BOOL ok = DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                  NULL, 0, &extents, sizeof(extents), &bytesRet, NULL);
        CloseHandle(hVol);

        if (!ok || extents.NumberOfDiskExtents < 1)
            continue;

        DWORD diskNum = extents.Extents[0].DiskNumber;
        if (diskNum >= MAX_DRIVES || seenDrive[diskNum])
            continue;

        seenDrive[diskNum] = TRUE;

        DriveInfo info;
        ZeroMemory(&info, sizeof(info));
        if (query_via_volume(letter, (int)diskNum, &info))
            drives[foundDrives++] = info;
    }

    printf("\n");
    printf("  Storage Devices\n");
    printf("  ---------------\n\n");

    if (foundDrives == 0)
    {
        printf("  No storage devices detected.\n");
    }
    else
    {
        printf("  %-5s %-24s %-10s %-24s %-18s %s\n",
               "PD#", "Model", "Size", "Bus Type", "Type", "Serial");
        printf("  %-5s %-24s %-10s %-24s %-18s %s\n",
               "----", "------------------------", "----------", "------------------------", "------------------", "--------------------");

        for (int i = 0; i < foundDrives; i++)
        {
            DriveInfo *d = &drives[i];
            char sizeStr[32];
            format_size(d->size, sizeStr, sizeof(sizeStr));
            const char *type = classify_drive(d->penaltyOk, d->penaltyIncurs, d->busType, d->removable, d->model);
            const char *busStr = bus_type_to_str(d->busType);

            printf("  %-5d %-24s %-10s %-24s %-18s %s\n",
                   d->diskNumber, d->model, sizeStr, busStr, type,
                   d->serial[0] ? d->serial : "-");
        }

        printf("  %-5s %-24s %-10s %-24s %-18s %s\n",
               "----", "------------------------", "----------", "------------------------", "------------------", "--------------------");
        printf("\n  Total: %d physical drive(s)\n", foundDrives);
    }

    printf("\n  Volume Map\n");
    printf("  ----------\n\n");

    printf("  %-4s %-5s %-24s %-9s %-10s %-18s %s\n",
           "Vol", "PD#", "Label", "Size", "FS", "Type", "Free");
    printf("  %-4s %-5s %-24s %-9s %-10s %-18s %s\n",
           "---", "----", "------------------------", "---------", "----------", "------------------", "------------------");

    for (char letter = 'A'; letter <= 'Z'; letter++)
    {
        if (!(logicalDrives & (1 << (letter - 'A'))))
            continue;

        char volPath[] = "\\\\.\\X:";
        volPath[4] = letter;

        HANDLE hVol = CreateFileA(volPath, 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (hVol == INVALID_HANDLE_VALUE)
            continue;

        VOLUME_DISK_EXTENTS extents;
        DWORD bytesRet = 0;
        BOOL ok = DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                  NULL, 0, &extents, sizeof(extents), &bytesRet, NULL);
        CloseHandle(hVol);
        if (!ok || extents.NumberOfDiskExtents < 1)
            continue;

        char fsName[256] = {0}, volName[256] = {0};
        char rootPath[8];
        snprintf(rootPath, sizeof(rootPath), "%c:\\", letter);

        DWORD fsFlags = 0;
        GetVolumeInformationA(rootPath, volName, sizeof(volName),
                              NULL, NULL, &fsFlags, fsName, sizeof(fsName));

        ULARGE_INTEGER freeBytes, totalBytes, totalFree;
        BOOL spaceOk = GetDiskFreeSpaceExA(rootPath, &freeBytes, &totalBytes, &totalFree);

        DWORD pid = extents.Extents[0].DiskNumber;
        const char *driveType = "Unknown";
        for (int di = 0; di < foundDrives; di++)
        {
            if ((DWORD)drives[di].diskNumber == pid)
            {
                const char *ct = classify_drive(drives[di].penaltyOk, drives[di].penaltyIncurs,
                                                 drives[di].busType, drives[di].removable,
                                                 drives[di].model);
                if (strstr(ct, "NVMe"))
                    driveType = "NVMe SSD";
                else if (strstr(ct, "HDD"))
                    driveType = "HDD";
                else if (strstr(ct, "SSD"))
                    driveType = "SSD";
                else if (strstr(ct, "USB"))
                    driveType = "USB";
                else if (strstr(ct, "eMMC"))
                    driveType = "eMMC";
                else if (strstr(ct, "Optane"))
                    driveType = "Optane";
                else if (strstr(ct, "SATA"))
                    driveType = "SATA";
                else if (strstr(ct, "SAS"))
                    driveType = "SAS";
                else if (strstr(ct, "SCSI"))
                    driveType = "SCSI";
                else if (strstr(ct, "UFS"))
                    driveType = "UFS Flash";
                else if (strstr(ct, "FireWire"))
                    driveType = "FireWire";
                else if (strstr(ct, "Fibre"))
                    driveType = "Fibre Ch.";
                else if (strstr(ct, "iSCSI"))
                    driveType = "iSCSI";
                else if (strstr(ct, "Virtual"))
                    driveType = "Virtual";
                else if (strstr(ct, "SD Card"))
                    driveType = "SD Card";
                else
                    driveType = ct;
                break;
            }
        }

        trim_str(volName);
        const char *label = volName[0] ? volName : "-";

        LARGE_INTEGER li;
        li.QuadPart = totalBytes.QuadPart;
        char sizeStr[32] = "-";
        if (spaceOk) format_size(li, sizeStr, sizeof(sizeStr));

        li.QuadPart = freeBytes.QuadPart;
        char freeStr[32] = "-";
        if (spaceOk) format_size(li, freeStr, sizeof(freeStr));

        printf("  %c:   %-5d %-24s %-9s %-10s %-18s %s\n",
               letter, pid, label, sizeStr,
               fsName[0] ? fsName : "-",
               driveType, freeStr);
    }

    printf("\n");
    printf("  Windows Version\n");
    printf("  ---------------\n\n");

    HKEY hKey;
    char regVal[256];
    DWORD regSize, regType;

    // Proper version detection via RtlGetVersion
    typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleA("ntdll");
    RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
    DWORD major = 0, minor = 0, build = 0;

    if (RtlGetVersion)
    {
        RTL_OSVERSIONINFOW osvi = {0};
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        if (RtlGetVersion(&osvi) == 0)
        {
            major = osvi.dwMajorVersion;
            minor = osvi.dwMinorVersion;
            build = osvi.dwBuildNumber;
        }
    }

    const char *osName = "Windows";
    if (major == 10)
    {
        if (build >= 22000)
            osName = "Windows 11";
        else
            osName = "Windows 10";
    }
    else if (major == 6 && minor == 3)
        osName = "Windows 8.1";
    else if (major == 6 && minor == 2)
        osName = "Windows 8";
    else if (major == 6 && minor == 1)
        osName = "Windows 7";

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        char edition[64] = "Unknown";
        regSize = sizeof(edition);
        if (RegQueryValueExA(hKey, "EditionID", NULL, &regType, (LPBYTE)edition, &regSize) != ERROR_SUCCESS)
            strcpy(edition, "Unknown");

        char displayVer[64] = "";
        regSize = sizeof(displayVer);
        RegQueryValueExA(hKey, "DisplayVersion", NULL, &regType, (LPBYTE)displayVer, &regSize);

        DWORD ubr = 0;
        regSize = sizeof(ubr);
        RegQueryValueExA(hKey, "UBR", NULL, &regType, (LPBYTE)&ubr, &regSize);

        DWORD installDate = 0;
        regSize = sizeof(installDate);
        RegQueryValueExA(hKey, "InstallDate", NULL, &regType, (LPBYTE)&installDate, &regSize);

        RegCloseKey(hKey);

        printf("  OS:      %s %s", osName, edition);
        if (displayVer[0]) printf(" (%s)", displayVer);
        printf("\n");
        printf("  Build:   %lu.%lu\n", build, ubr);

        if (installDate)
        {
            time_t t = (time_t)installDate;
            struct tm *tmInfo = localtime(&t);
            char dateStr[64];
            strftime(dateStr, sizeof(dateStr), "%Y-%m-%d %H:%M:%S", tmInfo);
            printf("  Installed: %s\n", dateStr);
        }
    }
    else
    {
        printf("  OS:      %s (build %lu)\n", osName, build);
    }

    SYSTEM_INFO sysInfo;
    GetNativeSystemInfo(&sysInfo);
    const char *arch = "Unknown";
    switch (sysInfo.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64: arch = "x86-64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = "ARM64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
    case PROCESSOR_ARCHITECTURE_IA64: arch = "IA-64"; break;
    }
    printf("  Arch:    %s\n", arch);

    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(memInfo);
    if (GlobalMemoryStatusEx(&memInfo))
    {
        char memStr[32];
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)memInfo.ullTotalPhys;
        format_size(li, memStr, sizeof(memStr));
        printf("  RAM:     %s\n", memStr);
    }

    printf("\n");
    printf("  Windows Installation\n");
    printf("  --------------------\n\n");

    char winDir[MAX_PATH];
    if (GetWindowsDirectoryA(winDir, sizeof(winDir)))
    {
        char winLetter = winDir[0];
        printf("  Drive: %c:\\\n", winLetter);

        char volPath[] = "\\\\.\\X:";
        volPath[4] = winLetter;
        HANDLE hVol = CreateFileA(volPath, 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (hVol != INVALID_HANDLE_VALUE)
        {
            VOLUME_DISK_EXTENTS extents;
            DWORD bytesRet = 0;
            BOOL ok = DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                      NULL, 0, &extents, sizeof(extents), &bytesRet, NULL);
            CloseHandle(hVol);

            if (ok && extents.NumberOfDiskExtents >= 1)
            {
                DWORD winDisk = extents.Extents[0].DiskNumber;
                int found = 0;
                for (int i = 0; i < foundDrives; i++)
                {
                    if ((DWORD)drives[i].diskNumber == winDisk)
                    {
                        const char *ct = classify_drive(drives[i].penaltyOk, drives[i].penaltyIncurs,
                                                         drives[i].busType, drives[i].removable,
                                                         drives[i].model);
                        printf("  Media: %s\n", ct);
                        printf("  Model: %s\n", drives[i].model);
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    printf("  Media: Unknown\n");
            }
            else
            {
                printf("  Media: Unknown\n");
            }
        }
        else
        {
            printf("  Media: Unknown\n");
        }
    }
    else
    {
        printf("  Could not determine Windows drive.\n");
    }

    printf("\n");
    printf("  Programmed by matrixvanish (2026), All rights reserved.\n");
    printf("\n");
    return 0;
}

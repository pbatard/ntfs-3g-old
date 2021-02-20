/* driver.h -ntfs-3g UEFI filesystem driver */
/*
 *  Copyright © 2014-2021 Pete Batard <pete@akeo.ie>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#if defined(__MAKEWITH_GNUEFI)
#include <efi.h>
#include <efilib.h>
#include <efidebug.h>
#else
#include <Base.h>
#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>

#include <Protocol/UnicodeCollation.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/DevicePathFromText.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/DebugPort.h>
#include <Protocol/DebugSupport.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DiskIo2.h>
#include <Protocol/ComponentName.h>
#include <Protocol/ComponentName2.h>

#include <Guid/FileSystemInfo.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>
#include <Guid/ShellVariableGuid.h>
#endif

#pragma once

#if !defined(_MSC_VER)
#if !defined(__GNUC__) || (__GNUC__ < 4) || (__GNUC__ == 4 && __GNUC_MINOR__ < 7)
#error gcc 4.7 or later is required for the compilation of this driver.
#endif

/* Having GNU_EFI_USE_MS_ABI should avoid the need for that ugly uefi_call_wrapper */
#if defined(_GNU_EFI) && !defined(GNU_EFI_USE_MS_ABI)
#error gnu-efi, with option GNU_EFI_USE_MS_ABI, is required for the compilation of this driver.
#endif
#endif

/* Sort out the platform specifics */
#if defined(_M_ARM64) || defined(__aarch64__) || defined (_M_X64) || defined(__x86_64__)
#define PERCENT_P               L"%llx"
#else
#define PERCENT_P               L"%x"
#endif

/* Define to the full name and version of this package. */
#ifndef PACKAGE_STRING
#define PACKAGE_STRING          "ntfs-3g 2021.02.20"
#endif

#ifndef ARRAYSIZE
#define ARRAYSIZE(A)            (sizeof(A)/sizeof((A)[0]))
#endif

#ifndef MIN
#define MIN(x,y)                ((x)<(y)?(x):(y))
#endif

#ifndef PATH_MAX
#define PATH_MAX                4096
#endif

#ifndef PATH_CHAR
#define PATH_CHAR               L'/'
#endif

#define _WIDEN(s)               L ## s
#define WIDEN(s)                _WIDEN(s)

#define MINIMUM_INFO_LENGTH     (sizeof(EFI_FILE_INFO) + PATH_MAX * sizeof(CHAR16))
#define MINIMUM_FS_INFO_LENGTH  (sizeof(EFI_FILE_SYSTEM_INFO) + PATH_MAX * sizeof(CHAR16))
#define IS_ROOT(File)           (File == File->FileSystem->RootFile)
#define IS_PATH_DELIMITER(x)    (x == PATH_CHAR || x == L'\\')

/* Logging */
#define FS_LOGLEVEL_NONE        0
#define FS_LOGLEVEL_ERROR       1
#define FS_LOGLEVEL_WARNING     2
#define FS_LOGLEVEL_INFO        3
#define FS_LOGLEVEL_DEBUG       4
#define FS_LOGLEVEL_EXTRA       5

typedef UINTN(EFIAPI* Print_t)        (IN CONST CHAR16* fmt, ...);
extern Print_t PrintError;
extern Print_t PrintWarning;
extern Print_t PrintInfo;
extern Print_t PrintDebug;
extern Print_t PrintExtra;

#define FS_ASSERT(a)  if(!(a)) do { Print(L"*** ASSERT FAILED: %a(%d): %a ***\n", __FILE__, __LINE__, #a); while(1); } while(0)

/**
 * Print an error message along with a human readable EFI status code
 *
 * @v Status		EFI status code
 * @v Format		A non '\n' terminated error message string
 * @v ...			Any extra parameters
 */
#define PrintStatusError(Status, Format, ...) \
	if (LogLevel >= FS_LOGLEVEL_ERROR) { \
		Print(Format, ##__VA_ARGS__); PrintStatus(Status); }

/* Forward declaration */
struct _EFI_FS;

/* A file instance */
typedef struct _EFI_NTFS_FILE {
	EFI_FILE                        EfiFile;
	/* TODO: Might set flags like hidden, archive, ro and stuff */
	BOOLEAN                         IsDir;
	INTN                            DirIndex;
	INT64                           Offset;
	CHAR16                          *Path;
	CHAR16                          *Basename;
	INTN                            RefCount;
	struct _EFI_FS                  *FileSystem;
	VOID                            *NtfsInode;
} EFI_NTFS_FILE;

/* A file system instance */
typedef struct _EFI_FS {
	LIST_ENTRY                      *Flink;
	LIST_ENTRY                      *Blink;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL FileIoInterface;
	EFI_BLOCK_IO_PROTOCOL           *BlockIo;
	EFI_BLOCK_IO2_PROTOCOL          *BlockIo2;
	EFI_BLOCK_IO2_TOKEN             BlockIo2Token;
	EFI_DISK_IO_PROTOCOL            *DiskIo;
	EFI_DISK_IO2_PROTOCOL           *DiskIo2;
	EFI_DISK_IO2_TOKEN              DiskIo2Token;
	CHAR16                          *DevicePathString;
	EFI_NTFS_FILE                   *RootFile;
	VOID                            *NtfsVolume;
	CHAR16                          *NtfsVolumeLabel;
	INT64                           Offset;
} EFI_FS;

extern UINTN LogLevel;
extern LIST_ENTRY FsListHead;

extern VOID SetLogging(VOID);
extern VOID PrintStatus(EFI_STATUS Status);
extern INTN CompareDevicePaths(CONST EFI_DEVICE_PATH* dp1, CONST EFI_DEVICE_PATH* dp2);
extern VOID CleanPath(CHAR16* Path);
extern CHAR16* DevicePathToString(CONST EFI_DEVICE_PATH* DevicePath);
extern EFI_STATUS FSInstall(EFI_FS* This, EFI_HANDLE ControllerHandle);
extern VOID FSUninstall(EFI_FS* This, EFI_HANDLE ControllerHandle);
extern EFI_STATUS EFIAPI FileOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* This,
	EFI_FILE_HANDLE* Root);

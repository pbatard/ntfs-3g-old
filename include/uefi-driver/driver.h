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

/* Driver version */
#define NTFS_DRIVER_VERSION_MAJOR 0
#define NTFS_DRIVER_VERSION_MINOR 2

#ifndef ARRAYSIZE
#define ARRAYSIZE(A)            (sizeof(A)/sizeof((A)[0]))
#endif

#define _STRINGIFY(s)           #s
#define STRINGIFY(s)            _STRINGIFY(s)

#define _WIDEN(s)               L ## s
#define WIDEN(s)                _WIDEN(s)

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

extern UINTN LogLevel;

extern VOID SetLogging(VOID);
extern VOID PrintStatus(EFI_STATUS Status);

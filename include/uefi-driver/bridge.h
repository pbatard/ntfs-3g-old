/* bridge.h - libntfs-3g interface for UEFI */
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

#pragma once

#include <stdint.h>

#if defined(__MAKEWITH_GNUEFI)
#include <efi.h>
#else
#include <Base.h>
#include <Uefi.h>
#endif

/* Used with NtfsGetEfiTime */
#define TIME_CREATED        0
#define TIME_ACCESSED       1
#define TIME_MODIFIED       2

/* Similar to the MREF() macro from libntfs-3g */
#define GetInodeNumber(x)   ((UINT64)((x) & 0XFFFFFFFFFFFFULL))

/*
 * The s64 and u64 types used for ntfs_filldir_t suprisingly do not
 * always translate to the INT64 and UINT64 types used the EDK2, as
 * EDK2/gcc may use a long (which is 64-bit on Linux) rather than a
 * long long (also 64-bit) and the compiler will complain that the
 * underlying types are different (!).
 */
typedef int64_t             INT64_T;
typedef uint64_t            UINT64_T;

/* This typedef mirrors the ntfs_filldir_t one in ntfs-3g's dir.h */
typedef INT32(*NTFS_DIRHOOK)(VOID* HookData, CONST CHAR16* Name,
	CONST INT32 NameLen, CONST INT32 NameType, CONST INT64_T Pos,
	CONST UINT64_T MRef, CONST UINT32 DtType);

VOID NtfsGetEfiTime(EFI_NTFS_FILE* File, EFI_TIME* Time, INTN Type);
VOID NtfsSetLogger(UINTN LogLevel);
EFI_STATUS NtfsMount(EFI_FS* FileSystem);
EFI_STATUS NtfsUnmount(EFI_FS* FileSystem);
EFI_STATUS NtfsCreateFile(EFI_NTFS_FILE** File, EFI_FS* FileSystem);
VOID NtfsDestroyFile(EFI_NTFS_FILE* File);
EFI_STATUS NtfsOpen(EFI_NTFS_FILE* File);
VOID NtfsClose(EFI_NTFS_FILE* File);
EFI_STATUS NtfsRead(EFI_NTFS_FILE* File, VOID* Data, UINTN* Len);
UINT64 NtfsGetFileSize(EFI_NTFS_FILE* File);
UINT64 NtfsGetFileOffset(EFI_NTFS_FILE* File);
VOID NtfsSetFileOffset(EFI_NTFS_FILE* File, UINT64 Offset);
EFI_STATUS NtfsSetInfo(EFI_FILE_INFO* Info, VOID* NtfsVolume, CONST UINT64 MRef, BOOLEAN IsDir);
EFI_STATUS NtfsReadDir(EFI_NTFS_FILE* File, NTFS_DIRHOOK Hook, VOID* HookData);
UINT64 NtfsGetVolumeFreeSpace(VOID* NtfsVolume);

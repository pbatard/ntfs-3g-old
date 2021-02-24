/* driver.h -ntfs-3g UEFI filesystem driver */
/*
 *  Copyright © 2021 Pete Batard <pete@akeo.ie>
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

#ifdef __MAKEWITH_GNUEFI

#include <efi.h>
#include <efilib.h>
#include <efidebug.h>

#else /* __MAKEWITH_GNUEFI */

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

#endif /* __MAKEWITH_GNUEFI */

/* This forces all NTFS volumes to be opened read-only */
#define FORCE_READONLY

#define NTFS_MUTEX_GUID { 0xf4ed18ca, 0xcdfb, 0x40ca, { 0x97, 0xec, 0x32, 0x2a, 0x8b, 0x01, 0x4e, 0x5f } }

/* Define to the full name and version of this package. */
#ifndef PACKAGE_STRING
#define PACKAGE_STRING              "ntfs-3g 2021.02.20"
#endif

#define MINIMUM_INFO_LENGTH         (sizeof(EFI_FILE_INFO) + PATH_MAX * sizeof(CHAR16))
#define MINIMUM_FS_INFO_LENGTH      (sizeof(EFI_FILE_SYSTEM_INFO) + PATH_MAX * sizeof(CHAR16))

/* A file instance */
typedef struct _EFI_NTFS_FILE {
	EFI_FILE                         EfiFile;
	BOOLEAN                          IsDir;
	BOOLEAN                          IsRoot;
	INTN                             DirIndex;
	INT64                            Offset;
	CHAR16                          *Path;
	CHAR16                          *Basename;
	INTN                             RefCount;
	struct _EFI_FS                  *FileSystem;
	VOID                            *NtfsInode;
} EFI_NTFS_FILE;

/* A file system instance */
typedef struct _EFI_FS {
	LIST_ENTRY                      *ForwardLink;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  FileIoInterface;
	EFI_BLOCK_IO_PROTOCOL           *BlockIo;
	EFI_BLOCK_IO2_PROTOCOL          *BlockIo2;
	EFI_DISK_IO_PROTOCOL            *DiskIo;
	EFI_DISK_IO2_PROTOCOL           *DiskIo2;
	EFI_DISK_IO2_TOKEN               DiskIo2Token;
	CHAR16                          *DevicePathString;
	VOID                            *NtfsVolume;
	CHAR16                          *NtfsVolumeLabel;
	INT64                            Offset;
	INTN                             TotalRefCount;
} EFI_FS;

/* The top of our file system instances list */
extern LIST_ENTRY FsListHead;

extern EFI_STATUS FSInstall(EFI_FS* This, EFI_HANDLE ControllerHandle);
extern VOID FSUninstall(EFI_FS* This, EFI_HANDLE ControllerHandle);
extern EFI_STATUS EFIAPI FileOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* This,
	EFI_FILE_HANDLE* Root);

/*
 * All the public file system operations that are defined in file.c
 */
EFI_STATUS EFIAPI FileOpen(EFI_FILE_HANDLE This, EFI_FILE_HANDLE* New,
	CHAR16* Name, UINT64 Mode, UINT64 Attributes);
EFI_STATUS EFIAPI FileOpenEx(EFI_FILE_HANDLE This, EFI_FILE_HANDLE* New,
	CHAR16* Name, UINT64 Mode, UINT64 Attributes, EFI_FILE_IO_TOKEN* Token);
EFI_STATUS EFIAPI FileClose(EFI_FILE_HANDLE This);
EFI_STATUS EFIAPI FileDelete(EFI_FILE_HANDLE This);
EFI_STATUS EFIAPI FileRead(EFI_FILE_HANDLE This, UINTN* Len, VOID* Data);
EFI_STATUS EFIAPI FileReadEx(IN EFI_FILE_PROTOCOL* This, IN OUT EFI_FILE_IO_TOKEN* Token);
EFI_STATUS EFIAPI FileWrite(EFI_FILE_HANDLE This, UINTN* Len, VOID* Data);
EFI_STATUS EFIAPI FileWriteEx(IN EFI_FILE_PROTOCOL* This, EFI_FILE_IO_TOKEN* Token);
EFI_STATUS EFIAPI FileSetPosition(EFI_FILE_HANDLE This, UINT64 Position);
EFI_STATUS EFIAPI FileGetPosition(EFI_FILE_HANDLE This, UINT64* Position);
EFI_STATUS EFIAPI FileGetInfo(EFI_FILE_HANDLE This, EFI_GUID* Type, UINTN* Len, VOID* Data);
EFI_STATUS EFIAPI FileSetInfo(EFI_FILE_HANDLE This, EFI_GUID* Type, UINTN Len, VOID* Data);
EFI_STATUS EFIAPI FileFlush(EFI_FILE_HANDLE This);
EFI_STATUS EFIAPI FileFlushEx(EFI_FILE_HANDLE This, EFI_FILE_IO_TOKEN* Token);

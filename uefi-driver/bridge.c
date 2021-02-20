/* bridge.c - libntfs-3g interface for UEFI */
/*
 *  Copyright © 2021 Pete Batard <pete@akeo.ie>
 *
 *  Parts taken from lowntfs-3g.c:
 *  Copyright © 2005-2007 Yura Pakhuchiy
 *  Copyright © 2005 Yuval Fledel
 *  Copyright © 2006-2009 Szabolcs Szakacsits
 *  Copyright © 2007-2021 Jean-Pierre Andre
 *  Copyright © 2009 Erik Larsson
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

#include "compat.h"
#include "volume.h"
#include "unistr.h"
#include "logging.h"
#include "dir.h"

#include "driver.h"
#include "bridge.h"

#define PrintErrno()    PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno))
#define IS_DIR(ni)      (((ntfs_inode*)(ni))->mrec->flags & MFT_RECORD_IS_DIRECTORY)

/* Compute an EFI_TIME representation of an ntfs_time field */
VOID
NtfsGetEfiTime(EFI_NTFS_FILE* File, EFI_TIME* Time, INTN Type)
{
	/* TODO: Actually populate the time */
	ZeroMem(Time, sizeof(EFI_TIME));
}

VOID
NtfsSetLogger(UINTN LogLevel)
{
	ntfs_log_set_handler(ntfs_log_handler_uefi);
	/* TODO: Only enable relevant log levels */
	ntfs_log_set_levels(0xFFFF);
}

EFI_STATUS
NtfsMount(EFI_FS* FileSystem)
{
	ntfs_volume* vol = NULL;
	CHAR8* DevName = NULL;

	/* ntfs_ucstombs() can be used to convert to UTF-8 */
	ntfs_ucstombs(FileSystem->DevicePathString,
		StrLen(FileSystem->DevicePathString), &DevName, 0);
	if (DevName == NULL)
		return EFI_OUT_OF_RESOURCES;

	/* Insert this filesystem in our list so that ntfs_mount() can locate it */
	InsertTailList(&FsListHead, (LIST_ENTRY*)FileSystem);

	ntfs_log_set_handler(ntfs_log_handler_uefi);

	vol = ntfs_mount(DevName, 0);
	FreePool(DevName);
	if (vol == NULL) {
		RemoveEntryList((LIST_ENTRY*)FileSystem);
		return EFI_NOT_FOUND;
	}
	FileSystem->NtfsVolume = vol;
	ntfs_mbstoucs(vol->vol_name, &FileSystem->NtfsVolumeLabel);

	return EFI_SUCCESS;
}

EFI_STATUS
NtfsUnmount(EFI_FS* FileSystem)
{
	ntfs_umount(FileSystem->NtfsVolume, FALSE);
	free(FileSystem->NtfsVolumeLabel);
	FileSystem->NtfsVolumeLabel = NULL;

	RemoveEntryList((LIST_ENTRY*)FileSystem);

	return EFI_SUCCESS;
}

EFI_STATUS
NtfsCreateFile(EFI_NTFS_FILE** File, EFI_FS* FileSystem)
{
	EFI_NTFS_FILE* NewFile;

	NewFile = AllocateZeroPool(sizeof(*NewFile));
	if (NewFile == NULL)
		return EFI_OUT_OF_RESOURCES;

	/* Initialize the attributes */
	NewFile->FileSystem = FileSystem;

	/* TODO: Do we actually need to bother with root at all? */

	/* See if we are initializing the root file */
	if (FileSystem->RootFile == NULL) {
		FileSystem->RootFile = NewFile;
		FileSystem->RootFile->NtfsInode = ntfs_inode_open(FileSystem->NtfsVolume, FILE_root);
		if (FileSystem->RootFile->NtfsInode == NULL) {
			PrintError(L"Failed to initialize ROOT!\n");
			FreePool(NewFile);
			FileSystem->RootFile = NULL;
			return EFI_NOT_FOUND;
		}
	} else {
		CopyMem(&NewFile->EfiFile, &FileSystem->RootFile->EfiFile, sizeof(EFI_FILE));
	}
	*File = NewFile;
	return EFI_SUCCESS;
}

VOID
NtfsDestroyFile(EFI_NTFS_FILE* File)
{
	if (File == NULL)
		return;
	FreePool(File->Path);
	FreePool(File);
}

EFI_STATUS
NtfsOpen(EFI_NTFS_FILE* File)
{
	char* path = NULL;
	int sz;

	sz = ntfs_ucstombs(File->Path, StrLen(File->Path), &path, 0);
	if (sz <= 0) {
		PrintError(L"Could not allocate path string\n");
		return EFI_OUT_OF_RESOURCES;
	}
	File->NtfsInode = ntfs_pathname_to_inode(File->FileSystem->NtfsVolume, NULL, path);
	free(path);
	if (File->NtfsInode == NULL)
		return EFI_NOT_FOUND;

	File->IsDir = IS_DIR(File->NtfsInode);

	return EFI_SUCCESS;
}

VOID
NtfsClose(EFI_NTFS_FILE* File)
{
	ntfs_inode_close(File->NtfsInode);
}

EFI_STATUS
NtfsRead(EFI_NTFS_FILE* File, VOID* Data, UINTN* Len)
{
	ntfs_attr* na = NULL;
	s64 max_read, size = *Len;

	*Len = 0;

	na = ntfs_attr_open(File->NtfsInode, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		PrintErrno();
		return EFI_DEVICE_ERROR;
	}

	max_read = na->data_size;
	if (File->Offset + size > max_read) {
		if (max_read < File->Offset) {
			*Len = 0;
			ntfs_attr_close(na);
			return EFI_SUCCESS;
		}
		size = max_read - File->Offset;
	}

	while (size > 0) {
		s64 ret = ntfs_attr_pread(na, File->Offset, size, &((UINT8*)Data)[*Len]);
		if (ret != size)
			PrintError(L"%a: Error reading inode %lld at offset %lld: %lld <> %lld",
				((ntfs_inode*)File->NtfsInode)->mft_no,
				File->Offset, *Len, ret);
		if (ret <= 0 || ret > size) {
			ntfs_attr_close(na);
			if (ret >= 0)
				errno = EIO;
			PrintErrno();
			ntfs_attr_close(na);
			return EFI_DEVICE_ERROR;
		}
		size -= ret;
		File->Offset += ret;
		*Len += ret;
	}

	ntfs_attr_close(na);
	return EFI_SUCCESS;
}

UINT64
NtfsGetFileSize(EFI_NTFS_FILE* File)
{
	if (File->NtfsInode == NULL) 
		return 0;
	return ((ntfs_inode*)File->NtfsInode)->data_size;
}

UINT64
NtfsGetFileOffset(EFI_NTFS_FILE* File)
{
	/* TODO: Make sure this is updated after ntfs_pread() */
	return File->Offset;
}

VOID
NtfsSetFileOffset(EFI_NTFS_FILE* File, UINT64 Offset)
{
	/* TODO: use this is combination with ntfs_pread() */
	File->Offset = Offset;
}

EFI_STATUS
NtfsReaddir(EFI_NTFS_FILE* File, NTFS_DIRHOOK Hook, VOID* HookData)
{
	s64 pos = 0;

	if (ntfs_readdir(File->NtfsInode, &pos, HookData, Hook)) {
		PrintErrno();
		return EFI_NOT_FOUND;
	}

	return EFI_SUCCESS;
}

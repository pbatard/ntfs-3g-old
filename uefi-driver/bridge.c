/* bridge.c - libntfs-3g interface for UEFI */
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

#include "compat.h"
#include "volume.h"
#include "unistr.h"
#include "logging.h"

#include "driver.h"

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
	CHAR8* DevName = NULL;

	/* ntfs_ucstombs() can be used to convert to UTF-8 */
	ntfs_ucstombs(FileSystem->DevicePathString,
		StrLen(FileSystem->DevicePathString), &DevName, 0);
	if (DevName == NULL)
		return EFI_OUT_OF_RESOURCES;

	/* Insert this filesystem in our list so that ntfs_mount() can locate it */
	InsertTailList(&FsListHead, (LIST_ENTRY*)FileSystem);

	ntfs_log_set_handler(ntfs_log_handler_uefi);

	FileSystem->NtfsVolume = ntfs_mount(DevName, 0);
	FreePool(DevName);

	if (FileSystem->NtfsVolume == NULL) {
		RemoveEntryList((LIST_ENTRY*)FileSystem);
		return EFI_NOT_FOUND;
	}

	return EFI_SUCCESS;
}

EFI_STATUS
NtfsUnmount(EFI_FS* FileSystem)
{
	ntfs_umount(FileSystem->NtfsVolume, FALSE);

	RemoveEntryList((LIST_ENTRY*)FileSystem);

	return EFI_SUCCESS;
}

EFI_STATUS
NtfsCreateFile(EFI_NTFS_FILE** File, EFI_FS* FileSystem)
{
	EFI_NTFS_FILE* NewFile;
	ntfs_inode* ni;

	NewFile = AllocateZeroPool(sizeof(*NewFile));
	if (NewFile == NULL)
		return EFI_OUT_OF_RESOURCES;

	NewFile->NtfsInode = AllocateZeroPool(sizeof(*ni));
	if (NewFile->NtfsInode == NULL) {
		FreePool(NewFile);
		return EFI_OUT_OF_RESOURCES;
	}

	/* Initialize the attributes */
	NewFile->FileSystem = FileSystem;
	FS_ASSERT(FileSystem->RootFile != NULL);
	CopyMem(&NewFile->EfiFile, &FileSystem->RootFile->EfiFile, sizeof(EFI_FILE));

	ni = (ntfs_inode*)NewFile->NtfsInode;
	ni->vol = (ntfs_volume*)FileSystem->NtfsVolume;
	/* TODO: Initialize any other ni attributes we need */

	*File = NewFile;
	return EFI_SUCCESS;
}

VOID
NtfsDestroyFile(EFI_NTFS_FILE* File)
{
	if (File == NULL)
		return;
	if (File->NtfsInode != NULL)
		FreePool(File->NtfsInode);
	FreePool(File);
}


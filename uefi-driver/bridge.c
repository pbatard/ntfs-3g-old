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
 *  Parts taken from date_time.c from Oryx Embedded:
 *  Copyright © 2010-2021 Oryx Embedded SARL.
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

#define PrintErrno()            PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno))
#define IS_DIR(ni)              (((ntfs_inode*)(ni))->mrec->flags & MFT_RECORD_IS_DIRECTORY)
#define NTFS_TO_UNIX_TIME(t)    ((t - (NTFS_TIME_OFFSET)) / 10000000)


/* From https://www.oryx-embedded.com/doc/date__time_8c_source.html */
static VOID ConvertUnixTimeToEfiTime(time_t t, EFI_TIME* Time)
{
	UINT32 a, b, c, d, e, f;

	ZeroMem(Time, sizeof(EFI_TIME));

	/* Negative Unix time values are not supported */
	if (t < 1)
		return;

	/* Clear nanoseconds */
	Time->Nanosecond = 0;

	/* Retrieve hours, minutes and seconds */
	Time->Second = t % 60;
	t /= 60;
	Time->Minute = t % 60;
	t /= 60;
	Time->Hour = t % 24;
	t /= 24;

	/* Convert Unix time to date */
	a = (UINT32)((4 * t + 102032) / 146097 + 15);
	b = (UINT32)(t + 2442113 + a - (a / 4));
	c = (20 * b - 2442) / 7305;
	d = b - 365 * c - (c / 4);
	e = d * 1000 / 30601;
	f = d - e * 30 - e * 601 / 1000;

	/* January and February are counted as months 13 and 14 of the previous year */
	if (e <= 13) {
		c -= 4716;
		e -= 1;
	} else {
		c -= 4715;
		e -= 13;
	}

	/* Retrieve year, month and day */
	Time->Year = c;
	Time->Month = e;
	Time->Day = f;
}

/* Compute an EFI_TIME representation of an ntfs_time field */
VOID
NtfsGetEfiTime(EFI_NTFS_FILE* File, EFI_TIME* Time, INTN Type)
{
	ntfs_inode* ni = (ntfs_inode*)File->NtfsInode;
	ntfs_time time = NTFS_TIME_OFFSET;

	FS_ASSERT(ni != NULL);

	if (ni != NULL) {
		switch (Type) {
		case TIME_CREATED:
			time = ni->creation_time;
			break;
		case TIME_ACCESSED:
			time = ni->last_access_time;
			break;
		case TIME_MODIFIED:
			time = ni->last_data_change_time;
			break;
		default:
			FS_ASSERT(TRUE);
			break;
		}
	}

	ConvertUnixTimeToEfiTime(NTFS_TO_UNIX_TIME(time), Time);
}

VOID
NtfsSetLogger(UINTN LogLevel)
{
	/* Critical is always enabled for the UEFI driver */
	UINT32 levels = NTFS_LOG_LEVEL_CRITICAL;
	
	if (LogLevel >= FS_LOGLEVEL_ERROR)
		levels |= NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR;
	if (LogLevel >= FS_LOGLEVEL_WARNING)
		levels |= NTFS_LOG_LEVEL_WARNING;
	if (LogLevel >= FS_LOGLEVEL_INFO)
		levels |= NTFS_LOG_LEVEL_INFO | NTFS_LOG_LEVEL_VERBOSE | NTFS_LOG_LEVEL_PROGRESS;
	if (LogLevel >= FS_LOGLEVEL_DEBUG)
		levels |= NTFS_LOG_LEVEL_DEBUG | NTFS_LOG_LEVEL_QUIET;
	if (LogLevel >= FS_LOGLEVEL_EXTRA)
		levels |= NTFS_LOG_LEVEL_TRACE;

	ntfs_log_set_levels(levels);
}

EFI_STATUS
NtfsMount(EFI_FS* FileSystem)
{
	ntfs_volume* vol = NULL;
	ntfs_mount_flags flags = NTFS_MNT_EXCLUSIVE | NTFS_MNT_IGNORE_HIBERFILE | NTFS_MNT_MAY_RDONLY;
	CHAR8* DevName = NULL;

#ifdef FORCE_READONLY
	flags |= NTFS_MNT_RDONLY;
#endif

	/* ntfs_ucstombs() can be used to convert to UTF-8 */
	ntfs_ucstombs(FileSystem->DevicePathString,
		StrLen(FileSystem->DevicePathString), &DevName, 0);
	if (DevName == NULL)
		return EFI_OUT_OF_RESOURCES;

	/* Insert this filesystem in our list so that ntfs_mount() can locate it */
	InsertTailList(&FsListHead, (LIST_ENTRY*)FileSystem);

	ntfs_log_set_handler(ntfs_log_handler_uefi);

	vol = ntfs_mount(DevName, flags);
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

UINT64
NtfsGetVolumeFreeSpace(VOID* NtfsVolume)
{
	ntfs_volume* vol = (ntfs_volume*)NtfsVolume;

	return vol->free_clusters * vol->cluster_size;
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
	return File->Offset;
}

VOID
NtfsSetFileOffset(EFI_NTFS_FILE* File, UINT64 Offset)
{
	File->Offset = Offset;
}

EFI_STATUS
NtfsSetInfo(EFI_FILE_INFO* Info, VOID* NtfsVolume, CONST UINT64 MRef)
{
	ntfs_inode* ni = ntfs_inode_open(NtfsVolume, MRef);

	if (ni == NULL)
		return EFI_NOT_FOUND;

	Info->FileSize = ni->data_size;
	ConvertUnixTimeToEfiTime(NTFS_TO_UNIX_TIME(ni->creation_time), &Info->CreateTime);
	ConvertUnixTimeToEfiTime(NTFS_TO_UNIX_TIME(ni->last_access_time), &Info->LastAccessTime);
	ConvertUnixTimeToEfiTime(NTFS_TO_UNIX_TIME(ni->last_data_change_time), &Info->ModificationTime);

	Info->Attribute = 0;
	if (ni->flags & FILE_ATTR_READONLY)
		Info->Attribute |= EFI_FILE_READ_ONLY;
	if (ni->flags & FILE_ATTR_HIDDEN)
		Info->Attribute |= EFI_FILE_HIDDEN;
	if (ni->flags & FILE_ATTR_SYSTEM)
		Info->Attribute |= EFI_FILE_SYSTEM;
	if (ni->flags & FILE_ATTR_ARCHIVE)
		Info->Attribute |= EFI_FILE_ARCHIVE;

#ifdef FORCE_READONLY
	Info->Attribute |= EFI_FILE_READ_ONLY;
#endif

	ntfs_inode_close(ni);
	return EFI_SUCCESS;
}

EFI_STATUS
NtfsReadDir(EFI_NTFS_FILE* File, NTFS_DIRHOOK Hook, VOID* HookData)
{
	s64 pos = 0;

	if (ntfs_readdir(File->NtfsInode, &pos, HookData, Hook)) {
		PrintErrno();
		return EFI_NOT_FOUND;
	}

	return EFI_SUCCESS;
}

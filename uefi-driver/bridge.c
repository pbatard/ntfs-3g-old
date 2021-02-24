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
#include "uefi_logging.h"
#include "uefi_support.h"

#define IS_DIR(ni)      (((ntfs_inode*)(ni))->mrec->flags & MFT_RECORD_IS_DIRECTORY)

/*
 * Translate a UEFI driver log level into a libntfs-3g log level.
 */
VOID
NtfsSetLogger(UINTN Level)
{
	/* Critical is always enabled for the UEFI driver */
	UINT32 levels = NTFS_LOG_LEVEL_CRITICAL;
	
	if (Level >= FS_LOGLEVEL_ERROR)
		levels |= NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR;
	if (Level >= FS_LOGLEVEL_WARNING)
		levels |= NTFS_LOG_LEVEL_WARNING;
	if (Level >= FS_LOGLEVEL_INFO)
		levels |= NTFS_LOG_LEVEL_INFO | NTFS_LOG_LEVEL_VERBOSE | NTFS_LOG_LEVEL_PROGRESS;
	if (Level >= FS_LOGLEVEL_DEBUG)
		levels |= NTFS_LOG_LEVEL_DEBUG | NTFS_LOG_LEVEL_QUIET;
	if (Level >= FS_LOGLEVEL_EXTRA)
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
		SafeStrLen(FileSystem->DevicePathString), &DevName, 0);
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

EFI_STATUS
NtfsCreateFile(EFI_NTFS_FILE** File, EFI_FS* FileSystem)
{
	EFI_NTFS_FILE* NewFile;

	NewFile = AllocateZeroPool(sizeof(*NewFile));
	if (NewFile == NULL)
		return EFI_OUT_OF_RESOURCES;

	/* Initialize the attributes */
	NewFile->FileSystem = FileSystem;
	NewFile->EfiFile.Revision = EFI_FILE_PROTOCOL_REVISION2;
	NewFile->EfiFile.Open = FileOpen;
	NewFile->EfiFile.Close = FileClose;
	NewFile->EfiFile.Delete = FileDelete;
	NewFile->EfiFile.Read = FileRead;
	NewFile->EfiFile.Write = FileWrite;
	NewFile->EfiFile.GetPosition = FileGetPosition;
	NewFile->EfiFile.SetPosition = FileSetPosition;
	NewFile->EfiFile.GetInfo = FileGetInfo;
	NewFile->EfiFile.SetInfo = FileSetInfo;
	NewFile->EfiFile.Flush = FileFlush;
	NewFile->EfiFile.OpenEx = FileOpenEx;
	NewFile->EfiFile.ReadEx = FileReadEx;
	NewFile->EfiFile.WriteEx = FileWriteEx;
	NewFile->EfiFile.FlushEx = FileFlushEx;

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

	/* Open root if Path is NULL, regular file or dir otherwise */
	if (File->Path == NULL) {
		File->NtfsInode = ntfs_inode_open(File->FileSystem->NtfsVolume, FILE_root);
		File->IsRoot = TRUE;
	} else {
		sz = ntfs_ucstombs(File->Path, SafeStrLen(File->Path), &path, 0);
		if (sz <= 0) {
			PrintError(L"Could not allocate path string\n");
			return EFI_OUT_OF_RESOURCES;
		}
		File->NtfsInode = ntfs_pathname_to_inode(File->FileSystem->NtfsVolume, NULL, path);
		free(path);
	}
	if (File->NtfsInode == NULL)
		return EFI_NOT_FOUND;

	File->IsDir = IS_DIR(File->NtfsInode);

	return EFI_SUCCESS;
}

VOID
NtfsClose(EFI_NTFS_FILE* File)
{
	if (File == NULL)
		return;
	ntfs_inode_close(File->NtfsInode);
}

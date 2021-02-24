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
 * Convert an errno to an EFI_STATUS code. Adapted from:
 * https://github.com/ipxe/ipxe/blob/master/src/include/ipxe/errno/efi.h
 */
static EFI_STATUS ErrnoToEfiStatus(VOID)
{
	switch (errno) {
	case 0:
		return EFI_SUCCESS;
	case ECANCELED:
		return EFI_ABORTED;
	case EACCES:
	case EPERM:
	case ETXTBSY:
		return EFI_ACCESS_DENIED;
	case EADDRINUSE:
	case EALREADY:
	case EINPROGRESS:
	case EISCONN:
		return EFI_ALREADY_STARTED;
	case EMSGSIZE:
		return EFI_BAD_BUFFER_SIZE;
	case E2BIG:
	case EOVERFLOW:
	case ERANGE:
		return EFI_BUFFER_TOO_SMALL;
	case ENODEV:
		return EFI_DEVICE_ERROR;
	case ENOEXEC:
		return EFI_LOAD_ERROR;
	case ESPIPE:
		return EFI_END_OF_FILE;
	case EFBIG:
		return EFI_END_OF_MEDIA;
	case EBADF:
	case EDOM:
	case EFAULT:
	case EIDRM:
	case EILSEQ:
	case EINVAL:
	case ENAMETOOLONG:
	case EPROTOTYPE:
		return EFI_INVALID_PARAMETER;
	case EMFILE:
	case EMLINK:
	case ENFILE:
	case ENOBUFS:
	case ENOLCK:
	case ENOLINK:
	case ENOMEM:
	case ENOSR:
		return EFI_OUT_OF_RESOURCES;
	case EBADMSG:
	case EISDIR:
	case EIO:
	case ENOMSG:
	case ENOSTR:
	case EPROTO:
		return EFI_PROTOCOL_ERROR;
	case EBUSY:
	case ENODATA:
		return EFI_NO_RESPONSE;
	case ECHILD:
	case ENOENT:
	case ENXIO:
		return EFI_NOT_FOUND;
	case EAGAIN:
	case EINTR:
	case EWOULDBLOCK:
		return EFI_NOT_READY;
	case ESRCH:
		return EFI_NOT_STARTED;
	case ETIME:
	case ETIMEDOUT:
		return EFI_TIMEOUT;
	case EAFNOSUPPORT:
	case ENOPROTOOPT:
	case ENOSYS:
	case ENOTSUP:
	case EOPNOTSUPP:
		return EFI_UNSUPPORTED;
	case ELOOP:
	case ENOTDIR:
	case ENOTEMPTY:
	case EXDEV:
		return EFI_VOLUME_CORRUPTED;
	case ENOSPC:
		return EFI_VOLUME_FULL;
	case EEXIST:
	case EROFS:
		return EFI_WRITE_PROTECTED;
	default:
		return EFI_NO_MAPPING;
	}
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

	UnixTimeToEfiTime(NTFS_TO_UNIX_TIME(time), Time);
}

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

BOOLEAN
NtfsIsVolumeReadOnly(VOID* NtfsVolume)
{
#ifdef FORCE_READONLY
	/* NVolReadOnly() should apply, but just in case... */
	return TRUE;
#else
	ntfs_volume* vol = (ntfs_volume*)NtfsVolume;
	return NVolReadOnly(vol);
#endif
}

EFI_STATUS
NtfsMount(EFI_FS* FileSystem)
{
	int sz;
	ntfs_volume* vol = NULL;
	ntfs_mount_flags flags = NTFS_MNT_EXCLUSIVE | NTFS_MNT_IGNORE_HIBERFILE | NTFS_MNT_MAY_RDONLY;
	CHAR8* DevName = NULL;

	/* Don't double mount a volume */
	if (FileSystem->MountCount++ > 0)
		return EFI_SUCCESS;

#ifdef FORCE_READONLY
	flags |= NTFS_MNT_RDONLY;
#endif

	/* ntfs_ucstombs() can be used to convert to UTF-8 */
	sz = ntfs_ucstombs(FileSystem->DevicePathString,
		SafeStrLen(FileSystem->DevicePathString), &DevName, 0);
	if (sz <= 0) {
		PrintError(L"%a failed to convert '%s': %a\n",
			__FUNCTION__, FileSystem->DevicePathString, strerror(errno));
		return ErrnoToEfiStatus();
	}

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
	PrintInfo(L"Mounted volume '%s'\n", FileSystem->NtfsVolumeLabel);

	return EFI_SUCCESS;
}

EFI_STATUS
NtfsUnmount(EFI_FS* FileSystem)
{
	ntfs_umount(FileSystem->NtfsVolume, FALSE);

	PrintInfo(L"Unmounted volume '%s'\n", FileSystem->NtfsVolumeLabel);
	free(FileSystem->NtfsVolumeLabel);
	FileSystem->NtfsVolumeLabel = NULL;
	FileSystem->MountCount = 0;
	FileSystem->TotalRefCount = 0;

	RemoveEntryList((LIST_ENTRY*)FileSystem);

	return EFI_SUCCESS;
}

UINT64
NtfsGetVolumeFreeSpace(VOID* NtfsVolume)
{
	ntfs_volume* vol = (ntfs_volume*)NtfsVolume;

	ntfs_volume_get_free_space(vol);

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

	if (File->Path[0] == PATH_CHAR && File->Path[1] == 0) {
		/* Root directory */
		File->NtfsInode = ntfs_inode_open(File->FileSystem->NtfsVolume, FILE_root);
		File->IsRoot = TRUE;
	} else {
		sz = ntfs_ucstombs(File->Path, SafeStrLen(File->Path), &path, 0);
		if (sz <= 0) {
			PrintError(L"%a failed to convert '%s': %a\n",
				__FUNCTION__, File->Path, strerror(errno));
			return ErrnoToEfiStatus();
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

EFI_STATUS
NtfsRead(EFI_NTFS_FILE* File, VOID* Data, UINTN* Len)
{
	ntfs_attr* na = NULL;
	s64 max_read, size = *Len;

	*Len = 0;

	na = ntfs_attr_open(File->NtfsInode, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno));
		return ErrnoToEfiStatus();
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
			PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno));
			return ErrnoToEfiStatus();
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

/*
 * Fill an EFI_FILE_INFO struct with data from the NTFS inode.
 * This function takes either a Path or an MREF (with the MREF
 * being used if Path is NULL).
 */
EFI_STATUS
NtfsGetInfo(EFI_FILE_INFO* Info, VOID* NtfsVolume, CONST CHAR16* Path,
	CONST UINT64 MRef, BOOLEAN IsDir)
{
	char* path = NULL;
	ntfs_inode* ni;
	int sz;

	if (Path != NULL) {
		sz = ntfs_ucstombs(Path, SafeStrLen(Path), &path, 0);
		if (sz <= 0) {
			PrintError(L"%a failed to convert '%s': %a\n",
				__FUNCTION__, Path, strerror(errno));
			return ErrnoToEfiStatus();
		}
		ni = ntfs_pathname_to_inode(NtfsVolume, NULL, path);
		free(path);
	} else {
		ni = ntfs_inode_open(NtfsVolume, MRef);
	}

	if (ni == NULL)
		return EFI_NOT_FOUND;

	Info->FileSize = ni->data_size;
	Info->PhysicalSize = ni->allocated_size;
	UnixTimeToEfiTime(NTFS_TO_UNIX_TIME(ni->creation_time), &Info->CreateTime);
	UnixTimeToEfiTime(NTFS_TO_UNIX_TIME(ni->last_access_time), &Info->LastAccessTime);
	UnixTimeToEfiTime(NTFS_TO_UNIX_TIME(ni->last_data_change_time), &Info->ModificationTime);

	Info->Attribute = 0;
	if (IsDir)
		Info->Attribute |= EFI_FILE_DIRECTORY;
	if (ni->flags & FILE_ATTR_READONLY | NtfsIsVolumeReadOnly(NtfsVolume))
		Info->Attribute |= EFI_FILE_READ_ONLY;
	if (ni->flags & FILE_ATTR_HIDDEN)
		Info->Attribute |= EFI_FILE_HIDDEN;
	if (ni->flags & FILE_ATTR_SYSTEM)
		Info->Attribute |= EFI_FILE_SYSTEM;
	if (ni->flags & FILE_ATTR_ARCHIVE)
		Info->Attribute |= EFI_FILE_ARCHIVE;

	ntfs_inode_close(ni);
	return EFI_SUCCESS;
}

EFI_STATUS
NtfsReadDir(EFI_NTFS_FILE* File, NTFS_DIRHOOK Hook, VOID* HookData)
{
	s64 pos = 0;

	if (ntfs_readdir(File->NtfsInode, &pos, HookData, Hook)) {
		PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno));
		return ErrnoToEfiStatus();
	}

	return EFI_SUCCESS;
}

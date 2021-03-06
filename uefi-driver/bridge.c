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
#include "cache.h"

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

static inline int _to_utf8(CONST CHAR16* Src, char** dst, char* function)
{
	/* ntfs_ucstombs() can be used to convert to UTF-8 */
	int sz = ntfs_ucstombs(Src, SafeStrLen(Src), dst, 0);
	if (sz <= 0)
		PrintError(L"%a failed to convert '%s': %a\n",
			function, Src, strerror(errno));
	return sz;
}

#define to_utf8(Src, dst) _to_utf8(Src, dst, __FUNCTION__)

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
	/* Critical log level is always enabled */
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

	ntfs_log_clear_levels(UINT32_MAX);
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

/*
 * Soooooooo.... we have to perform our own caching here, because ntfs-3g
 * is not designed to handle double open, and the UEFI Shell *DOES* some
 * weird stuff, such as opening the same file twice, first rw then ro,
 * while keeping the rw instance opened, as well as other very illogical
 * things. Which means that, if we just hook these into ntfs_open_inode()
 * calls, all kind of bad things related to caching are supposed to happen.
 * Ergo, we need to keep a list of all the files we already have an inode
 * for, and perform look ups to prevent double inode open.
 */

/* A file lookup entry */
typedef struct {
	LIST_ENTRY* ForwardLink;
	LIST_ENTRY* BackLink;
	EFI_NTFS_FILE* File;
} LookupEntry;

/*
 * Look for an existing file instance in our list, either
 * by matching a File->Path (if Inum is 0) or the inode
 * number specified in Inum.
 * Returns a pointer to the file instance when found, NULL
 * if not found.
 */
static EFI_NTFS_FILE*
NtfsLookup(EFI_NTFS_FILE* File, UINT64 Inum)
{
	LookupEntry* ListHead = (LookupEntry*)&File->FileSystem->LookupListHead;
	LookupEntry* Entry;
	ntfs_inode* ni;

	for (Entry = (LookupEntry*)ListHead->ForwardLink;
		Entry != ListHead;
		Entry = (LookupEntry*)Entry->ForwardLink) {
		if (Inum == 0) {
			if (StrCmp(File->Path, Entry->File->Path) == 0)
				return Entry->File;
		} else {
			ni = Entry->File->NtfsInode;
			if (ni->mft_no == GetInodeNumber(Inum)) 
				return Entry->File;
		}
	}
	return NULL;
}

/* Look for a parent file instance */
static EFI_NTFS_FILE*
NtfsLookupParent(EFI_NTFS_FILE* File)
{
	EFI_NTFS_FILE* Parent = NULL;
	LookupEntry* ListHead = (LookupEntry*)&File->FileSystem->LookupListHead;
	LookupEntry* Entry;

	/* BaseName always points into a non empty Path */
	FS_ASSERT(File->BaseName[-1] == PATH_CHAR);
	File->BaseName[-1] = 0;

	for (Entry = (LookupEntry*)ListHead->ForwardLink;
		Entry != ListHead && Parent == NULL;
		Entry = (LookupEntry*)Entry->ForwardLink) {
		FS_ASSERT(Entry->File != NULL);
		/* Need to prevent ourselves from matching */
		if (Entry->File == File)
			continue;
		/* An empty path should return the root */
		if (File->Path[0] == 0 && Entry->File->IsRoot)
			Parent = Entry->File;
		if (StrCmp(File->Path, Entry->File->Path) == 0)
			Parent = Entry->File;
	}

	/* Make sure to restore the path */
	File->BaseName[-1] = PATH_CHAR;
	return Parent;
}

/* Add a new file instance to the lookup list */
static VOID
NtfsLookupAdd(EFI_NTFS_FILE* File)
{
	LIST_ENTRY* ListHead = &File->FileSystem->LookupListHead;
	LookupEntry* Entry = AllocatePool(sizeof(LookupEntry));

	if (Entry) {
		Entry->File = File;
		InsertTailList(ListHead, (LIST_ENTRY*)Entry);
	}
}

/* Remove an existing file instance from the lookup list */
static VOID
NtfsLookupRem(EFI_NTFS_FILE* File)
{
	LookupEntry* ListHead = (LookupEntry*)&File->FileSystem->LookupListHead;
	LookupEntry* Entry;

	for (Entry = (LookupEntry*)ListHead->ForwardLink;
		Entry != ListHead;
		Entry = (LookupEntry*)Entry->ForwardLink) {
		if (File == Entry->File) {
			RemoveEntryList((LIST_ENTRY*)Entry);
			FreePool(Entry);
			return;
		}
	}
}

/* Clear the lookup list and free all allocated resources */
static VOID
NtfsLookupFree(LIST_ENTRY* List)
{
	LookupEntry *ListHead = (LookupEntry*)List, *Entry;

	for (Entry = (LookupEntry*)ListHead->ForwardLink;
		Entry != ListHead;
		Entry = (LookupEntry*)Entry->ForwardLink) {
		RemoveEntryList((LIST_ENTRY*)Entry);
		FreePool(Entry);
	}
}

EFI_STATUS
NtfsMountVolume(EFI_FS* FileSystem)
{
	ntfs_volume* vol = NULL;
	ntfs_mount_flags flags = NTFS_MNT_EXCLUSIVE | NTFS_MNT_IGNORE_HIBERFILE | NTFS_MNT_MAY_RDONLY;
	char* device = NULL;

	/* Don't double mount a volume */
	if (FileSystem->MountCount++ > 0)
		return EFI_SUCCESS;

#ifdef FORCE_READONLY
	flags |= NTFS_MNT_RDONLY;
#endif

	if (to_utf8(FileSystem->DevicePathString, &device) <= 0)
		return ErrnoToEfiStatus();

	/* Insert this filesystem in our list so that ntfs_mount() can locate it */
	InsertTailList(&FsListHead, (LIST_ENTRY*)FileSystem);

	/* Initialize the Lookup List for this volume */
	InitializeListHead(&FileSystem->LookupListHead);

	ntfs_log_set_handler(ntfs_log_handler_uefi);

	vol = ntfs_mount(device, flags);
	FreePool(device);
	if (vol == NULL) {
		RemoveEntryList((LIST_ENTRY*)FileSystem);
		return EFI_NOT_FOUND;
	}
	/* Population of free space must be done manually */
	ntfs_volume_get_free_space(vol);
	FileSystem->NtfsVolume = vol;
	ntfs_mbstoucs(vol->vol_name, &FileSystem->NtfsVolumeLabel);
	PrintInfo(L"Mounted volume '%s'\n", FileSystem->NtfsVolumeLabel);

	return EFI_SUCCESS;
}

EFI_STATUS
NtfsUnmountVolume(EFI_FS* FileSystem)
{
	ntfs_umount(FileSystem->NtfsVolume, FALSE);

	PrintInfo(L"Unmounted volume '%s'\n", FileSystem->NtfsVolumeLabel);
	NtfsLookupFree(&FileSystem->LookupListHead);
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
NtfsAllocateFile(EFI_NTFS_FILE** File, EFI_FS* FileSystem)
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
NtfsFreeFile(EFI_NTFS_FILE* File)
{
	if (File == NULL)
		return;
	/* Only destroy a file that has no refs */
	if (File->RefCount <= 0) {
		FreePool(File->Path);
		FreePool(File);
	}
}

/*
 * Open or reopen a file instance
 */
EFI_STATUS
NtfsOpenFile(EFI_NTFS_FILE** FilePointer)
{
	EFI_NTFS_FILE *File, *Parent;
	char *path = NULL;
	ntfs_inode* ni;

	/* See if we already have a file instance open. */
	File = NtfsLookup(*FilePointer, 0);

	if (File != NULL) {
		/* Existing file instance found => Use that one */
		NtfsFreeFile(*FilePointer);
		*FilePointer = File;
		return EFI_SUCCESS;
	}

	/* Existing file instance was not found */
	File = *FilePointer;
	File->IsRoot = (File->Path[0] == PATH_CHAR && File->Path[1] == 0);
	if (File->IsRoot) {
		/* Root directory */
		ni = ntfs_inode_open(File->FileSystem->NtfsVolume, FILE_root);
	} else {
		Parent = NtfsLookupParent(File);
		if (to_utf8(Parent ? File->BaseName : File->Path, &path) <= 0)
			return ErrnoToEfiStatus();
		/* Use Parent inode if we have it to avoid double inode errors */
		ni = ntfs_pathname_to_inode(File->FileSystem->NtfsVolume,
			Parent ? Parent->NtfsInode : NULL, path);
		free(path);
	}
	if (ni == NULL)
		return EFI_NOT_FOUND;
	File->IsDir = IS_DIR(ni);
	File->NtfsInode = ni;

	/* Add the new entry */
	NtfsLookupAdd(File);

	return EFI_SUCCESS;
}

static int bullshit_cache_compare(const struct CACHED_GENERIC* cached,
	const struct CACHED_GENERIC* wanted)
{
	return (!cached->variable || strcmp(cached->variable, wanted->variable));
}

/*
 * Create new file or reopen an existing one
 */
EFI_STATUS
NtfsCreateFile(EFI_NTFS_FILE** FilePointer)
{
	EFI_STATUS Status;
#if CACHE_INODE_SIZE
	struct CACHED_INODE item;
	ntfs_volume* vol = (*FilePointer)->FileSystem->NtfsVolume;
#endif
	EFI_NTFS_FILE *File, *Parent = NULL;
	char *path = NULL, *basename = NULL;
	ntfs_inode *dir_ni = NULL, *ni = NULL;
	int sz;

	File = NtfsLookup(*FilePointer, 0);

	/* If an existing file instance is found, use that one */
	if (File != NULL) {
		/* Entries must be of the same type */
		if (File->IsDir != (*FilePointer)->IsDir)
			return EFI_ACCESS_DENIED;
		NtfsFreeFile(*FilePointer);
		*FilePointer = File;
		return EFI_SUCCESS;
	}
	File = *FilePointer;

	/* Convert the path to UTF-8 */
	sz = to_utf8(File->Path, &path);
	if (sz <= 0) {
		Status = ErrnoToEfiStatus();
		goto out;
	}

	/* Search for an already open parent */
	Parent = NtfsLookupParent(File);
	if (Parent != NULL) {
		dir_ni = Parent->NtfsInode;
	} else {
		/* Isolate dirname and get the directory inode */
		FS_ASSERT(path[0] == '/');
		while (path[--sz] != '/');
		path[sz] = 0;
		/* NB: ntfs_pathname_to_inode() with an empty path returns the root inode */
		dir_ni = ntfs_pathname_to_inode(File->FileSystem->NtfsVolume, NULL, path);
		path[sz] = '/';
		if (dir_ni == NULL) {
			Status = ErrnoToEfiStatus();
			goto out;
		}
	}

	/* Search the volume for an already existing inode */
	sz = to_utf8(File->BaseName, &basename);
	if (sz <= 0) {
		Status = ErrnoToEfiStatus();
		goto out;
	}

	ni = ntfs_pathname_to_inode(File->FileSystem->NtfsVolume, dir_ni, basename);
	if (ni != NULL) {
		/* Entries must be of the same type */
		if ((File->IsDir && !IS_DIR(ni)) || (!File->IsDir && IS_DIR(ni))) {
			Status = EFI_ACCESS_DENIED;
			goto out;
		}
	} else {
		/* Create the new file or directory */
		ni = ntfs_create(dir_ni, 0, File->BaseName,
			SafeStrLen(File->BaseName), File->IsDir ? S_IFDIR : S_IFREG);
		if (ni == NULL) {
			Status = ErrnoToEfiStatus();
			goto out;
		}
	}

	/* Update cache lookup record */
	ntfs_inode_update_mbsname(dir_ni, basename, ni->mft_no);

#if CACHE_INODE_SIZE
	/*
	 * Manually update xinode_cache, since ntfs-3g produces a "Could not
	 * delete inode cache entry" on delete unless we do... And that's even
	 * as we do call ntfs_pathname_to_inode(), which is supposed to update
	 * the cache. Looks like create/delete/create sequence from the UEFI
	 * Shell, when doing something like 'echo blah > file', is odd enough
	 * to trip libntfs-3g's caching. But even with this, we still get a
	 * "Bad hash list, cache lookup hashing dropped" error on deletion :(
	 */
	item.inum = ni->mft_no;
	item.pathname = path;
	item.varsize = strlen(path);
	ntfs_enter_cache(vol->xinode_cache, GENERIC(&item), bullshit_cache_compare);
#endif

	File->NtfsInode = ni;
	Status = EFI_SUCCESS;

out:
	free(basename);
	free(path);
	/* NB: ntfs_inode_close(NULL) is fine */
	if (Parent == NULL)
		ntfs_inode_close(dir_ni);
	if EFI_ERROR(Status) {
		ntfs_inode_close(ni);
		File->NtfsInode = NULL;
	}
	return Status;
}

VOID
NtfsCloseFile(EFI_NTFS_FILE* File)
{
	EFI_NTFS_FILE* Parent = NULL;
	ntfs_inode* ni;
	u64 parent_inum;

	if (File == NULL)
		return;
	ni = File->NtfsInode;
	/* 
	 * If the inode is dirty, ntfs_inode_close() will issue an
	 * ntfs_inode_sync() which may try to open the parent inode.
	 * Therefore, since ntfs-3g is not keen on reopen, if we do
	 * have the parent inode open, we need to close it first.
	 * Of course, the big question becomes: "But what if that
	 * parent's parent is also open and dirty?", which we assert
	 * it isn't...
	 */
	if (NInoDirty(ni) || NInoAttrListDirty(ni)) {
		Parent = NtfsLookupParent(File);
		if (Parent != NULL) {
			parent_inum = ((ntfs_inode*)Parent->NtfsInode)->mft_no;
			ntfs_inode_close(Parent->NtfsInode);
		}
	}
	ntfs_inode_close(File->NtfsInode);
	if (Parent != NULL) {
		Parent->NtfsInode = ntfs_inode_open(File->FileSystem->NtfsVolume, parent_inum);
		if (Parent->NtfsInode == NULL) {
			PrintError(L"%a: Failed to reopen Parent: %a\n", __FUNCTION__, strerror(errno));
			NtfsLookupRem(Parent);
		}
	}
	NtfsLookupRem(File);
}

/*
 * Like FileDelete(), this call should only
 * return EFI_WARN_DELETE_FAILURE on error.
 */
EFI_STATUS
NtfsDeleteFile(EFI_NTFS_FILE* File)
{
	EFI_NTFS_FILE *Parent = NULL, *GrandParent = NULL;
	char* path = NULL;
	ntfs_inode* dir_ni;
	u64 parent_inum, grandparent_inum;
	int r, sz;

	/* Convert the path to UTF-8 */
	sz = to_utf8(File->Path, &path);
	if (sz <= 0)
		return EFI_WARN_DELETE_FAILURE;

	Parent = NtfsLookupParent(File);

	/* If the lookup failed, then the parent dir is not already open */
	if (Parent == NULL) {
		/* Isolate dirname and get the inode */
		FS_ASSERT(path[0] == '/');
		while (path[--sz] != '/');
		path[sz] = 0;
		dir_ni = ntfs_pathname_to_inode(File->FileSystem->NtfsVolume, NULL, path);
		path[sz] = '/';
	} else {
		/*
		 * ntfs-3g may attempt to reopen the file's grandparent, since it
		 * issue ntfs_inode_close on dir_ni which, when dir_ni is dirty,
		 * ultimately results in ntfs_inode_sync_file_name(dir_ni, NULL)
		 * which calls ntfs_inode_open(le64_to_cpu(fn->parent_directory))
		 * So we must make sure the grandparent's inode is closed...
		 */
		GrandParent = NtfsLookupParent(Parent);
		if (GrandParent != NULL) {
			if (GrandParent->IsRoot) {
				GrandParent = NULL;
			} else {
				grandparent_inum = ((ntfs_inode*)GrandParent->NtfsInode)->mft_no;
				ntfs_inode_close(GrandParent->NtfsInode);
			}
		}

		/* Parent dir was already open */
		dir_ni = Parent->NtfsInode;
		parent_inum = dir_ni->mft_no;
	}

	/* Delete the file */
	r = ntfs_delete(File->FileSystem->NtfsVolume, path, File->NtfsInode,
		dir_ni, File->BaseName, SafeStrLen(File->BaseName));
	free(path);
	NtfsLookupRem(File);
	if (r < 0) {
		PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno));
		return EFI_WARN_DELETE_FAILURE;
	}

	/* Reopen Parent or GrandParent if they were closed */
	if (Parent != NULL) {
		Parent->NtfsInode = ntfs_inode_open(File->FileSystem->NtfsVolume, parent_inum);
		if (Parent->NtfsInode == NULL) {
			PrintError(L"%a: Failed to reopen Parent: %a\n", __FUNCTION__, strerror(errno));
			NtfsLookupRem(Parent);
			return ErrnoToEfiStatus();
		}
	}
	if (GrandParent != NULL) {
		GrandParent->NtfsInode = ntfs_inode_open(File->FileSystem->NtfsVolume, grandparent_inum);
		if (GrandParent->NtfsInode == NULL) {
			PrintError(L"%a: Failed to reopen GrandParent: %a\n", __FUNCTION__, strerror(errno));
			NtfsLookupRem(GrandParent);
			return ErrnoToEfiStatus();
		}
	}

	return EFI_SUCCESS;
}

EFI_STATUS
NtfsReadFile(EFI_NTFS_FILE* File, VOID* Data, UINTN* Len)
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

EFI_STATUS
NtfsWriteFile(EFI_NTFS_FILE* File, VOID* Data, UINTN* Len)
{
	ntfs_attr* na = NULL;
	s64 size = *Len;

	*Len = 0;

	na = ntfs_attr_open(File->NtfsInode, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		PrintError(L"%a failed (open): %a\n", __FUNCTION__, strerror(errno));
		return ErrnoToEfiStatus();
	}

	while (size > 0) {
		s64 ret = ntfs_attr_pwrite(na, File->Offset, size, &((UINT8*)Data)[*Len]);
		if (ret <= 0) {
			ntfs_attr_close(na);
			if (ret >= 0)
				errno = EIO;
			PrintError(L"%a failed (write): %a\n", __FUNCTION__, strerror(errno));
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
 * This function takes either a File or an MREF (with the MREF
 * being used if it's non-zero).
 */
EFI_STATUS
NtfsGetFileInfo(EFI_NTFS_FILE* File, EFI_FILE_INFO* Info, CONST UINT64 MRef, BOOLEAN IsDir)
{
	BOOLEAN NeedClose = FALSE;
	EFI_NTFS_FILE* Existing = NULL;
	ntfs_inode* ni = File->NtfsInode;

	/*
	 * If Non-zero MREF, we are listing a dir, in which case we need
	 * to open (and later close) the inode.
	 */
	if (MRef != 0) {
		Existing = NtfsLookup(File, MRef);
		if (Existing != NULL) {
			ni = Existing->NtfsInode;
		} else {
			ni = ntfs_inode_open(File->FileSystem->NtfsVolume, MRef);
			NeedClose = TRUE;
		}
	} else
		PrintExtra(L"NtfsGetInfo for inode: %lld\n", ni->mft_no);

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
	if (ni->flags & FILE_ATTR_READONLY | NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume))
		Info->Attribute |= EFI_FILE_READ_ONLY;
	if (ni->flags & FILE_ATTR_HIDDEN)
		Info->Attribute |= EFI_FILE_HIDDEN;
	if (ni->flags & FILE_ATTR_SYSTEM)
		Info->Attribute |= EFI_FILE_SYSTEM;
	if (ni->flags & FILE_ATTR_ARCHIVE)
		Info->Attribute |= EFI_FILE_ARCHIVE;

	if (NeedClose)
		ntfs_inode_close(ni);

	return EFI_SUCCESS;
}

/*
 * Update NTFS inode data with the attributes from an EFI_FILE_INFO struct.
 */
EFI_STATUS
NtfsSetFileInfo(EFI_NTFS_FILE* File, EFI_FILE_INFO* Info)
{
	ntfs_inode* ni = File->NtfsInode;
	ntfs_attr* na;
	int r;

	/* Resize the data section accordingly */
	PrintExtra(L"NtfsSetInfo for inode: %lld\n", ni->mft_no);

	if (Info->FileSize != ni->data_size) {
		na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
		if (!na) {
			PrintError(L"%a ntfs_attr_open failed: %a\n", __FUNCTION__, strerror(errno));
			return ErrnoToEfiStatus();
		}
		r = ntfs_attr_truncate(na, Info->FileSize);
		ntfs_attr_close(na);
		if (r) {
			PrintError(L"%a ntfs_attr_truncate failed: %a\n", __FUNCTION__, strerror(errno));
			return ErrnoToEfiStatus();
		}
	}

	ni->creation_time = UNIX_TO_NTFS_TIME(EfiTimeToUnixTime(&Info->CreateTime));
	ni->last_access_time = UNIX_TO_NTFS_TIME(EfiTimeToUnixTime(&Info->LastAccessTime));
	ni->last_data_change_time = UNIX_TO_NTFS_TIME(EfiTimeToUnixTime(&Info->ModificationTime));

	ni->flags &= ~(FILE_ATTR_READONLY | FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM | FILE_ATTR_ARCHIVE);
	if (Info->Attribute & EFI_FILE_READ_ONLY)
		ni->flags |= FILE_ATTR_READONLY;
	if (Info->Attribute & EFI_FILE_HIDDEN)
		ni->flags |= FILE_ATTR_HIDDEN;
	if (Info->Attribute & EFI_FILE_SYSTEM)
		ni->flags |= FILE_ATTR_SYSTEM;
	if (Info->Attribute & EFI_FILE_ARCHIVE)
		ni->flags |= FILE_ATTR_ARCHIVE;

	/* No sync, since, per UEFI specs, change of attributes apply on close */
	return EFI_SUCCESS;
}

EFI_STATUS
NtfsReadDirectory(EFI_NTFS_FILE* File, NTFS_DIRHOOK Hook, VOID* HookData)
{
	s64 pos = 0;

	if (ntfs_readdir(File->NtfsInode, &pos, HookData, Hook)) {
		PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno));
		return ErrnoToEfiStatus();
	}

	return EFI_SUCCESS;
}

EFI_STATUS
NtfsRenameVolume(VOID* NtfsVolume, CONST CHAR16* Label, CONST INTN Len)
{
	if (ntfs_volume_rename(NtfsVolume, Label, (int)Len) < 0) {
		PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno));
		return ErrnoToEfiStatus();
	}
	return EFI_SUCCESS;
}

EFI_STATUS
NtfsFlushFile(EFI_NTFS_FILE* File)
{
	EFI_STATUS Status = EFI_SUCCESS;
	EFI_NTFS_FILE* Parent = NULL;
	ntfs_inode* ni;
	u64 parent_inum;

	ni = File->NtfsInode;
	/* Nothing to do if the file is not dirty */
	if (!NInoDirty(ni) && NInoAttrListDirty(ni))
		return EFI_SUCCESS;

	/*
	 * Same story as with NtfsCloseFile, with the parent 
	 * inode needing to be closed to be able to issue sync()
	 */
	Parent = NtfsLookupParent(File);
	if (Parent != NULL) {
		parent_inum = ((ntfs_inode*)Parent->NtfsInode)->mft_no;
		ntfs_inode_close(Parent->NtfsInode);
	}
	if (ntfs_inode_sync(File->NtfsInode) < 0) {
		PrintError(L"%a failed: %a\n", __FUNCTION__, strerror(errno));
		Status = ErrnoToEfiStatus();
	}
	if (Parent != NULL) {
		Parent->NtfsInode = ntfs_inode_open(File->FileSystem->NtfsVolume, parent_inum);
		if (Parent->NtfsInode == NULL) {
			PrintError(L"%a: Failed to reopen Parent: %a\n", __FUNCTION__, strerror(errno));
			NtfsLookupRem(Parent);
		}
	}
	return Status;
}

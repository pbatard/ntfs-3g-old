/* uefi_file.c - SimpleFileIo Interface */
/*
 *  Copyright © 2014-2021 Pete Batard <pete@akeo.ie>
 *  Based on iPXE's efi_driver.c and efi_file.c:
 *  Copyright © 2011,2013 Michael Brown <mbrown@fensystems.co.uk>.
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

#include "uefi_driver.h"
#include "uefi_bridge.h"
#include "uefi_logging.h"
#include "uefi_support.h"

/**
 * Open file
 *
 * @v This			File handle
 * @ret new			New file handle
 * @v Name			File name
 * @v Mode			File mode
 * @v Attributes	File attributes (for newly-created files)
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileOpen(EFI_FILE_HANDLE This, EFI_FILE_HANDLE* New,
	CHAR16* Name, UINT64 Mode, UINT64 Attributes)
{
	EFI_STATUS Status;
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);
	EFI_NTFS_FILE* NewFile = NULL;
	CHAR16* Path = NULL;
	INTN i, Len;

	PrintInfo(L"Open(" PERCENT_P L"%s, \"%s\", Mode %llx)\n", (UINTN)This,
		File->IsRoot ? L" <ROOT>" : L"", Name, Mode);

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume) && (Mode != EFI_FILE_MODE_READ)) {
		PrintInfo(L"Invalid mode for read-only media\n", Name);
		return EFI_WRITE_PROTECTED;
	}

	/* Additional failures */
	if ((StrCmp(Name, L"..") == 0) && File->IsRoot) {
		PrintInfo(L"Trying to open <ROOT>'s parent\n");
		return EFI_NOT_FOUND;
	}
	if (!File->IsDir) {
		PrintWarning(L"Parent is not a directory\n");
		return EFI_NOT_FOUND;
	}

	/* Prevent the creation of files named '', '.' or '..' */
	if ((Mode & EFI_FILE_MODE_CREATE) && (
		(*Name == 0) || (StrCmp(Name, L".") == 0) ||
		(StrCmp(Name, L"..") == 0)))
		return EFI_ACCESS_DENIED;

	/* See if we're trying to reopen current (which the Shell insists on doing) */
	if ((*Name == 0) || (StrCmp(Name, L".") == 0)) {
		PrintInfo(L"  Reopening %s\n", File->IsRoot ? L"<ROOT>" : File->Path);
		File->RefCount++;
		File->FileSystem->TotalRefCount++;
		PrintExtra(L"TotalRefCount = %d\n", File->FileSystem->TotalRefCount);
		*New = This;
		PrintInfo(L"  RET: " PERCENT_P L"\n", (UINTN)*New);
		return EFI_SUCCESS;
	}

	Path = AllocateZeroPool(PATH_MAX * sizeof(CHAR16));
	if (Path == NULL) {
		PrintError(L"Could not allocate path\n");
		Status = EFI_OUT_OF_RESOURCES;
		goto out;
	}

	/* If we have an absolute path, don't bother completing with the parent */
	if (IS_PATH_DELIMITER(Name[0])) {
		Len = 0;
	} else {
		SafeStrCpy(Path, PATH_MAX, File->Path);
		Len = SafeStrLen(Path);
		/* Add delimiter */
		Path[Len++] = PATH_CHAR;
	}

	/* Copy the rest of the path */
	SafeStrCpy(&Path[Len], PATH_MAX - Len, Name);

	/* Convert the delimiters if needed */
	for (i = SafeStrLen(Path) - 1; i >= Len; i--) {
		if (Path[i] == DOS_PATH_CHAR)
			Path[i] = PATH_CHAR;
	}

	/* Clean the path by removing double delimiters and processing '.' and '..' */
	CleanPath(Path);

	/* Validate that our paths are non-empty and absolute */
	FS_ASSERT(Path[0] == PATH_CHAR);

	/* Allocate and initialise an instance of a file */
	Status = NtfsAllocateFile(&NewFile, File->FileSystem);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not instantiate file");
		goto out;
	}

	/* Extra check to see if we're trying to create root */
	if (Path[0] == PATH_CHAR && Path[1] == 0 && (Mode & EFI_FILE_MODE_CREATE)) {
		Status = EFI_ACCESS_DENIED;
		goto out;
	}

	NewFile->Path = Path;
	/* Avoid double free on error */
	Path = NULL;

	/* Set BaseName */
	for (i = SafeStrLen(NewFile->Path) - 1; i >= 0; i--) {
		if (NewFile->Path[i] == PATH_CHAR)
			break;
	}
	NewFile->BaseName = &NewFile->Path[i + 1];

	/* NB: The calls below may update NewFile to an existing open instance */
	if (Mode & EFI_FILE_MODE_CREATE) {
		NewFile->IsDir = Attributes & EFI_FILE_DIRECTORY;
		PrintInfo(L"Creating %s '%s'\n", NewFile->IsDir ? L"dir" : L"file", NewFile->Path);
		Status = NtfsCreateFile(&NewFile);
		if (EFI_ERROR(Status))
			goto out;
	} else {
		Status = NtfsOpenFile(&NewFile);
		if (EFI_ERROR(Status)) {
			if (Status != EFI_NOT_FOUND)
				PrintStatusError(Status, L"Could not open file '%s'", Name);
			goto out;
		}
	}

	NewFile->RefCount++;
	File->FileSystem->TotalRefCount++;
	PrintExtra(L"TotalRefCount = %d\n", File->FileSystem->TotalRefCount);
	*New = &NewFile->EfiFile;

	PrintInfo(L"  RET: " PERCENT_P L"\n", (UINTN)*New);
	Status = EFI_SUCCESS;

out:
	if (EFI_ERROR(Status)) {
		/* NB: This call only destroys the file if RefCount = 0 */
		NtfsFreeFile(NewFile);
	}
	FreePool(Path);
	return Status;
}

/* Ex version */
EFI_STATUS EFIAPI
FileOpenEx(EFI_FILE_HANDLE This, EFI_FILE_HANDLE* New, CHAR16* Name,
	UINT64 Mode, UINT64 Attributes, EFI_FILE_IO_TOKEN* Token)
{
	return FileOpen(This, New, Name, Mode, Attributes);
}

/**
 * Close file
 *
 * @v This			File handle
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileClose(EFI_FILE_HANDLE This)
{
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);
	/* Keep a pointer to the FS since we're going to delete File */
	EFI_FS* FileSystem = File->FileSystem;

	PrintInfo(L"Close(" PERCENT_P L"|'%s') %s\n", (UINTN)This, File->Path,
		File->IsRoot ? L"<ROOT>" : L"");

	File->RefCount--;
	if (File->RefCount <= 0) {
		NtfsCloseFile(File);
		/* NB: BaseName points into File->Path and does not need to be freed */
		NtfsFreeFile(File);
	}

	/* If there are no more files open on the volume, unmount it */
	FileSystem->TotalRefCount--;
	PrintExtra(L"TotalRefCount = %d\n", FileSystem->TotalRefCount);
	if (FileSystem->TotalRefCount <= 0) {
		PrintInfo(L"Last file instance: Unmounting volume\n");
		NtfsUnmountVolume(FileSystem);
	}

	return EFI_SUCCESS;
}

/**
 * Close and delete file
 *
 * @v This			File handle
 * @ret Status		EFI status code
 *
 * Note that, per specs, this function call can only ever
 * return EFI_SUCCESS or EFI_WARN_DELETE_FAILURE...
 */
EFI_STATUS EFIAPI
FileDelete(EFI_FILE_HANDLE This)
{
	EFI_STATUS Status;
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);

	/* Keep a pointer to the FS since we're going to delete File */
	EFI_FS* FileSystem = File->FileSystem;

	PrintInfo(L"Delete(" PERCENT_P L"|'%s') %s\n", (UINTN)This, File->Path,
		File->IsRoot ? L"<ROOT>" : L"");

	if (File->IsRoot || File->NtfsInode == NULL)
		return EFI_ACCESS_DENIED;

	File->RefCount--;
	FileSystem->TotalRefCount--;
	PrintExtra(L"TotalRefCount = %d\n", FileSystem->TotalRefCount);

	/* No need to close the file, NtfsDeleteFile will do it */

	/* Don't delete root or files that have more than one ref */
	if (File->IsRoot || File->RefCount > 0)
		return EFI_WARN_DELETE_FAILURE;

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume)) {
		PrintError(L"Cannot delete '%s'\n", File->Path);
		return EFI_WARN_DELETE_FAILURE;
	}

	Status = NtfsDeleteFile(File);
	NtfsFreeFile(File);

	/* If there are no more files open on the volume, unmount it */
	if (FileSystem->TotalRefCount <= 0) {
		PrintInfo(L"Last file instance: Unmounting volume\n");
		NtfsUnmountVolume(FileSystem);
	}
	return Status;
}

/*
 * Directory listing is tricky because on one hand, UEFI wants repeated
 * Read() calls on a directory handle, that return a single directory
 * entry each, and on the other hand, ntfs-3g wants a single readdir()
 * call on a directory inode, that invokes a callback for each directory
 * entry. Thus, a basic way of processing dirs, by invoking readddir()
 * for each UEFI Read() call and then using the repeated callbacks to
 * match with the relevant entry, would turn out to be very wasteful as
 * we'd literally be invoking a full directory listing for every entry
 * with the end result that, if you were to list something like Windows'
 * System32 dir, that typically contains more than 3,000 entries, you'd
 * end up issuing 3,000+ *full* directory listings.
 *
 * In order to improve performance, we therefore implement a mechanism
 * that caches the directory listings we get from ntfs-3g, through a
 * two-pass callback system:
 * - The first pass counts the number of entries and determines the
 *   maximum filename size, so that we can allocate a properly sized
 *   array of EFI_FILE_INFO structures.
 * - The second pass fills the newly allocated array with the relevant
 *   data.
 * Then, when UEFI calls Read(), we just return the cached entry.
 *
 * While this of course requires additional memory, if we assume a max
 * filename length of about 64 characters, the overhead is of about 200
 * KB per 1000 directory entries, which we deem more than acceptable,
 * even for low memory UEFI systems.
 * 
 * Note that, for performance reasons, we do reuse the same cache for
 * all of the directory handles we serve, and, more importantly, that
 * the cache does not get updated until all handles have been closed.
 * For typical UEFI Shell usage, this is more than good enough. But if
 * you are using this driver in an application, please be mindful that
 * directory changes may not appear until that directory is reopened.
 */

/**
 * First-pass DirHook: Count entries and find the max filename length.
 */
static INT32 DirHookCount(VOID* Data, CONST CHAR16* Name,
	CONST INT32 NameLen, CONST INT32 NameType, CONST INT64 Pos,
	CONST UINT64 MRef, CONST UINT32 DtType)
{
	EFI_NTFS_FILE* File = (EFI_NTFS_FILE*)Data;

	/* Don't list any system files except root */
	if (GetInodeNumber(MRef) < FILE_FIRST_USER && GetInodeNumber(MRef) != FILE_ROOT)
		return 0;

	File->DirEntryCount++;
	File->DirEntrySize = MAX(File->DirEntrySize,
		SIZE_OF_EFI_FILE_INFO + ((UINTN)NameLen + 1) * sizeof(CHAR16));
	return 0;
}

/**
 * Second-pass DirHook: Fill the EFI_FILE_INFO cache array.
 */
static INT32 DirHookCache(VOID* Data, CONST CHAR16* Name,
	CONST INT32 NameLen, CONST INT32 NameType, CONST INT64 Pos,
	CONST UINT64 MRef, CONST UINT32 DtType)
{
	EFI_STATUS Status;
	EFI_FILE_INFO* Info;
	EFI_NTFS_FILE* File = (EFI_NTFS_FILE*)Data;
	UINT8* p = (UINT8*)File->DirEntry;

	/* Don't list any system files except root */
	if (GetInodeNumber(MRef) < FILE_FIRST_USER && GetInodeNumber(MRef) != FILE_ROOT)
		return 0;

	/* No overflow here since we are just computing the address */
	Info = (EFI_FILE_INFO*) &p[File->DirPos * File->DirEntrySize];

	/*
	 * Technically, it is possible for a new entry to have been added
	 * between our first and second pass, in which case we may end up
	 * with an array overflow. Protect ourselves against that.
	 */
	if (File->DirPos++ >= File->DirEntryCount) {
		PrintError(L"Unexpected directory entry!");
		return -1;
	}

	/* Also for safety, validate that NameLen still fits our records */
	if (SIZE_OF_EFI_FILE_INFO + ((UINTN)NameLen + 1) * sizeof(CHAR16) > File->DirEntrySize) {
		PrintError(L"Unexpected directory entry name length!");
		/* We could just truncate the name, but something's amiss here */
		return -1;
	}

	/* Copy the filename and set the size */
	CopyMem(Info->FileName, Name, NameLen * sizeof(CHAR16));
	Info->FileName[NameLen] = 0;
	Info->Size = SIZE_OF_EFI_FILE_INFO + ((UINTN)NameLen + 1) * sizeof(CHAR16);

	/*
	 * Set the Info attributes we obtain from the inode.
	 * DtType is 4 for directories.
	 */
	Status = NtfsGetFileInfo(File, Info, MRef, (DtType == 4));
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not get directory entry info");
		NtfsSetErrno(Status);
		return -1;
	}

	return 0;
}

static EFI_STATUS
FileReturnDirEntry(EFI_NTFS_FILE* File, UINTN* Len, VOID* Data)
{
	EFI_FILE_INFO* Info;
	UINT8* p = (UINT8*)File->DirEntry;

	/* End of lookup */
	if (File->DirPos >= File->DirEntryCount) {
		/* Note that we don't destroy the cache here */
		*Len = 0;
		return EFI_SUCCESS;
	}


	FS_ASSERT(File->DirPos < File->DirEntryCount);
	Info = (EFI_FILE_INFO*)&p[File->DirPos * File->DirEntrySize];
	FS_ASSERT(Info->Size <= SIZE_OF_EFI_FILE_INFO + 256 * sizeof(CHAR16));
	if (*Len < Info->Size)
		return EFI_BUFFER_TOO_SMALL;


	CopyMem(Data, Info, Info->Size);
	*Len = Info->Size;
	File->DirPos++;

	return EFI_SUCCESS;
}

/**
 * Cache directory entries by calling NtfsReadDirectory() twice.
 *
 * @v file			EFI file
 * @v Len			Length to read
 * @v Data			Data buffer
 * @ret Status		EFI status code
 */
static EFI_STATUS
FileReadDirCache(EFI_NTFS_FILE* File)
{
	EFI_STATUS Status;

	/* First pass: Count entries and and determine max filename length */
	File->DirEntryCount = 0;
	File->DirEntrySize = 0;
	Status = NtfsReadDirectory(File, DirHookCount, File);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Directory listing pass 1 failed\n");
		return Status;
	}

	/* Sanity check since the maximum size of an NTFS file name is 255 */
	FS_ASSERT(File->DirEntrySize <= SIZE_OF_EFI_FILE_INFO + 256 * sizeof(CHAR16));

	File->DirEntry = AllocatePool(File->DirEntryCount * File->DirEntrySize);
	if (File->DirEntry == NULL) {
		Status = EFI_OUT_OF_RESOURCES;
		PrintStatusError(Status, L"Directory listing failed\n");
		return Status;
	}

	/* Second pass: Populate the directory entries */
	Status = NtfsReadDirectory(File, DirHookCache, File);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Directory listing pass 2 failed\n");
		return Status;
	}

	PrintExtra(L"Cached %d directory entries\n", File->DirEntryCount);

	/* Must reset the pos */
	File->DirPos = 0;
	return EFI_SUCCESS;
}

/**
 * Read from file
 *
 * @v This			File handle
 * @v Len			Length to read
 * @v Data			Data buffer
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileRead(EFI_FILE_HANDLE This, UINTN* Len, VOID* Data)
{
	EFI_STATUS Status;
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);

	PrintExtra(L"Read(" PERCENT_P L"|'%s', %d) %s\n", (UINTN)This, File->Path,
		*Len, File->IsDir ? L"<DIR>" : L"");

	if (File->NtfsInode == NULL)
		return EFI_DEVICE_ERROR;

	/* If this is a directory, then fetch the directory entries */
	if (File->IsDir) {
		/*
		 * For efficiency, reuse the cache if already populated.
		 * However, new entries are not listed until the dir is reopened.
		 */
		if (File->DirEntry == NULL) {
			FS_ASSERT(File->DirPos == 0);
			Status = FileReadDirCache(File);
			if (EFI_ERROR(Status))
				return Status;
		}
		return FileReturnDirEntry(File, Len, Data);
	}

	return NtfsReadFile(File, Data, Len);
}

/* Ex version */
EFI_STATUS EFIAPI
FileReadEx(IN EFI_FILE_PROTOCOL *This, IN OUT EFI_FILE_IO_TOKEN *Token)
{
	return FileRead(This, &(Token->BufferSize), Token->Buffer);
}

/**
 * Write to file
 *
 * @v This			File handle
 * @v Len			Length to write
 * @v Data			Data buffer
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileWrite(EFI_FILE_HANDLE This, UINTN* Len, VOID* Data)
{
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);

	PrintExtra(L"Write(" PERCENT_P L"|'%s', %d) %s\n", (UINTN)This, File->Path,
		*Len, File->IsDir ? L"<DIR>" : L"");

	if (File->NtfsInode == NULL)
		return EFI_DEVICE_ERROR;

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume))
		return EFI_WRITE_PROTECTED;

	/* "Writes to open directory files are not supported" */
	if (File->IsDir)
		return EFI_UNSUPPORTED;

	return NtfsWriteFile(File, Data, Len);
}

/* Ex version */
EFI_STATUS EFIAPI
FileWriteEx(IN EFI_FILE_PROTOCOL* This, EFI_FILE_IO_TOKEN* Token)
{
	return FileWrite(This, &(Token->BufferSize), Token->Buffer);
}

/**
 * Set file position
 *
 * @v This			File handle
 * @v Position		New file position
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileSetPosition(EFI_FILE_HANDLE This, UINT64 Position)
{
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);
	UINT64 FileSize;

	PrintInfo(L"SetPosition(" PERCENT_P L"|'%s', %lld) %s\n", (UINTN) This,
		File->Path, Position, (File->IsDir) ? L"<DIR>" : L"");

	if (File->NtfsInode == NULL)
		return EFI_DEVICE_ERROR;

	if (File->IsDir) {
		/* Per specs: "The only position that may be set is zero" */
		if (Position != 0)
			return EFI_UNSUPPORTED;
		File->DirPos = 0;
		return EFI_SUCCESS;
	}

	FileSize = NtfsGetFileSize(File);
	/* Per specs */
	if (Position == 0xFFFFFFFFFFFFFFFFULL)
		Position = FileSize;
	if (Position > FileSize) {
		PrintError(L"'%s': Cannot seek to #%llx of %llx\n",
				File->Path, Position, FileSize);
		return EFI_UNSUPPORTED;
	}

	/* Set position */
	File->Offset = Position;
	PrintDebug(L"'%s': Position set to %llx\n",
			File->Path, Position);

	return EFI_SUCCESS;
}

/**
 * Get file position
 *
 * @v This			File handle
 * @ret Position	New file position
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileGetPosition(EFI_FILE_HANDLE This, UINT64* Position)
{
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);

	PrintInfo(L"GetPosition(" PERCENT_P L"|'%s', %lld)\n", (UINTN) This, File->Path);

	if (File->NtfsInode == NULL)
		return EFI_DEVICE_ERROR;

	/* Per UEFI specs */
	if (File->IsDir)
		return EFI_UNSUPPORTED;

	*Position = File->Offset;
	return EFI_SUCCESS;
}

/**
 * Get file information
 *
 * @v This			File handle
 * @v Type			Type of information
 * @v Len			Buffer size
 * @v Data			Buffer
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileGetInfo(EFI_FILE_HANDLE This, EFI_GUID* Type, UINTN* Len, VOID* Data)
{
	EFI_STATUS Status;
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);
	EFI_FILE_INFO* Info = (EFI_FILE_INFO*)Data;
	EFI_FILE_SYSTEM_INFO *FSInfo = (EFI_FILE_SYSTEM_INFO*)Data;
	EFI_FILE_SYSTEM_VOLUME_LABEL *VLInfo = (EFI_FILE_SYSTEM_VOLUME_LABEL*)Data;
	UINTN Size;

	PrintInfo(L"GetInfo(" PERCENT_P L"|'%s', %d) %s\n", (UINTN) This,
		File->Path, *Len, File->IsDir ? L"<DIR>" : L"");

	if (File->NtfsInode == NULL)
		return EFI_DEVICE_ERROR;

	/* Determine information to return */
	if (CompareMem(Type, &gEfiFileInfoGuid, sizeof(*Type)) == 0) {

		PrintExtra(L"Get regular file information\n");

		Size = SafeStrSize(File->BaseName);
		FS_ASSERT(Size >= sizeof(CHAR16));
		if (*Len < SIZE_OF_EFI_FILE_INFO + Size) {
			*Len = SIZE_OF_EFI_FILE_INFO + Size;
			return EFI_BUFFER_TOO_SMALL;
		}

		/* Set the Info attributes we obtain from the path */
		ZeroMem(Data, SIZE_OF_EFI_FILE_INFO);
		Status = NtfsGetFileInfo(File, Info, 0, File->IsDir);
		if (EFI_ERROR(Status)) {
			PrintStatusError(Status, L"Could not get file info");
			return Status;
		}

		CopyMem(Info->FileName, File->BaseName, Size);
		Info->Size = SIZE_OF_EFI_FILE_INFO + Size;
		*Len = (INTN)Info->Size;
		return EFI_SUCCESS;

	} else if (CompareMem(Type, &gEfiFileSystemInfoGuid, sizeof(*Type)) == 0) {

		PrintExtra(L"Get file system information\n");

		Size = (File->FileSystem->NtfsVolumeLabel == NULL) ?
			sizeof(CHAR16) : SafeStrSize(File->FileSystem->NtfsVolumeLabel);
		if (*Len < SIZE_OF_EFI_FILE_SYSTEM_INFO + Size) {
			*Len = SIZE_OF_EFI_FILE_SYSTEM_INFO + Size;
			return EFI_BUFFER_TOO_SMALL;
		}

		ZeroMem(Data, SIZE_OF_EFI_FILE_SYSTEM_INFO + sizeof(CHAR16));
		FSInfo->Size = SIZE_OF_EFI_FILE_SYSTEM_INFO;
		FSInfo->ReadOnly = NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume);

		/* NB: This should really be cluster size, but we don't have access to that */
		if (File->FileSystem->BlockIo2 != NULL)
			FSInfo->BlockSize = File->FileSystem->BlockIo2->Media->BlockSize;
		else
			FSInfo->BlockSize = File->FileSystem->BlockIo->Media->BlockSize;
		if (FSInfo->BlockSize == 0) {
			PrintWarning(L"Corrected Media BlockSize\n");
			FSInfo->BlockSize = 512;
		}

		if (File->FileSystem->BlockIo2 != NULL) {
			FSInfo->VolumeSize = (File->FileSystem->BlockIo2->Media->LastBlock + 1) *
				FSInfo->BlockSize;
		} else {
			FSInfo->VolumeSize = (File->FileSystem->BlockIo->Media->LastBlock + 1) *
				FSInfo->BlockSize;
		}

		FSInfo->FreeSpace = NtfsGetVolumeFreeSpace(File->FileSystem->NtfsVolume);

		/* NUL string has already been populated if NtfsVolumeLabel is NULL */
		if (File->FileSystem->NtfsVolumeLabel != NULL)
			CopyMem(FSInfo->VolumeLabel, File->FileSystem->NtfsVolumeLabel, Size);
		FSInfo->Size = SIZE_OF_EFI_FILE_SYSTEM_INFO + Size;
		*Len = (INTN)FSInfo->Size;
		return EFI_SUCCESS;

	} else if (CompareMem(Type, &gEfiFileSystemVolumeLabelInfoIdGuid, sizeof(*Type)) == 0) {

		PrintExtra(L"Get volume label\n");

		/* Per specs, only valid for root */
		if (!File->IsRoot)
			return EFI_ACCESS_DENIED;

		if (*Len < sizeof(CHAR16))
			return EFI_BUFFER_TOO_SMALL;

		Size = (File->FileSystem->NtfsVolumeLabel == NULL) ?
			sizeof(CHAR16) : SafeStrSize(File->FileSystem->NtfsVolumeLabel);
		if (Size < *Len) {
			*Len = Size;
			return EFI_BUFFER_TOO_SMALL;
		}

		if (File->FileSystem->NtfsVolumeLabel != NULL)
			CopyMem(VLInfo->VolumeLabel, File->FileSystem->NtfsVolumeLabel, Size);
		else
			VLInfo->VolumeLabel[0] = 0;
		*Len = Size;
		return EFI_SUCCESS;

	} else {

		PrintError(L"'%s': Cannot get information of type %s\n", File->Path, GuidToStr(Type));
		return EFI_UNSUPPORTED;

	}
}

/**
 * Set file information
 *
 * @v This			File handle
 * @v Type			Type of information
 * @v Len			Buffer size
 * @v Data			Buffer
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileSetInfo(EFI_FILE_HANDLE This, EFI_GUID* Type, UINTN Len, VOID* Data)
{
	EFI_STATUS Status;
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);
	EFI_FILE_INFO* Info = (EFI_FILE_INFO*)Data;
	EFI_FILE_SYSTEM_INFO* FSInfo = (EFI_FILE_SYSTEM_INFO*)Data;
	EFI_FILE_SYSTEM_VOLUME_LABEL* VLInfo = (EFI_FILE_SYSTEM_VOLUME_LABEL*)Data;

	PrintInfo(L"SetInfo(" PERCENT_P L"|'%s', %d) %s\n", (UINTN)This,
		File->Path, Len, File->IsDir ? L"<DIR>" : L"");

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume))
		return EFI_WRITE_PROTECTED;

	if (File->NtfsInode == NULL)
		return EFI_DEVICE_ERROR;

	if (CompareMem(Type, &gEfiFileInfoGuid, sizeof(*Type)) == 0) {
		PrintExtra(L"Set regular file information\n");
		if ((Len < SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16)) ||
			(StrSize(Info->FileName) > Len - SIZE_OF_EFI_FILE_INFO))
			return EFI_BAD_BUFFER_SIZE;
		Status = NtfsSetFileInfo(File, Info);
		if (EFI_ERROR(Status))
			PrintStatusError(Status, L"Could not set file info");
		return Status;
	} else if (CompareMem(Type, &gEfiFileSystemInfoGuid, sizeof(*Type)) == 0) {
		PrintExtra(L"Set volume label (FS)\n");
		if (!File->IsRoot)
			return EFI_ACCESS_DENIED;
		if ((Len < SIZE_OF_EFI_FILE_SYSTEM_INFO + sizeof(CHAR16)) ||
			(StrSize(FSInfo->VolumeLabel) > Len - SIZE_OF_EFI_FILE_SYSTEM_INFO))
			return EFI_BAD_BUFFER_SIZE;
		return NtfsRenameVolume(File->FileSystem->NtfsVolume,
			FSInfo->VolumeLabel, (Len - SIZE_OF_EFI_FILE_SYSTEM_INFO) / sizeof(CHAR16));
	} else if (CompareMem(Type, &gEfiFileSystemVolumeLabelInfoIdGuid, sizeof(*Type)) == 0) {
		PrintExtra(L"Set volume label (VL)\n");
		if (!File->IsRoot)
			return EFI_ACCESS_DENIED;
		if (Len < sizeof(CHAR16) || StrSize(VLInfo->VolumeLabel) > Len)
			return EFI_BAD_BUFFER_SIZE;
		return NtfsRenameVolume(File->FileSystem->NtfsVolume,
			VLInfo->VolumeLabel, Len / sizeof(CHAR16));
	} else {
		PrintError(L"'%s': Cannot set information of type %s", File->Path, GuidToStr(Type));
		return EFI_UNSUPPORTED;
	}
}

/**
 * Flush file modified data
 *
 * @v This			File handle
 * @v Type			Type of information
 * @v Len			Buffer size
 * @v Data			Buffer
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileFlush(EFI_FILE_HANDLE This)
{
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);

	PrintInfo(L"Flush(" PERCENT_P L"|'%s')\n", (UINTN) This, File->Path);

	if (File->NtfsInode == NULL)
		return EFI_DEVICE_ERROR;

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume))
		return EFI_SUCCESS;

	return NtfsFlushFile(File);
}

/* Ex version */
EFI_STATUS EFIAPI
FileFlushEx(EFI_FILE_HANDLE This, EFI_FILE_IO_TOKEN* Token)
{
	return FileFlush(This);
}

/**
 * Open the volume and return a handle to the root directory
 *
 * Note that, because we are working in an environment that can be
 * shut down without notice, we want the volume to remain mounted
 * for as little time as possible so that the user doesn't end up
 * with the dreaded "unclean NTFS volume" after restart.
 * In order to accomplish that, we keep a total reference count of
 * all the files open on volume (in EFI_FS's TotalRefCount), which
 * gets updated during Open() and Close() operations.
 * When that number reaches zero, we unmount the NTFS volume.
 *
 * Obviously, this constant mounting and unmounting of the volume
 * does have an effect on performance (and shouldn't really be
 * necessary if the volume is mounted read-only), but it is the
 * one way we have to try to preserve file system integrity on a
 * system that users may just shut down by yanking a power cable.
 *
 * @v This			EFI simple file system
 * @ret Root		File handle for the root directory
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* This, EFI_FILE_HANDLE* Root)
{
	EFI_STATUS Status;
	EFI_NTFS_FILE* RootFile = NULL;
	EFI_FS* FSInstance = BASE_CR(This, EFI_FS, FileIoInterface);

	PrintInfo(L"OpenVolume: %s\n", FSInstance->DevicePathString);

	/* Mount the NTFS volume */
	Status = NtfsMountVolume(FSInstance);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not mount NTFS volume");
		goto out;
	}

	/* Create the root file */
	Status = NtfsAllocateFile(&RootFile, FSInstance);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not create root file");
		goto out;
	}

	/* Setup the root path */
	RootFile->Path = AllocateZeroPool(2 * sizeof(CHAR16));
	if (RootFile->Path == NULL) {
		Status = EFI_OUT_OF_RESOURCES;
		PrintStatusError(Status, L"Could not allocate root file name");
		goto out;
	}
	RootFile->Path[0] = PATH_CHAR;
	RootFile->BaseName = &RootFile->Path[1];

	/* Open the root file */
	Status = NtfsOpenFile(&RootFile);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not open root file");
		goto out;
	}

	/* Increase RefCounts (which should NOT expected to be 0) */
	RootFile->RefCount++;
	FSInstance->TotalRefCount++;
	PrintExtra(L"TotalRefCount = %d\n", FSInstance->TotalRefCount);

	/* Return the root handle */
	*Root = (EFI_FILE_HANDLE)RootFile;
	Status = EFI_SUCCESS;

out:
	if (EFI_ERROR(Status)) {
		NtfsCloseFile(RootFile);
		NtfsFreeFile(RootFile);
		NtfsUnmountVolume(FSInstance);
	}
	return Status;
}

/**
 * Install the EFI simple file system protocol
 * If successful this call instantiates a new FS#: drive, that is made
 * available on the next 'map -r'. Note that all this call does is add
 * the FS protocol. OpenVolume won't be called until a process tries
 * to access a file or the root directory on the volume.
 */
EFI_STATUS
FSInstall(EFI_FS* This, EFI_HANDLE ControllerHandle)
{
	const CHAR8 NtfsMagic[8] = { 'N', 'T', 'F', 'S', ' ', ' ', ' ', ' ' };
	EFI_STATUS Status;
	CHAR8* Buffer;

	/*
	 * Check if it's a filesystem we can handle by reading the first block
	 * of the volume and looking for the NTFS magic in the OEM ID.
	 */
	Buffer = (CHAR8*)AllocateZeroPool(This->BlockIo->Media->BlockSize);
	if (Buffer == NULL)
		return EFI_OUT_OF_RESOURCES;
	Status = This->BlockIo->ReadBlocks(This->BlockIo, This->BlockIo->Media->MediaId,
			0, This->BlockIo->Media->BlockSize, Buffer);
	if (!EFI_ERROR(Status))
		Status = CompareMem(&Buffer[3], NtfsMagic, sizeof(NtfsMagic)) ? EFI_UNSUPPORTED : EFI_SUCCESS;
	FreePool(Buffer);
	if (EFI_ERROR(Status))
		return Status;

	PrintInfo(L"FSInstall: %s\n", This->DevicePathString);

	/* Install the simple file system protocol. */
	Status = gBS->InstallMultipleProtocolInterfaces(&ControllerHandle,
		&gEfiSimpleFileSystemProtocolGuid, &This->FileIoInterface,
		NULL);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not install simple file system protocol");
		return Status;
	}

	InitializeListHead(&FsListHead);

	return EFI_SUCCESS;
}

/* Uninstall EFI simple file system protocol */
VOID
FSUninstall(EFI_FS* This, EFI_HANDLE ControllerHandle)
{
	PrintInfo(L"FSUninstall: %s\n", This->DevicePathString);

	if (This->TotalRefCount > 0) {
		PrintWarning(L"Files are still open on this volume! Forcing unmount...\n");
		NtfsUnmountVolume(This);
	}

	gBS->UninstallMultipleProtocolInterfaces(ControllerHandle,
		&gEfiSimpleFileSystemProtocolGuid, &This->FileIoInterface,
		NULL);
}

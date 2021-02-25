/* file.c - SimpleFileIo Interface */
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

#include "driver.h"
#include "bridge.h"
#include "uefi_logging.h"
#include "uefi_support.h"

/* Structure used with DirHook */
typedef struct {
	INTN           Index;
	EFI_NTFS_FILE* Parent;
	EFI_FILE_INFO* Info;
} DIR_DATA;

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

	PrintInfo(L"Open(" PERCENT_P L"%s, \"%s\")\n", (UINTN)This,
		File->IsRoot ? L" <ROOT>" : L"", Name);

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume) &&  Mode != EFI_FILE_MODE_READ) {
		PrintWarning(L"File '%s' can only be opened in read-only mode\n", Name);
		return EFI_WRITE_PROTECTED;
	}

	/* Additional failures */
	if ((StrCmp(Name, L"..") == 0) && File->IsRoot) {
		PrintInfo(L"Trying to open <ROOT>'s parent\n");
		return EFI_NOT_FOUND;
	}

	/* See if we're trying to reopen current (which the EFI Shell insists on doing) */
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
		if (Path[i] == L'\\')
			Path[i] = PATH_CHAR;
	}

	/* Clean the path by removing double delimiters and processing '.' and '..' */
	CleanPath(Path);

	/* Validate that our paths are non-empty and absolute */
	FS_ASSERT(Path[0] == PATH_CHAR);

	/* Allocate and initialise an instance of a file */
	Status = NtfsCreateFile(&NewFile, File->FileSystem);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not instantiate file");
		goto out;
	}

	/* See if we're dealing with the root */
	if (Path[0] == 0 || (Path[0] == PATH_CHAR && Path[1] == 0)) {
		PrintInfo(L"  Reopening <ROOT>\n");
		NewFile->IsRoot = TRUE;
	}

	NewFile->Path = Path;
	/* Avoid double free on error */
	Path = NULL;

	/* Set basename */
	for (i = SafeStrLen(NewFile->Path) - 1; i >= 0; i--) {
		if (NewFile->Path[i] == PATH_CHAR)
			break;
	}
	NewFile->Basename = &NewFile->Path[i + 1];

	Status = NtfsOpen(NewFile);
	if (EFI_ERROR(Status)) {
		if (Status != EFI_NOT_FOUND)
			PrintStatusError(Status, L"Could not open file '%s'", Name);
		goto out;
	}

	NewFile->RefCount++;
	File->FileSystem->TotalRefCount++;
	PrintExtra(L"TotalRefCount = %d\n", File->FileSystem->TotalRefCount);
	*New = &NewFile->EfiFile;

	PrintInfo(L"  RET: " PERCENT_P L"\n", (UINTN)*New);
	Status = EFI_SUCCESS;

out:
	if (EFI_ERROR(Status))
		NtfsDestroyFile(NewFile);
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
		NtfsClose(File);
		/* NB: Basename points into File->Path and does not need to be freed */
		NtfsDestroyFile(File);
	}

	/* If there are no more files open on the volume, unmount it */
	FileSystem->TotalRefCount--;
	PrintExtra(L"TotalRefCount = %d\n", FileSystem->TotalRefCount);
	if (FileSystem->TotalRefCount <= 0) {
		PrintInfo(L"Last file instance: Unmounting volume\n");
		NtfsUnmount(FileSystem);
	}

	return EFI_SUCCESS;
}

/**
 * Close and delete file
 *
 * @v This			File handle
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileDelete(EFI_FILE_HANDLE This)
{
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);

	/* Close file */
	FileClose(This);

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume)) {
		PrintError(L"Cannot delete '%s'\n", File->Path);
		return EFI_WRITE_PROTECTED;
	}

	return NtfsDeleteFile(File);
}

/**
 * Process directory entries
 */
static INT32 DirHook(VOID* Data, CONST CHAR16* Name,
	CONST INT32 NameLen, CONST INT32 NameType, CONST INT64 Pos,
	CONST UINT64 MRef, CONST UINT32 DtType)
{
	EFI_STATUS Status;
	DIR_DATA* HookData = (DIR_DATA*)Data;
	UINTN Len, MaxLen;

	/* Never process inodes 0 and 1 */
	if (GetInodeNumber(MRef) <= 1)
		return 0;

	/* Eliminate '.' or '..' */
	if ((Name[0] ==  '.') && ((NameLen == 1) || ((NameLen == 2) && (Name[1] == '.'))))
		return 0;

	/* Ignore any entry that doesn't match our index */
	if ((HookData->Index)-- != 0)
		return 0;

	/* Set the Info attributes we obtain from this function's parameters */
	FS_ASSERT(HookData->Info->Size > sizeof(EFI_FILE_INFO));
	MaxLen = (UINTN)(HookData->Info->Size - sizeof(EFI_FILE_INFO)) / sizeof(CHAR16);
	SafeStrCpy(HookData->Info->FileName, MaxLen, Name);
	Len = MIN((UINTN)NameLen, MaxLen);
	HookData->Info->FileName[Len] = 0;
	/* The Info struct size already accounts for the extra NUL */
	HookData->Info->Size = sizeof(EFI_FILE_INFO) + (UINT64)Len * sizeof(CHAR16);

	/* Set the Info attributes we obtain from the inode */
	Status = NtfsGetInfo(HookData->Info, HookData->Parent->FileSystem->NtfsVolume,
		NULL, MRef, (DtType == 4));	/* DtType is 4 for directories */
	if (EFI_ERROR(Status))
		PrintStatusError(Status, L"Could not set directory entry info");

	return 0;
}

/**
 * Read directory entry
 *
 * @v file			EFI file
 * @v Len			Length to read
 * @v Data			Data buffer
 * @ret Status		EFI status code
 */
static EFI_STATUS
FileReadDir(EFI_NTFS_FILE* File, UINTN* Len, VOID* Data)
{
	DIR_DATA HookData = { File->DirIndex, File, (EFI_FILE_INFO*)Data };
	EFI_STATUS Status;
	INTN PathLen, MaxPathLen;

	/* Populate our Info template */
	ZeroMem(HookData.Info, *Len);
	HookData.Info->Size = *Len;
	PathLen = SafeStrLen(File->Path);
	MaxPathLen = (*Len - sizeof(EFI_FILE_INFO)) / sizeof(CHAR16);
	if (PathLen > MaxPathLen) {
		/* TODO: Might have to proceed anyway as it doesn't look like all */
		/* UEFI Shells retry with a larger buffer on EFI_BUFFER_TOO_SMALL */
		PrintWarning(L"Path is too long for Readdir\n");
		return EFI_BUFFER_TOO_SMALL;
	}
	SafeStrCpy(HookData.Info->FileName, MaxPathLen, File->Path);
	if (HookData.Info->FileName[PathLen - 1] != PATH_CHAR)
		HookData.Info->FileName[PathLen++] = PATH_CHAR;

	/* Call ReadDir(), which calls into DirHook() for each entry */
	Status = NtfsReadDir(File, DirHook, &HookData);
	if (HookData.Index >= 0) {
		/* No more entries */
		*Len = 0;
		return EFI_SUCCESS;
	}

	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Directory listing failed");
		return Status;
	}

	*Len = (UINTN) HookData.Info->Size;
	/* Advance to the next entry */
	File->DirIndex++;

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
	EFI_NTFS_FILE* File = BASE_CR(This, EFI_NTFS_FILE, EfiFile);

	PrintInfo(L"Read(" PERCENT_P L"|'%s', %d) %s\n", (UINTN)This, File->Path,
		*Len, File->IsDir ? L"<DIR>" : L"");

	/* If this is a directory, then fetch the directory entries */
	if (File->IsDir)
		return FileReadDir(File, Len, Data);

	return NtfsRead(File, Data, Len);
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

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume))
		return EFI_WRITE_PROTECTED;

	/* "Writes to open directory files are not supported" */
	if (File->IsDir)
		return EFI_UNSUPPORTED;

	return NtfsWrite(File, Data, Len);
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

	/* If this is a directory, reset the Index to the start */
	if (File->IsDir) {
		if (Position != 0)
			return EFI_INVALID_PARAMETER;
		File->DirIndex = 0;
		return EFI_SUCCESS;
	}

	FileSize = NtfsGetFileSize(File);
	if (Position > FileSize) {
		PrintError(L"'%s': Cannot seek to #%llx of %llx\n",
				File->Path, Position, FileSize);
		return EFI_UNSUPPORTED;
	}

	/* Set position */
	NtfsSetFileOffset(File, Position);
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

	if (File->IsDir)
		*Position = File->DirIndex;
	else
		*Position = NtfsGetFileOffset(File);
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
	EFI_FILE_SYSTEM_INFO *FSInfo = (EFI_FILE_SYSTEM_INFO*)Data;
	EFI_FILE_INFO* Info = (EFI_FILE_INFO*)Data;
	EFI_FILE_SYSTEM_VOLUME_LABEL *VLInfo = (EFI_FILE_SYSTEM_VOLUME_LABEL*)Data;
	UINTN MaxLen;

	PrintInfo(L"GetInfo(" PERCENT_P L"|'%s', %d) %s\n", (UINTN) This,
		File->Path, *Len, File->IsDir ? L"<DIR>" : L"");

	/* Determine information to return */
	if (CompareMem(Type, &gEfiFileInfoGuid, sizeof(*Type)) == 0) {

		/* Fill file information */
		PrintExtra(L"Get regular file information\n");
		if (*Len < MINIMUM_INFO_LENGTH) {
			*Len = MINIMUM_INFO_LENGTH;
			return EFI_BUFFER_TOO_SMALL;
		}

		ZeroMem(Data, sizeof(EFI_FILE_INFO));
		Info->Size = *Len;

		/* Set the Info attributes we obtain from the path */
		Status = NtfsGetInfo(Info, File->FileSystem->NtfsVolume, File->Path,
			0, File->IsDir);
		if (EFI_ERROR(Status)) {
			PrintStatusError(Status, L"Could not set file info");
			return Status;
		}

		/* The Info struct size accounts for the NUL string terminator */
		MaxLen = (UINTN)(Info->Size - sizeof(EFI_FILE_INFO)) / sizeof(CHAR16);
		SafeStrCpy(Info->FileName, MaxLen, File->Basename);
		Info->Size = sizeof(EFI_FILE_INFO) + (UINT64)MaxLen * sizeof(CHAR16);
		*Len = (INTN)Info->Size;
		return EFI_SUCCESS;

	} else if (CompareMem(Type, &gEfiFileSystemInfoGuid, sizeof(*Type)) == 0) {

		/* Get file system information */
		PrintExtra(L"Get file system information\n");
		if (*Len < MINIMUM_FS_INFO_LENGTH) {
			*Len = MINIMUM_FS_INFO_LENGTH;
			return EFI_BUFFER_TOO_SMALL;
		}

		ZeroMem(Data, sizeof(EFI_FILE_INFO));
		FSInfo->Size = *Len;
		FSInfo->ReadOnly = NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume);
		/* NB: This should really be cluster size, but we don't have access to that */
		if (File->FileSystem->BlockIo2 != NULL) {
			FSInfo->BlockSize = File->FileSystem->BlockIo2->Media->BlockSize;
		} else {
			FSInfo->BlockSize = File->FileSystem->BlockIo->Media->BlockSize;
		}
		if (FSInfo->BlockSize  == 0) {
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

		if (File->FileSystem->NtfsVolumeLabel == NULL) {
			FSInfo->VolumeLabel[0] = 0;
			*Len = sizeof(EFI_FILE_SYSTEM_INFO);
		} else {
			/* The Info struct size accounts for the NUL string terminator */
			MaxLen = (INTN)(FSInfo->Size - sizeof(EFI_FILE_SYSTEM_INFO)) / sizeof(CHAR16);
			SafeStrCpy(FSInfo->VolumeLabel, MaxLen, File->FileSystem->NtfsVolumeLabel);
			FSInfo->Size = sizeof(EFI_FILE_SYSTEM_INFO) + (UINT64)MaxLen * sizeof(CHAR16);
			*Len = (INTN)FSInfo->Size;
		}
		return EFI_SUCCESS;

	} else if (CompareMem(Type, &gEfiFileSystemVolumeLabelInfoIdGuid, sizeof(*Type)) == 0) {

		/* Get the volume label */
		if (File->FileSystem->NtfsVolumeLabel != NULL)
			SafeStrCpy(VLInfo->VolumeLabel, *Len / sizeof(CHAR16), File->FileSystem->NtfsVolumeLabel);
		else
			VLInfo->VolumeLabel[0] = 0;
		return EFI_SUCCESS;

	} else {

		Print(L"'%s': Cannot get information of type ", File->Path);
		PrintGuid(Type);
		Print(L"\n");
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
	EFI_FILE_SYSTEM_VOLUME_LABEL* VLInfo = (EFI_FILE_SYSTEM_VOLUME_LABEL*)Data;

	PrintInfo(L"SetInfo(" PERCENT_P L"|'%s', %d) %s\n", (UINTN)This,
		File->Path, Len, File->IsDir ? L"<DIR>" : L"");

	if (NtfsIsVolumeReadOnly(File->FileSystem->NtfsVolume))
		return EFI_WRITE_PROTECTED;

	if (CompareMem(Type, &gEfiFileInfoGuid, sizeof(*Type)) == 0) {
		PrintExtra(L"Set regular file information\n");
		Status = NtfsSetInfo(Info, File->FileSystem->NtfsVolume, File->Path);
		if (EFI_ERROR(Status)) 
			PrintStatusError(Status, L"Could not set file info");
		return Status;
	} else if (CompareMem(Type, &gEfiFileSystemVolumeLabelInfoIdGuid, sizeof(*Type)) == 0) {
		PrintExtra(L"Set volume label\n");
		return NtfsRenameVolume(File->FileSystem->NtfsVolume,
			VLInfo->VolumeLabel, Len / sizeof(CHAR16));
	} else {
		Print(L"'%s': Cannot set information of type ", File->Path);
		PrintGuid(Type);
		Print(L"\n");
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
	Status = NtfsMount(FSInstance);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not mount NTFS volume");
		goto out;
	}

	/* Create the root file */
	Status = NtfsCreateFile(&RootFile, FSInstance);
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
	RootFile->Basename = &RootFile->Path[1];

	/* Open the root file */
	Status = NtfsOpen(RootFile);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not open root file");
		goto out;
	}

	/* Set the inital refcounts for both the file and the file system */
	RootFile->RefCount = 1;
	FSInstance->TotalRefCount++;
	PrintExtra(L"TotalRefCount = %d\n", FSInstance->TotalRefCount);

	/* Return the root handle */
	*Root = (EFI_FILE_HANDLE)RootFile;
	Status = EFI_SUCCESS;

out:
	if (EFI_ERROR(Status)) {
		NtfsClose(RootFile);
		NtfsDestroyFile(RootFile);
		NtfsUnmount(FSInstance);
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
	EFI_STATUS Status;

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
		NtfsUnmount(This);
	}

	gBS->UninstallMultipleProtocolInterfaces(ControllerHandle,
		&gEfiSimpleFileSystemProtocolGuid, &This->FileIoInterface,
		NULL);
}

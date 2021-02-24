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

#ifdef FORCE_READONLY
	if (Mode != EFI_FILE_MODE_READ) {
		PrintWarning(L"File '%s' can only be opened in read-only mode\n", Name);
		return EFI_WRITE_PROTECTED;
	}
#endif

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

	Path = AllocatePool(PATH_MAX * sizeof(CHAR16));
	if (Path == NULL) {
		PrintError(L"Could not allocate path\n");
		Status = EFI_OUT_OF_RESOURCES;
		goto out;
	}

	/* If we have an absolute path, don't bother completing with the parent */
	if (IS_PATH_DELIMITER(Name[0])) {
		Len = 0;
	} else {
		Path[0] = PATH_CHAR;
		SafeStrCpy(&Path[1], PATH_MAX - 1, File->Path);
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

#ifdef FORCE_READONLY
	PrintError(L"Cannot delete '%s'\n", File->Path);
#else
	/* TODO: unlink file */
#endif

	/* Warn of failure to delete */
	return EFI_WARN_DELETE_FAILURE;
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
	return EFI_UNSUPPORTED;
}

/* Ex version */
EFI_STATUS EFIAPI
FileReadEx(IN EFI_FILE_PROTOCOL *This, IN OUT EFI_FILE_IO_TOKEN *Token)
{
	return EFI_UNSUPPORTED;
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
	return EFI_UNSUPPORTED;
}

/* Ex version */
EFI_STATUS EFIAPI
FileWriteEx(IN EFI_FILE_PROTOCOL* This, EFI_FILE_IO_TOKEN* Token)
{
	return EFI_UNSUPPORTED;
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
	return EFI_UNSUPPORTED;
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
	return EFI_UNSUPPORTED;
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
	return EFI_UNSUPPORTED;
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
	return EFI_UNSUPPORTED;
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
	return EFI_UNSUPPORTED;
}

/* Ex version */
EFI_STATUS EFIAPI
FileFlushEx(EFI_FILE_HANDLE This, EFI_FILE_IO_TOKEN* Token)
{
	return EFI_UNSUPPORTED;
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

	/* Open the root file (a NULL path opens root) */
	Status = NtfsOpen(RootFile);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not open root file");
		goto out;
	}

	/* Now setup the actual root path */
	RootFile->Path = AllocateZeroPool(2 * sizeof(CHAR16));
	if (RootFile->Path == NULL) {
		Status = EFI_OUT_OF_RESOURCES;
		PrintStatusError(Status, L"Could not allocate root file name");
		goto out;
	}
	RootFile->Path[0] = PATH_CHAR;
	RootFile->Basename = &RootFile->Path[1];

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

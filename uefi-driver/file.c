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
 * Open root directory
 *
 * @v This			EFI simple file system
 * @ret Root		File handle for the root directory
 * @ret Status		EFI status code
 */
EFI_STATUS EFIAPI
FileOpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* This, EFI_FILE_HANDLE* Root)
{
	EFI_FS* FSInstance = BASE_CR(This, EFI_FS, FileIoInterface);

	PrintInfo(L"OpenVolume\n");
	*Root = &FSInstance->RootFile->EfiFile;

	return EFI_SUCCESS;
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

	/* TODO: Call on ntfs_mount() */

	/* Initialize the root handle */
	This->RootFile = NULL;
	Status = NtfsCreateFile(&This->RootFile, This);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not create root file");
		return Status;
	}

	/* Setup the EFI part */
	This->RootFile->EfiFile.Revision = EFI_FILE_PROTOCOL_REVISION2;

	/* TODO: Setup the UEFI file ops */

	/* Setup the other attributes */
	This->RootFile->Path = AllocateZeroPool(2 * sizeof(CHAR16));
	if (This->RootFile->Path == NULL) {
		Status = EFI_OUT_OF_RESOURCES;
		PrintStatusError(Status, L"Could not allocate root file name");
		NtfsDestroyFile(This->RootFile);
		return Status;
	}
	This->RootFile->Path[0] = PATH_CHAR;
	This->RootFile->Basename = &This->RootFile->Path[1];
	This->RootFile->IsDir = TRUE;

	/* Install the simple file system protocol. */
	Status = gBS->InstallMultipleProtocolInterfaces(&ControllerHandle,
		&gEfiSimpleFileSystemProtocolGuid, &This->FileIoInterface,
		NULL);
	if (EFI_ERROR(Status)) {
		PrintStatusError(Status, L"Could not install simple file system protocol");
		NtfsDestroyFile(This->RootFile);
		return Status;
	}

	return EFI_SUCCESS;
}

/* Uninstall EFI simple file system protocol */
VOID
FSUninstall(EFI_FS* This, EFI_HANDLE ControllerHandle)
{
	PrintInfo(L"FSUninstall: %s\n", This->DevicePathString);

	gBS->UninstallMultipleProtocolInterfaces(ControllerHandle,
		&gEfiSimpleFileSystemProtocolGuid, &This->FileIoInterface,
		NULL);

	/* TODO: Call on ntfs_umount() */
}

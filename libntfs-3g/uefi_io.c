/**
 * uefi_io.c - UEFI disk io functions. Originated from the Linux-NTFS project.
 *
 * Copyright (c) 2000-2006 Anton Altaparmakov
 * Copyright (c) 2021      Pete Batard
 *
 * This program/include file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program/include file is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (in the main directory of the NTFS-3G
 * distribution in the file COPYING); if not, write to the Free Software
 * Foundation,Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>

#include "types.h"
#include "device.h"
#include "logging.h"
#include "compat.h"
#include "unistr.h"
#include "uefi_support.h"

/**
 * ntfs_device_uefi_io_open: For UEFI drivers, there isn't much to
 * do in terms of initializing a device, because by the time we get
 * to this call, we should be able to use the Device Path to locate
 * an existing EFI_FS instance with the UEFI Block and Disk devices
 * we need.
 */
static int ntfs_device_uefi_io_open(struct ntfs_device *dev, int flags)
{
	CHAR16* DevName = NULL;
	EFI_FS* FileSystem;

	if (NDevOpen(dev)) {
		errno = EBUSY;
		return -1;
	}

	/* All the devices we access are block devices */
	NDevSetBlock(dev);
	dev->d_private = NULL;
	
	/*
	 * Use the Device Path to locate an pre-existing instance from
	 * the global EFI_FS list we maintain.
	 */
	ntfs_mbstoucs(dev->d_name, &DevName);
	for (FileSystem = (EFI_FS*)FORWARD_LINK_REF(FsListHead);
		FileSystem != (EFI_FS*)&FsListHead;
		FileSystem = (EFI_FS*)FileSystem->Flink) {
		if (StrCmp(FileSystem->DevicePathString, DevName) == 0)
			break;
	}
	free(DevName);
	if (FileSystem == (EFI_FS*)&FsListHead) {
		errno = ENODEV;
		return -1;
	}

	FileSystem->Offset = 0;
	dev->d_private = FileSystem;
	if ((flags & O_RDWR) != O_RDWR)
		NDevSetReadOnly(dev);
	NDevSetOpen(dev);

	return 0;
}

/**
 * ntfs_device_uefi_io_sync - Flush any buffered changes to the device
 */
static int ntfs_device_uefi_io_sync(struct ntfs_device* dev)
{
	EFI_STATUS Status;
	EFI_FS* FileSystem = (EFI_FS*)dev->d_private;

	FS_ASSERT(FileSystem != NULL);
	FS_ASSERT(FileSystem->DiskIo != NULL);
	FS_ASSERT(FileSystem->BlockIo != NULL);

	if (!NDevReadOnly(dev)) {
		/* We only support flush for DiskIo2 type devices */
		if (FileSystem->DiskIo2 == NULL) {
			errno = ENOSYS;
			return -1;
		}
		Status = FileSystem->DiskIo2->FlushDiskEx(FileSystem->DiskIo2,
			&(FileSystem->DiskIo2Token));
		if (EFI_ERROR(Status)) {
			errno = EIO;
			return -1;
		}
		NDevClearDirty(dev);
	}
	return 0;
}

/**
 * ntfs_device_uefi_io_close - Close the device
 */
static int ntfs_device_uefi_io_close(struct ntfs_device *dev)
{
	if (!NDevOpen(dev)) {
		errno = EBADF;
		ntfs_log_perror("Device is not open");
		return -1;
	}
	if (NDevDirty(dev))
		if (ntfs_device_uefi_io_sync(dev)) {
			ntfs_log_perror("Failed to sync device");
			return -1;
		}

	NDevClearOpen(dev);
	dev->d_private = NULL;

	return 0;
}

/**
 * ntfs_device_uefi_io_seek - Seek to a place on the device
 */
static s64 ntfs_device_uefi_io_seek(struct ntfs_device* dev, s64 offset,
	int whence)
{
	EFI_FS* FileSystem = (EFI_FS*)dev->d_private;
	FS_ASSERT(FileSystem != NULL);

	/* TODO: Support SEEK_END and check for overflow */
	switch (whence) {
	case SEEK_SET:
		FileSystem->Offset = offset;
		return offset;
	case SEEK_CUR:
		if (FileSystem->Offset + offset < 0) {
			errno = EINVAL;
			return -1;
		}
		FileSystem->Offset += offset;
		return offset;
	default:
		ntfs_log_perror("Seek option is not implemented\n");
		errno = EINVAL;
		return -1;
	}
}

/**
 * ntfs_device_uefi_io_pread - Perform a positioned read from the device
 */
static s64 ntfs_device_uefi_io_pread(struct ntfs_device *dev, void *buf,
		s64 count, s64 offset)
{
	EFI_STATUS Status;
	EFI_FS* FileSystem = (EFI_FS*)dev->d_private;
	EFI_BLOCK_IO_MEDIA* Media;

	FS_ASSERT(FileSystem != NULL);
	FS_ASSERT(FileSystem->DiskIo != NULL);
	FS_ASSERT(FileSystem->BlockIo != NULL);
	FS_ASSERT(count > 0);
	FS_ASSERT(offset >= 0);

	if (FileSystem->BlockIo2 != NULL)
		Media = FileSystem->BlockIo2->Media;
	else
		Media = FileSystem->BlockIo->Media;

	if (FileSystem->DiskIo2 != NULL)
		Status = FileSystem->DiskIo2->ReadDiskEx(FileSystem->DiskIo2, Media->MediaId,
			offset, &(FileSystem->DiskIo2Token), count, buf);
	else
		Status = FileSystem->DiskIo->ReadDisk(FileSystem->DiskIo, Media->MediaId,
			offset, (UINTN)count, buf);

	if (EFI_ERROR(Status)) {
		ntfs_log_perror("Failed to read data at address %08llx", offset);
		errno = EIO;
		return -1;
	}

	FileSystem->Offset += count;
	return count;
}

/**
 * ntfs_device_uefi_io_pwrite - Perform a positioned write to the device
 */
static s64 ntfs_device_uefi_io_pwrite(struct ntfs_device *dev, const void *buf,
		s64 count, s64 offset)
{
	EFI_STATUS Status;
	EFI_FS* FileSystem = (EFI_FS*)dev->d_private;
	EFI_BLOCK_IO_MEDIA* Media;

	FS_ASSERT(FileSystem != NULL);
	FS_ASSERT(FileSystem->DiskIo != NULL);
	FS_ASSERT(FileSystem->BlockIo != NULL);
	FS_ASSERT(count > 0);
	FS_ASSERT(offset >= 0);

	if (NDevReadOnly(dev)) {
		errno = EROFS;
		return -1;
	}
	NDevSetDirty(dev);

	if (FileSystem->BlockIo2 != NULL)
		Media = FileSystem->BlockIo2->Media;
	else
		Media = FileSystem->BlockIo->Media;

	if (FileSystem->DiskIo2 != NULL)
		Status = FileSystem->DiskIo2->WriteDiskEx(FileSystem->DiskIo2, Media->MediaId,
			offset, &(FileSystem->DiskIo2Token), count, (VOID*)buf);
	else
		Status = FileSystem->DiskIo->WriteDisk(FileSystem->DiskIo, Media->MediaId,
			offset, (UINTN)count, (VOID*)buf);

	if (EFI_ERROR(Status)) {
		ntfs_log_perror("Failed to write data at address %08llx", offset);
		errno = EIO;
		return -1;
	}

	FileSystem->Offset += count;
	return count;
}

/**
 * ntfs_device_uefi_io_read - Read from the device, from the current location
 */
static s64 ntfs_device_uefi_io_read(struct ntfs_device* dev, void* buf,
	s64 count)
{
	s64 res;
	EFI_FS* FileSystem = (EFI_FS*)dev->d_private;
	FS_ASSERT(FileSystem != NULL);

	res = ntfs_device_uefi_io_pread(dev, buf, count, FileSystem->Offset);
	if (res > 0)
		FileSystem->Offset += res;
	return res;
}

/**
 * ntfs_device_uefi_io_write - Write to the device, at the current location
 */
static s64 ntfs_device_uefi_io_write(struct ntfs_device* dev, const void* buf,
	s64 count)
{
	s64 res;
	EFI_FS* FileSystem = (EFI_FS*)dev->d_private;
	FS_ASSERT(FileSystem != NULL);

	if (NDevReadOnly(dev)) {
		errno = EROFS;
		return -1;
	}
	NDevSetDirty(dev);

	res = ntfs_device_uefi_io_pwrite(dev, buf, count, FileSystem->Offset);
	if (res > 0)
		FileSystem->Offset += res;
	return res;
}

/**
 * ntfs_device_uefi_io_stat - Get information about the device
 */
static int ntfs_device_uefi_io_stat(struct ntfs_device *dev, struct stat *buf)
{
	ntfs_log_perror("%s() called\n", __FUNCTION__);
	errno = ENOSYS;
	return -1;
}

/**
 * ntfs_device_uefi_io_ioctl - Perform an ioctl on the device
 */
static int ntfs_device_uefi_io_ioctl(struct ntfs_device *dev,
		unsigned long request, void *argp)
{
	ntfs_log_perror("%s() called\n", __FUNCTION__);
	errno = ENOSYS;
	return -1;
}

/**
 * Device operations for working with unix style devices and files.
 */
struct ntfs_device_operations ntfs_device_uefi_io_ops = {
	.open		= ntfs_device_uefi_io_open,
	.close		= ntfs_device_uefi_io_close,
	.seek		= ntfs_device_uefi_io_seek,
	.read		= ntfs_device_uefi_io_read,
	.write		= ntfs_device_uefi_io_write,
	.pread		= ntfs_device_uefi_io_pread,
	.pwrite		= ntfs_device_uefi_io_pwrite,
	.sync		= ntfs_device_uefi_io_sync,
	.stat		= ntfs_device_uefi_io_stat,
	.ioctl		= ntfs_device_uefi_io_ioctl,
};

/* bridge.h - libntfs-3g interface for UEFI */
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

#pragma once

#include "driver.h"

VOID NtfsSetLogger(UINTN LogLevel);
EFI_STATUS NtfsMount(EFI_FS* FileSystem);
EFI_STATUS NtfsUnmount(EFI_FS* FileSystem);
EFI_STATUS NtfsCreateFile(EFI_NTFS_FILE** File, EFI_FS* FileSystem);
VOID NtfsDestroyFile(EFI_NTFS_FILE* File);
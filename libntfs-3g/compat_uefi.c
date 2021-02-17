/*
 * compat_uefi.c - Definition of standard function calls for UEFI.
 *
 * Copyright © 2021 Pete Batard <pete@akeo.ie>
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdarg.h>
#include <stdlib.h>

#include "compat.h"
#include "logging.h"

#if defined(__MAKEWITH_GNUEFI)
#include <efi.h>
#include <efilib.h>
#else
#include <Base.h>
#include <Uefi.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#endif

void* malloc(size_t size)
{
	return AllocatePool(size);
}

void* calloc(size_t nmemb, size_t size)
{
	return AllocateZeroPool(size * nmemb);
}

void* realloc(void* p, size_t new_size)
{
	size_t* ptr = (size_t*)p;

	if (ptr != NULL) {
		ptr = &ptr[-1];
#ifdef __MAKEWITH_GNUEFI
		ptr = ReallocatePool(ptr, (UINTN)*ptr, (UINTN)(new_size + sizeof(size_t)));
#else
		ptr = ReallocatePool((UINTN)*ptr, (UINTN)(new_size + sizeof(size_t)), ptr);
#endif
		if (ptr != NULL)
			*ptr++ = new_size;
	}
	return ptr;
}

/*
 * gnu-efi provides the following. But the EDK2 doesn't.
 */
#ifndef __MAKEWITH_GNUEFI
void* memcpy(void* dest, const void* src, size_t n)
{
	CopyMem(dest, src, n);
	return dest;
}

void* memset(void* s, int c, size_t n)
{
	SetMem(s, n, (UINT8)c);
	return s;
}
#endif

void* memmove(void* dest, const void* src, size_t n)
{
	/* CopyMem() supports the handling of overlapped regions */
	CopyMem(dest, src, n);
	return dest;
}

int memcmp(const void* s1, const void* s2, size_t n)
{
	return (int)CompareMem(s1, s2, n);
}

void free(void* a)
{
	FreePool(a);
}

/*
 * Converts Gregorian date to seconds since 1970-01-01 00:00:00.
 * Assumes input in normal date format, i.e. 1980-12-31 23:59:59
 * => year=1980, mon=12, day=31, hour=23, min=59, sec=59.
 *
 * Derived from the GPLv2+ implementation found at:
 * http://xenbits.xen.org/hg/xen-3.3-testing.hg/file/98fe9e75d24b/extras/mini-os/arch/ia64/time.c
 */
static UINT64 MkTime(EFI_TIME* Time)
{
	UINTN Month = Time->Month, Year = Time->Year;

	/* 1..12 -> 11,12,1..10 */
	if (0 >= (INTN)(Month -= 2)) {
		Month += 12;	/* Puts Feb last since it has leap day */
		Year -= 1;
	}

	return (
		(
			((UINT64)
				(Year / 4 - Year / 100 + Year / 400 + 367 * Month / 12 + Time->Day) +
				Year * 365 - 719499
				) * 24 + Time->Hour /* now have hours */
			) * 60 + Time->Minute /* now have minutes */
		) * 60 + Time->Second; /* finally seconds */
}

/*
 * Returns the current time in a timespec struct.
 */
int clock_gettime(clockid_t clk_id, struct timespec* now)
{
	EFI_TIME Time;
	EFI_STATUS Status;

	now->tv_sec = 0;
	now->tv_nsec = 0;

	if (clk_id != CLOCK_REALTIME) {
		ntfs_log_perror("%s: Unsupported clock id %d", __FUNCTION__, clk_id);
		errno = ENOSYS;
		return -1;
	}

	Status = RT->GetTime(&Time, NULL);
	if (EFI_ERROR(Status)) {
		ntfs_log_perror("%s: Failed to get the time (%r)", __FUNCTION__, Status);
		errno = EIO;
		return -1;
	}

	now->tv_sec = MkTime(&Time);
	now->tv_nsec = Time.Nanosecond;
	return 0;
}

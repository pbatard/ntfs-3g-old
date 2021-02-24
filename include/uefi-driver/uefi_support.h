/* uefi_support.h - UEFI support declarations */
/*
 *  Copyright © 2014-2021 Pete Batard <pete@akeo.ie>
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

#include <time.h>

#include "driver.h"

#ifndef _MSC_VER
#if !defined(__GNUC__) || (__GNUC__ < 5)
#error gcc 5.0 or later is required for the compilation of this driver.
#endif /* _MSC_VER */

/* Having GNU_EFI_USE_MS_ABI avoids the need for uefi_call_wrapper() */
#if defined(_GNU_EFI) & !defined(GNU_EFI_USE_MS_ABI)
#error gnu-efi, with option GNU_EFI_USE_MS_ABI, is required for the compilation of this driver.
#endif
#endif /* _GNU_EFI & !GNU_EFI_USE_MS_ABI */

/* Sort out the differences between EDK2 and gnu-efi */
#ifdef __MAKEWITH_GNUEFI
#define UnicodeSPrint           SPrint
#define BASE_CR                 _CR
#define FORWARD_LINK_REF(list)  (list).Flink
#define EFI_FILE_SYSTEM_VOLUME_LABEL EFI_FILE_SYSTEM_VOLUME_LABEL_INFO
#else
#define FORWARD_LINK_REF(list)  (list).ForwardLink
#endif

/* Sort out some platform specifics */
#if defined(_M_ARM64) || defined(__aarch64__) || defined (_M_X64) || defined(__x86_64__)
#define PERCENT_P               L"%llx"
#else
#define PERCENT_P               L"%x"
#endif

#ifndef ARRAYSIZE
#define ARRAYSIZE(A)            (sizeof(A)/sizeof((A)[0]))
#endif

#ifndef MIN
#define MIN(x,y)                ((x)<(y)?(x):(y))
#endif

#ifndef PATH_MAX
#define PATH_MAX                4096
#endif

#ifndef PATH_CHAR
#define PATH_CHAR               L'/'
#endif

#define IS_PATH_DELIMITER(x)    (x == PATH_CHAR || x == L'\\')

#define _WIDEN(s)               L ## s
#define WIDEN(s)                _WIDEN(s)

/* For safety, we set a a maximum size that strings shall not outgrow */
#define STRING_MAX              (PATH_MAX + 2)

/* Used with NtfsGetEfiTime */
#define TIME_CREATED            0
#define TIME_ACCESSED           1
#define TIME_MODIFIED           2

#define NTFS_TO_UNIX_TIME(t)    ((t - (NTFS_TIME_OFFSET)) / 10000000)
#define UNIX_TO_NTFS_TIME(t)    ((t * 10000000) + NTFS_TIME_OFFSET)

/* Convenience assertion macros */
#define FL_ASSERT(f, l, a)      if(!(a)) do { Print(L"*** ASSERT FAILED: %a(%d): %a ***\n", f, l, #a); while(1); } while(0)
#define FS_ASSERT(a)            FL_ASSERT(__FILE__, __LINE__, a)

/*
 * Secure string copy, that either uses the already secure version from
 * EDK2, or duplicates it for gnu-efi and asserts on any error.
 */
static __inline VOID _SafeStrCpy(CHAR16* Destination, UINTN DestMax,
	CONST CHAR16* Source, CONST CHAR8* File, CONST UINTN Line) {
#ifdef _GNU_EFI
	FL_ASSERT(File, Line, Destination != NULL);
	FL_ASSERT(File, Line, Source != NULL);
	FL_ASSERT(File, Line, DestMax != 0);
	/*
	 * EDK2 would use RSIZE_MAX, but we use the smaller PATH_MAX for
	 * gnu-efi as it can help detect path overflows while debugging.
	 */
	FL_ASSERT(File, Line, DestMax <= PATH_MAX);
	FL_ASSERT(File, Line, DestMax > StrLen(Source));
	while (*Source != 0)
		*(Destination++) = *(Source++);
	*Destination = 0;
#else
	FL_ASSERT(File, Line, StrCpyS(Destination, DestMax, Source) == 0);
#endif
}

#define SafeStrCpy(d, l, s) _SafeStrCpy(d, l, s, __FILE__, __LINE__)

/*
 * Secure string length, that asserts if the string is NULL or if
 * the length is larger than a predetermined value (STRING_MAX)
 */
static __inline UINTN _SafeStrLen(CONST CHAR16* String, CONST CHAR8* File,
	CONST UINTN Line) {
	UINTN Len = 0;
	FL_ASSERT(File, Line, String != NULL);
	Len = StrLen(String);
	FL_ASSERT(File, Line, Len < STRING_MAX);
	return Len;
}

#define SafeStrLen(s) _SafeStrLen(s, __FILE__, __LINE__)

/*
 * Prototypes for the function calls provided in support.c
 */
CHAR16* GuidToStr(EFI_GUID* Guid);
VOID UnixTimeToEfiTime(time_t t, EFI_TIME* Time);
time_t EfiTimeToUnixTime(EFI_TIME* Time);
INTN CompareDevicePaths(CONST EFI_DEVICE_PATH* dp1, CONST EFI_DEVICE_PATH* dp2);
VOID CleanPath(CHAR16* Path);
CHAR16* DevicePathToString(CONST EFI_DEVICE_PATH* DevicePath);

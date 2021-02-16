/* path.c - Path/DevicePath handling routines */
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

#include "driver.h"

#ifndef PATH_CHAR
#define PATH_CHAR '/'
#endif

/*
 * Poor man's Device Path to string conversion, where we
 * simply convert the path buffer to hexascii.
 * This is needed to support old Dell UEFI platforms, that
 * don't have the Device Path to Text protocol...
 */
CHAR16*
DevicePathToHex(CONST EFI_DEVICE_PATH* DevicePath)
{
	UINTN Len = 0, i;
	CHAR16* DevicePathString = NULL;
	UINT8* Dp = (UINT8*)DevicePath;

	if (DevicePath == NULL)
		return NULL;

	while (!IsDevicePathEnd(DevicePath)) {
		UINT16 NodeLen = DevicePathNodeLength(DevicePath);
		Len += NodeLen;
		DevicePath = (EFI_DEVICE_PATH*)((UINT8*)DevicePath + NodeLen);
	}

	DevicePathString = AllocatePool((2 * Len + 1) * sizeof(CHAR16));
	for (i = 0; i < Len; i++) {
		DevicePathString[2 * i] = ((Dp[i] >> 4) < 10) ?
			((Dp[i] >> 4) + '0') : ((Dp[i] >> 4) - 0xa + 'A');
		DevicePathString[2 * i + 1] = ((Dp[i] & 15) < 10) ?
			((Dp[i] & 15) + '0') : ((Dp[i] & 15) - 0xa + 'A');
	}
	DevicePathString[2 * Len] = 0;

	return DevicePathString;
}

/*
 * Convert a Device Path to a string.
 * The returned value Must be freed with FreePool().
 */
CHAR16*
DevicePathToString(CONST EFI_DEVICE_PATH* DevicePath)
{
	CHAR16* DevicePathString = NULL;
	EFI_STATUS Status;
	EFI_DEVICE_PATH_TO_TEXT_PROTOCOL* DevicePathToText;

	Status = BS->LocateProtocol(&gEfiDevicePathToTextProtocolGuid, NULL, (VOID**)&DevicePathToText);
	if (Status == EFI_SUCCESS)
		DevicePathString = DevicePathToText->ConvertDevicePathToText(DevicePath, FALSE, FALSE);
	else
#if defined(_GNU_EFI)
		DevicePathString = DevicePathToStr((EFI_DEVICE_PATH*)DevicePath);
#else
		DevicePathString = DevicePathToHex(DevicePath);
#endif
	return DevicePathString;
}

/*
 * Compare two device path instances.
 * Returns 0 if the two instances match.
 */
INTN
CompareDevicePaths(CONST EFI_DEVICE_PATH* dp1, CONST EFI_DEVICE_PATH* dp2)
{
	if (dp1 == NULL || dp2 == NULL)
		return -1;

	while (1) {
		UINT8 type1, type2;
		UINT8 subtype1, subtype2;
		UINT16 len1, len2;
		INTN ret;

		type1 = DevicePathType(dp1);
		type2 = DevicePathType(dp2);

		if (type1 != type2)
			return (INTN)type2 - (INTN)type1;

		subtype1 = DevicePathSubType(dp1);
		subtype2 = DevicePathSubType(dp2);

		if (subtype1 != subtype2)
			return (INTN)subtype1 - (INTN)subtype2;

		len1 = DevicePathNodeLength(dp1);
		len2 = DevicePathNodeLength(dp2);
		if (len1 != len2)
			return (INTN)len1 - (INTN)len2;

		ret = CompareMem(dp1, dp2, len1);
		if (ret != 0)
			return ret;

		if (IsDevicePathEnd(dp1))
			break;

		dp1 = (EFI_DEVICE_PATH*)((UINT8*)dp1 + len1);
		dp2 = (EFI_DEVICE_PATH*)((UINT8*)dp2 + len2);
	}

	return 0;
}

/*
 * Copy Src into Dst while converting the path to a relative one inside
 * the current directory. Dst must hold at least Len bytes.
 * Based on path sanitation code by Ludwig Nussel <ludwig.nussel@suse.de>
 */
VOID CopyPathRelative(CHAR8 *Dst, CHAR8 *Src, INTN Len)
{
	CHAR8* o = Dst;
	CHAR8* p = Src;

	*o = '\0';

	while (*p && *p == PATH_CHAR)
		++p;
	for ( ; Len && *p; ) {
		Src = p;
		while (*p && *p != PATH_CHAR)
			p++;
		if (!*p)
			p = Src + strlena(Src);

		/* . => skip */
		if (p - Src == 1 && *Src == '.' )
			if (*p)
				Src = ++p;
		/* .. => pop one */
		else if (p - Src == 2 && *Src == '.' && Src[1] == '.') {
			if (o != Dst) {
				UINTN i;
				*o = '\0';
				for (i = strlena(Dst) - 1; i > 0 && Dst[i] != PATH_CHAR; i--);
				Len += o - &Dst[i];
				o = &Dst[i];
				if(*p)
					++p;
			} else /* nothing to pop */
				if(*p)
					++p;
		} else {
			INTN copy;
			if (o != Dst) {
				--Len;
				*o++ = PATH_CHAR;
			}
			copy = MIN(p - Src, Len);
			CopyMem(o, Src, copy);
			Len -= copy;
			Src += copy;
			o += copy;
			if (*p)
				++p;
		}
		while (*p && *p == PATH_CHAR)
			++p;
	}
	o[Len ? 0 : -1] = '\0';
}

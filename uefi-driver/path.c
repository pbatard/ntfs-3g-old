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

	Status = gBS->LocateProtocol(&gEfiDevicePathToTextProtocolGuid, NULL, (VOID**)&DevicePathToText);
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

/* "The value 0xFFFF is guaranteed not to be a Unicode character at all" */
#define BLANK_CHAR 0xFFFF

/*
 * Clean an existing path from '.', '..' and/or double path separators.
 * This will for instance tranform 'a//b/c/./..///d/./e/..' into 'a/b/d'.
 * Note that trailing path separtors are perserved, if any.
 */
VOID CleanPath(CHAR16* Path)
{
	UINTN i, j, SepCount = 1;
	CHAR16* p, ** SepPos;

	if (!Path || !*Path)
		return;

	/* Count the number of path separators */
	for (p = Path; *p; p++)
		if (*p == PATH_CHAR)
			SepCount++;

	/* Allocate an array of pointers large enough */
	SepPos = AllocatePool(SepCount * sizeof(CHAR16*));
	if (SepPos == NULL)
		return;

	/* Fill our array with pointers to the path separators */
	for (i = 1, p = Path; *p; p++)
		if (*p == PATH_CHAR)
			SepPos[i++] = p;

	/*
	 * To simplify processing, set Path[-1] and
	 * Path[StrLen(Path)] as path separators.
	 */
	SepPos[0] = &Path[-1];
	SepPos[SepCount++] = p;

	/* Now eliminate every single '.' */
	for (i = 1; i < SepCount; i++) {
		/*
		 * Be weary that, per C89 and subsequent C standards:
		 * "When two pointers to members of the same array object are
		 *  subtracted, the difference is divided by the size of a member."
		 * This means that we don't need to correct for sizeof(CHAR16) below.
		 */
		if (SepPos[i] - SepPos[i - 1] == 2 && SepPos[i - 1][1] == L'.') {
			/* Remove the whole section including the leading path sep */
			SepPos[i - 1][1] = BLANK_CHAR;
			if (i > 1)
				SepPos[i - 1][0] = BLANK_CHAR;
		}
	}

	/* And remove sections preceding '..' */
	for (i = 1; i < SepCount; i++) {
		if (SepPos[i] - SepPos[i - 1] == 3 && SepPos[i - 1][1] == L'.' && SepPos[i - 1][2] == L'.') {
			j = i >= 2 ? i - 2 : 0;
			/*
			 * Look for a previous section start, taking care to
			 * step over the ones that have already been eliminated
			 */
			while (j > 0 && *SepPos[j] == BLANK_CHAR)
				j--;
			for (p = (j > 0 ? SepPos[j] : Path); p < SepPos[i]; p++)
				*p = BLANK_CHAR;
		}
	}

	/* Finally, rewrite the path eliminating BLANK_CHAR and double PATH_CHAR */
	for (i = 0, p = Path; *p; p++)
		if (*p != BLANK_CHAR && (*p != PATH_CHAR || i == 0 || Path[i - 1] != PATH_CHAR))
			Path[i++] = *p;
	Path[i] = 0;
}

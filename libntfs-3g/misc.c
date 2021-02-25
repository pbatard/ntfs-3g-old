/**
 * misc.c : miscellaneous :
 *		- dealing with errors in memory allocation
 *
 * Copyright (c) 2008 Jean-Pierre Andre
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "types.h"
#include "misc.h"
#include "logging.h"
#include "compat.h"

#ifndef UEFI_DRIVER
/**
 * ntfs_calloc
 * 
 * Return a pointer to the allocated memory or NULL if the request fails.
 */
void *ntfs_calloc(size_t size)
{
	void *p;
	
	p = calloc(1, size);
	if (!p)
		ntfs_log_perror("Failed to calloc %lld bytes", (long long)size);
	return p;
}

void *ntfs_malloc(size_t size)
{
	void *p;
	
	p = malloc(size);
	if (!p)
		ntfs_log_perror("Failed to malloc %lld bytes", (long long)size);
	return p;
}

void *ntfs_realloc(void *ptr, size_t size)
{
	void *p;

	p = realloc(ptr, size);
	if (!p)
		ntfs_log_perror("Failed to realloc %lld bytes",
				(long long)size);
	return p;
}

void ntfs_free(void *p)
{
	free(p);
}
#else

#ifdef __MAKEWITH_GNUEFI
#include <efi.h>
#include <efilib.h>
#else /* __MAKEWITH_GNUEFI */
#include <Base.h>
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#endif

 /*
  * Memory allocation calls that hook into the standard UEFI
  * allocation ones. Note that, in order to be able to use
  * UEFI's ReallocatePool(), we must keep track of the allocated
  * size, which we store as a size_t before the effective payload.
  */
void* ntfs_malloc(size_t size)
{
	/* Keep track of the allocated size for realloc */
	size_t* p = AllocatePool(size + sizeof(size_t));
	if (!p) {
		ntfs_log_perror("Failed to malloc %lld bytes", (long long)size);
		return NULL;
	}
	p[0] = size;
	return &p[1];
}

void* ntfs_calloc(size_t size)
{
	/* Keep track of the allocated size for realloc */
	size_t* p = AllocateZeroPool(size + sizeof(size_t));
	if (!p) {
		ntfs_log_perror("Failed to calloc %lld bytes", (long long)size);
		return NULL;
	}
	p[0] = size;
	return &p[1];
}

void* ntfs_realloc(void* p, size_t new_size)
{
	size_t* ptr = (size_t*)p;

	if (ptr != NULL) {
		/* Access the previous size, which was stored in malloc/calloc */
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

void ntfs_free(void* p)
{
	size_t* ptr = (size_t*)p;
	if (p != NULL)
		FreePool(&ptr[-1]);
}
#endif

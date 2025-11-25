/**
 * @file vfs.h
 *
 * @author awewsomegamer <awewsomegamer@gmail.com>
 *
 * @LICENSE
 * Arctan-OS/Kernel - Operating System Kernel
 * Copyright (C) 2023-2025 awewsomegamer
 *
 * This file is part of Arctan-OS/Kernel.
 *
 * Arctan is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * @DESCRIPTION
 * Abstract virtual file system driver. Is able to create and delete virtual
 * file systems for caching files on disk.
*/
#ifndef ARC_VFS_H
#define ARC_VFS_H

#define ARC_STD_PERM 0700

#include "drivers/resource.h"
#include "lib/graph/base.h"

#include <stdint.h>
#include <stdbool.h>

typedef struct ARC_VFSGraphData {
	ARC_Resource *resource;
	// ARC_GraphNode *mount; // Is this needed?
	ARC_GraphNode *link;
	struct stat stat;
} ARC_VFSGraphData;

int init_vfs();
int vfs_mount(char *mountpoint, ARC_Resource *resource);
int vfs_unmount(char *mountpoint);
int vfs_open(char *path, int flags, uint32_t mode, ARC_File **ret);
size_t vfs_read(void *buffer, size_t size, size_t count, ARC_File *file);
size_t vfs_write(void *buffer, size_t size, size_t count, ARC_File *file);
int vfs_seek(ARC_File *file, long offset, int whence);
int vfs_close(ARC_File *file);
int vfs_stat(char *path, struct stat *stat);
int vfs_create(char *path, uint32_t mode, int64_t dri_idx, void *dri_arg);
int vfs_remove(char *path);
int vfs_link(char *dest, char *targ, uint32_t mode);
int vfs_rename(char *to, char *from);
int vfs_list(char *path, int depth);


/**
 * Check the requested permissions against the permissions of the file.
 *
 * @param struct stat *stat - The permissions of the file.
 * @param uint32_t requested - Requested permissions (mode).
 * @return Zero if the current program has requested privelleges.
 * */
int vfs_check_perms(struct stat *stat, uint32_t requested);

#endif

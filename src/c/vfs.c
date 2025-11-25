/**
 * @file vfs.c
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
*/
#include "abi-bits/fcntl.h"
#include "abi-bits/seek-whence.h"
#include "fs/graph.h"
#include "fs/vfs.h"
#include "global.h"
#include "lib/graph/base.h"
#include "lib/spinlock.h"
#include "lib/util.h"
#include "mm/allocator.h"

#define NODE_CACHE_SIZE 1024

static ARC_GraphNode *vfs_node_cache[1024] = { 0 };
static uint64_t vfs_node_cache_idx = 0;
static ARC_Spinlock vfs_node_cache_lock;

int init_vfs() {
	return 0;
}

int vfs_mount(char *mountpoint, ARC_Resource *resource) {
	return 0;
}

int vfs_unmount(char *mountpoint) {
	return 0;
}

int vfs_open(char *path, int flags, uint32_t mode, ARC_File **ret) {
	return 0;
}

size_t vfs_read(void *buffer, size_t size, size_t count, ARC_File *file) {
	return 0;
}

size_t vfs_write(void *buffer, size_t size, size_t count, ARC_File *file) {
	return 0;
}

int vfs_seek(ARC_File *file, long offset, int whence) {
	return 0;
}

int vfs_close(ARC_File *file) {
	return 0;
}

int vfs_stat(char *path, struct stat *stat) {
	return 0;
}

int vfs_create(char *path, uint32_t mode, int64_t dri_idx, void *dri_arg) {
	return 0;
}

int vfs_remove(char *path) {
	return 0;
}

int vfs_link(char *dest, char *targ, uint32_t mode) {
	return 0;
}

int vfs_rename(char *to, char *from) {
	return 0;
}

int vfs_list(char *path, int depth) {
	return 0;
}

int vfs_check_perms(struct stat *stat, uint32_t requested) {
	uint32_t UID = 0;
	uint32_t GID = 0;

	if (UID == 0) {
		// ROOT CAN DO WHATEVER!!!!
		return 0;
	}

	if (stat->st_uid == UID) {
		return (stat->st_mode ^ requested) & ((requested >> 6) & 07);
	}

	if (stat->st_gid == GID) {
		return (stat->st_mode ^ requested) & ((requested >> 3) & 07);
	}

	return (stat->st_mode ^ requested) & (requested & 07);
}

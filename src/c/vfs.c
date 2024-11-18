/**
 * @file vfs.c
 *
 * @author awewsomegamer <awewsomegamer@gmail.com>
 *
 * @LICENSE
 * Arctan - Operating System Kernel
 * Copyright (C) 2023-2024 awewsomegamer
 *
 * This file is part of Arctan.
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
#include <fs/vfs.h>
#include <abi-bits/seek-whence.h>

static struct ARC_VFSNode vfs_root = { 0 };
char *vfs_root_name = "";

int init_vfs() {
	vfs_root.type = ARC_VFS_N_DIR;
	vfs_root.name = vfs_root_name;
	init_static_ticket_lock(&vfs_root.branch_lock);
	init_static_mutex(&vfs_root.property_lock);

	// NOTE: This is here such that it is impossible to
	//       delete the root node
	ARC_ATOMIC_INC(vfs_root.ref_count);

	return 0;
}

int vfs_mount(char *mountpoint, struct ARC_Resource *resource) {
	if (mountpoint == NULL || resource == NULL) {
		return -1;
	}


	return 0;
}

int vfs_unmount(struct ARC_VFSNode *mount) {
	if (mount == NULL) {
		return -1;
	}

	return 0;
}

int vfs_open(char *path, int flags, uint32_t mode, struct ARC_File **ret) {
	if (path == NULL || mode == 0 || ret == NULL) {
		return -1;
	}

	return 0;
}

int vfs_read(void *buffer, size_t size, size_t count, struct ARC_File *file) {
	if (buffer == NULL || size == 0 || count == 0 || file == NULL) {
		return 0;
	}

	return 0;
}

int vfs_write(void *buffer, size_t size, size_t count, struct ARC_File *file) {
	if (buffer == NULL || size == 0 || count == 0 || file == NULL) {
		return 0;
	}

	return 0;
}

int vfs_seek(struct ARC_File *file, long offset, int whence) {
	if (file == NULL) {
		return -1;
	}

	long size = file->node->stat.st_size;

	switch (whence) {
		case SEEK_SET: {
			if (0 < offset < size) {
				file->offset = offset;
			}

			break;
		}

		case SEEK_CUR: {
			if (0 < file->offset + offset < size) {
				file->offset += offset;
			}

			break;
		}

		case SEEK_END: {
			if (0 < size - offset - 1 < size) {
				file->offset = size - offset - 1;
			}

			break;
		}
	}

	return 0;
}

int vfs_close(struct ARC_File *file) {
	if (file == NULL) {
		return -1;
	}

	return 0;
}

int vfs_stat(char *filepath, struct stat *stat) {
	if (filepath == NULL || stat == NULL) {
		return -1;
	}

	return 0;
}

int vfs_create(char *path, uint32_t mode, int type, void *arg) {
	if (path == NULL || mode == 0 || type == ARC_VFS_NULL) {
		return -1;
	}

	return 0;
}

int vfs_remove(char *filepath, bool recurse) {
	if (filepath == NULL) {
		return -1;
	}

	return 0;
}

int vfs_link(char *a, char *b, uint32_t mode) {
	if (a == NULL || b == NULL || mode == 0) {
		return -1;
	}

	return 0;
}

int vfs_rename(char *a, char *b) {
	if (a == NULL || b == NULL) {
		return -1;
	}

	return 0;
}

int vfs_list(char *path, int recurse) {
	if (path == NULL) {
		return -1;
	}

	return 0;
}

struct ARC_VFSNode *vfs_create_rel(char *relative_path, struct ARC_VFSNode *start, uint32_t mode, int type, void *arg) {
	if (relative_path == NULL || start == NULL || mode == 0 || type == ARC_VFS_NULL) {
		return NULL;
	}

	return 0;
}

char *vfs_get_relative_path(char *a, char *b) {
	if (a == NULL || b == NULL) {
		return -1;
	}

	return 0;
}

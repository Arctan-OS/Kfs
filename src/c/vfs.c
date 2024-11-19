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
#include <fs/graph.h>
#include <abi-bits/seek-whence.h>
#include <global.h>
#include <mm/allocator.h>
#include <lib/util.h>
#include <lib/resource.h>

static struct ARC_VFSNode vfs_root = { 0 };
char *vfs_root_name = "";

static struct ARC_VFSNode *vfs_get_starting_node(char *filepath) {
	if (*filepath == '/') {
		return &vfs_root;
	} else {
		ARC_DEBUG(ERR, "Non-absolute (%s) paths unsupported\n", filepath);
	}

	return NULL;
}

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

	// Mountpoint should already exist
	struct ARC_VFSNode *node = NULL;
	char *upto = vfs_traverse_filepath(mountpoint, vfs_get_starting_node(mountpoint), &node, 1);

	if (upto == NULL || *upto != 0 || node == NULL) {
		return -2;
	}

	free(upto);

	if (node->type != ARC_VFS_N_DIR || node->children != NULL) {
		return -3;
	}

	mutex_lock(&node->property_lock);

	node->type = ARC_VFS_N_MOUNT;
	node->resource = resource;

	mutex_unlock(&node->property_lock);

	// NOTE: Shouldn't a reference be created to the resource?

	// ref_count remains incremented to ensure it cannot be deleted
	ticket_unlock(&node->branch_lock);

	return 0;
}

int vfs_unmount(struct ARC_VFSNode *node) {
	if (node == NULL) {
		return -1;
	}

	if (node->type != ARC_VFS_N_MOUNT) {
		return -2;
	}

	mutex_lock(&node->property_lock);

	node->type = ARC_VFS_N_MOUNT;
	node->resource = NULL;

	mutex_unlock(&node->property_lock);

	ARC_ATOMIC_DEC(node->ref_count);

	return 0;
}

int vfs_open(char *path, int flags, uint32_t mode, struct ARC_File **ret) {
	if (path == NULL || mode == 0 || ret == NULL) {
		return -1;
	}

	struct ARC_VFSNode *node = NULL;
	char *upto = NULL;

	if (flags & O_CREAT) {
		struct ARC_VFSNode *node_tmp = NULL;
		char *tmp = vfs_load_filepath(path, vfs_get_starting_node(path), &node_tmp);
		ticket_unlock(&node_tmp->branch_lock);

		upto = vfs_create_filepath(tmp, node_tmp, NULL, &node);

		free(tmp);
		ARC_ATOMIC_DEC(node_tmp->ref_count);
	} else {
		upto = vfs_traverse_filepath(path, vfs_get_starting_node(path), &node, 1);
	}

	if (upto == NULL || *upto != 0) {
		return -2;
	}

	free(upto);

	struct ARC_File *file = (struct ARC_File *)alloc(sizeof(*file));

	if (file == NULL) {
		ticket_unlock(&node->branch_lock);
		ARC_ATOMIC_DEC(node->ref_count);

		return -3;
	}

	memset(file, 0, sizeof(*file));

	file->mode = mode;
	file->flags = flags;
	file->node = node;
	file->reference = reference_resource(node->resource);

	*ret = file;

	ticket_unlock(&node->branch_lock);
	// Reference counter will be decremented by close function

	return 0;
}

int vfs_read(void *buffer, size_t size, size_t count, struct ARC_File *file) {
	if (buffer == NULL || size == 0 || count == 0 || file == NULL) {
		return 0;
	}

	struct ARC_VFSNode *node = file->node;

	if (node == NULL) {
		return 0;
	}

	struct ARC_Resource *res = node->resource;

	if (node->type == ARC_VFS_N_LINK && node->link != NULL) {
		res = node->link->resource;
	}

	return res->driver->read(buffer, size, count, file, res);
}

int vfs_write(void *buffer, size_t size, size_t count, struct ARC_File *file) {
	if (buffer == NULL || size == 0 || count == 0 || file == NULL) {
		return 0;
	}

	struct ARC_VFSNode *node = file->node;

	if (node == NULL) {
		return 0;
	}

	struct ARC_Resource *res = node->resource;

	if (node->type == ARC_VFS_N_LINK && node->link != NULL) {
		res = node->link->resource;
	}

	return res->driver->write(buffer, size, count, file, res);
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
	if (file == NULL || file->node == NULL) {
		return -1;
	}

	struct ARC_VFSNode *node = file->node;

	ARC_ATOMIC_DEC(node->ref_count);

	if (node->ref_count > 0) {
		// TODO: Delete the file descriptor
		return 0;
	}

	// TODO: Delete descriptor and file

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
		return NULL;
	}

	return NULL;
}

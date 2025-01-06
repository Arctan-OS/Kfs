/**
 * @file vfs.c
 *
 * @author awewsomegamer <awewsomegamer@gmail.com>
 *
 * @LICENSE
 * Arctan - Operating System Kernel
 * Copyright (C) 2023-2025 awewsomegamer
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
#include <abi-bits/fcntl.h>
#include <global.h>
#include <mm/allocator.h>
#include <lib/util.h>
#include <lib/resource.h>
#include <lib/ringbuffer.h>

#define NODE_CACHE_SIZE 1024

static struct ARC_VFSNode vfs_root = { 0 };

static struct ARC_VFSNode *vfs_node_cache[1024] = { 0 };
static uint64_t vfs_node_cache_idx = 0;
static ARC_GenericSpinlock vfs_node_cache_lock = 0;

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
	vfs_root.name = "";
	init_static_mutex(&vfs_root.branch_lock);
	init_static_mutex(&vfs_root.property_lock);
	init_static_spinlock(&vfs_node_cache_lock);

	// NOTE: This is here such that it is impossible to
	//       delete the root node
	ARC_ATOMIC_INC(vfs_root.ref_count);

	return 0;
}

int vfs_mount(char *mountpoint, struct ARC_Resource *resource) {
	if (mountpoint == NULL || resource == NULL) {
		ARC_DEBUG(ERR, "Resource or mount path are NULL\n");
		return -1;
	}

	// Mountpoint should already exist
	struct ARC_VFSNode *node = NULL;
	char *upto = vfs_traverse_filepath(mountpoint, vfs_get_starting_node(mountpoint), 1, &node);

	if (upto == NULL || node == NULL) {
		ARC_DEBUG(ERR, "Traversal failed\n");
		return -2;
	}

	if (*upto != 0) {
		ARC_DEBUG(ERR, "Traversla failed\n");
		free(upto);
		return -3;
	}

	free(upto);

	// TODO: Account for if node->children != NULL, should be able to just
	//       save the pointer and mount the resource
	if (node->type != ARC_VFS_N_DIR || node->children != NULL) {
		ARC_DEBUG(ERR, "Cannot mount on directory with children or non-directories\n");
		return -4;
	}

	mutex_lock(&node->property_lock);

	node->type = ARC_VFS_N_MOUNT;
	node->resource = resource;

	mutex_unlock(&node->property_lock);

	// ref_count remains incremented to ensure it cannot be deleted

	return 0;
}

int vfs_unmount(struct ARC_VFSNode *node) {
	if (node == NULL) {
		ARC_DEBUG(ERR, "No node given\n");
		return -1;
	}

	if (node->type != ARC_VFS_N_MOUNT) {
		ARC_DEBUG(ERR, "Cannot unmount non-mounted node\n");
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
	if (ret != NULL) {
		*ret = NULL;
	}

	if (path == NULL || mode == 0 || ret == NULL) {
		return -1;
	}

	struct ARC_VFSNode *node = NULL;
	char *upto = vfs_load_filepath(path, vfs_get_starting_node(path), 1, &node);

	if (upto == NULL) {
		ARC_DEBUG(ERR, "Traversal failed\n");
		return -2;
	}

	if (flags & O_CREAT) {
		// TODO: Fix, node info should NOT be NULL
		char *c_upto = vfs_create_filepath(upto, node, 1, NULL, &node);
		free(upto);
		upto = c_upto;
	}

	if (upto == NULL) {
		ARC_DEBUG(ERR, "Traversal failed\n");
		return -3;
	}

	if (*upto != 0) {
		ARC_DEBUG(ERR, "Traversal failed\n");
		free(upto);
		return -4;
	}

	free(upto);

	struct ARC_File *file = (struct ARC_File *)alloc(sizeof(*file));

	if (file == NULL) {
		ticket_unlock(&node->branch_lock);
		ARC_ATOMIC_DEC(node->ref_count);

		return -5;
	}

	memset(file, 0, sizeof(*file));

	file->mode = mode;
	file->node = node;

	*ret = file;

	// Reference counter will be decremented by close function

	return 0;
}

size_t vfs_read(void *buffer, size_t size, size_t count, struct ARC_File *file) {
	if (buffer == NULL || size == 0 || count == 0 || file == NULL) {
		return 0;
	}

	ARC_ATOMIC_INC(file->ref_count);

	struct ARC_VFSNode *node = file->node;

	if (node == NULL) {
		ARC_ATOMIC_DEC(file->ref_count);
		return 0;
	}

	struct ARC_File internal_desc = { 0 };
	memcpy(&internal_desc, file, sizeof(internal_desc));

	if (node->link != NULL) {
		node = node->link;
		internal_desc.node = node;
	}

	struct ARC_Resource *res = node->resource;

	if (res == NULL) {
		ARC_ATOMIC_DEC(file->ref_count);
		return 0;
	}

	int ret = res->driver->read(buffer, size, count, &internal_desc, res);

	file->offset += ret;

	ARC_ATOMIC_DEC(file->ref_count);

	return ret;
}

size_t vfs_write(void *buffer, size_t size, size_t count, struct ARC_File *file) {
	if (buffer == NULL || size == 0 || count == 0 || file == NULL) {
		return 0;
	}

	ARC_ATOMIC_INC(file->ref_count);

	struct ARC_VFSNode *node = file->node;

	if (node == NULL) {
		ARC_ATOMIC_DEC(file->ref_count);
		return 0;
	}

	struct ARC_File internal_desc = { 0 };
	memcpy(&internal_desc, file, sizeof(internal_desc));

	if (node->link != NULL) {
		node = node->link;
		internal_desc.node = node;
	}

	struct ARC_Resource *res = node->resource;

	if (res == NULL) {
		ARC_ATOMIC_DEC(file->ref_count);
		return 0;
	}

	int ret = res->driver->write(buffer, size, count, &internal_desc, res);

	file->offset += ret;

	ARC_ATOMIC_DEC(file->ref_count);

	return ret;
}

int vfs_seek(struct ARC_File *file, long offset, int whence) {
	if (file == NULL) {
		return -1;
	}

	ARC_ATOMIC_INC(file->ref_count);

	if (file->node == NULL) {
		ARC_ATOMIC_DEC(file->ref_count);
		return -2;
	}

	long size = file->node->stat.st_size;

	if (file->node->link != NULL) {
		size = file->node->link->stat.st_size;
	}

	switch (whence) {
		case SEEK_SET: {
			if (0 <= offset && offset < size) {
				file->offset = offset;
			}

			break;
		}

		case SEEK_CUR: {
			if (0 <= file->offset + offset && file->offset + offset < size) {
				file->offset += offset;
			}

			break;
		}

		case SEEK_END: {
			if (0 <= size - offset - 1 && size - offset - 1 < size) {
				file->offset = size - offset - 1;
			}

			break;
		}
	}

	ARC_ATOMIC_DEC(file->ref_count);

	return 0;
}

int vfs_close(struct ARC_File *file) {
	if (file == NULL || file->node == NULL) {
		return -1;
	}

	if (file->ref_count > 0) {
		return -2;
	}

	struct ARC_VFSNode *node = file->node;

	ARC_ATOMIC_DEC(node->ref_count);
	free(file);

	if (node->ref_count > 0) {
		return 0;
	}

	spinlock_lock(&vfs_node_cache_lock);
	uint64_t idx = ARC_ATOMIC_INC(vfs_node_cache_idx);
	vfs_node_cache_idx %= NODE_CACHE_SIZE;
	spinlock_unlock(&vfs_node_cache_lock);

	idx--;

	vfs_delete_node(vfs_node_cache[idx], 1);
	vfs_node_cache[idx] = node;

	return 0;
}

// TODO: What if the given filepath is a directory?
int vfs_stat(char *filepath, struct stat *stat) {
	if (filepath == NULL || stat == NULL) {
		return -1;
	}

	struct ARC_VFSNode *node = NULL;
	char *upto = vfs_load_filepath(filepath, vfs_get_starting_node(filepath), 1, &node);

	if (upto == NULL) {
		return -2;
	}

	if (*upto != 0) {
		ARC_ATOMIC_DEC(node->ref_count);
		free(upto);
		return -3;
	}

	int ret = node->resource->driver->stat(node->resource, NULL, stat);

	ARC_ATOMIC_DEC(node->ref_count);

	return ret;
}

int vfs_create(char *path, struct ARC_VFSNodeInfo *info) {
	if (path == NULL || info == NULL) {
		return -1;
	}

	char *upto = vfs_create_filepath(path, vfs_get_starting_node(path), 1, info, NULL);

	if (upto == NULL) {
		return -2;
	}

	if (*upto != 0) {
		free(upto);
		return -3;
	}

	return 0;
}

int vfs_remove(char *filepath, bool recurse) {
	if (filepath == NULL) {
		return -1;
	}

	struct ARC_VFSNode *node = NULL;
	char *upto = vfs_traverse_filepath(filepath, vfs_get_starting_node(filepath), 0, &node);

	if (upto == NULL) {
		return -2;
	}

	ARC_ATOMIC_DEC(node->ref_count);

	if (*upto != 0) {
		free(upto);
		return -3;
	}

	if (recurse) {
		vfs_delete_node_recursive(node, 1 | (1 << 1));
	} else {
		vfs_delete_node(node, 1 | (1 << 1));
	}

	return 0;
}

int vfs_link(char *a, char *b, int32_t mode) {
	if (a == NULL || b == NULL || mode == 0) {
		// Invalid parameters
		return -1;
	}

	struct ARC_VFSNode *node_a = NULL;
	char *upto = vfs_load_filepath(a, vfs_get_starting_node(a), 1, &node_a);

	if (upto == NULL) {
		// Something has gone very wrong
		return -2;
	}

	if (*upto != 0) {
		// The path to link to does not exist
		ARC_ATOMIC_DEC(node_a->ref_count);

		free(upto);
		return -3;
	}

	free(upto);

	struct ARC_VFSNode *node_b = NULL;
	upto = vfs_load_filepath(b, vfs_get_starting_node(b), 1, &node_b);

	if (upto == NULL) {
		// Something has gone very wrong
		return -4;
	}

	if (*upto == 0) {
		// The path that is going to be linked to already exists, do not
		// overwrite it
		ARC_ATOMIC_DEC(node_a->ref_count);
		ARC_ATOMIC_DEC(node_b->ref_count);

		return -5;
	}

	struct ARC_VFSNodeInfo info = {
	        .type = ARC_VFS_N_LINK,
		.mode = mode == -1 ? MASKED_READ(node_a->stat.st_mode, 0, 0x1FF) : MASKED_READ(mode, 0, 0x1FF),
		.driver_index = (uint64_t)-1
        };

	char *c_upto = vfs_create_filepath(upto, node_b, 1, &info, &node_b);
	free(upto);

	if (c_upto == NULL) {
		// Something has gone very wrong with the creation
		return -5;
	}

	if (*c_upto != 0) {
		// The creation has not completed all the way
		ARC_ATOMIC_DEC(node_a->ref_count);
		ARC_ATOMIC_DEC(node_b->ref_count);
		free(c_upto);
		return -6;
	}

	struct ARC_File fake = { .node = node_b };
	char *rel_path = vfs_get_path(b, a);
	vfs_write(rel_path, 1, strlen(rel_path), &fake);

	mutex_lock(&node_b->branch_lock);
	node_b->link = node_a;
	mutex_unlock(&node_b->branch_lock);

	ARC_ATOMIC_DEC(node_b->ref_count);

	// NOTE: ref_count of Node A is left incremented as it is now in use by this link

	return 0;
}

int vfs_rename(char *a, char *b) {
	if (a == NULL || b == NULL) {
		// Invalid parameters
		return -1;
	}

	struct ARC_VFSNode *node_a = NULL;
	char *upto = vfs_load_filepath(a, vfs_get_starting_node(a), 1, &node_a);

	if (upto == NULL) {
		// Something has gone very wrong
		return -2;
	}

	if (*upto != 0) {
		// The path to rename does not exist
		ARC_ATOMIC_DEC(node_a->ref_count);
		free(upto);
		return -3;
	}

	free(upto);

	struct ARC_VFSNode *node_b = NULL;
	upto = vfs_load_filepath(b, vfs_get_starting_node(b), 1, &node_b);

	if (upto == NULL) {
		// Something has gone very wrong
		return -5;
	}

	if (*upto == 0) {
		// File path already exists, cannot overwrite
		ARC_ATOMIC_DEC(node_a->ref_count);
		ARC_ATOMIC_DEC(node_b->ref_count);

		free(upto);
		return -6;
	}

	struct ARC_VFSNodeInfo info = {
	        .type = ARC_VFS_N_DIR,
		.flags = 1,
		.mode = node_a->stat.st_mode,
		.driver_index = (uint64_t)-1
        };

	char *c_upto = vfs_create_filepath(upto, node_b, 1 | (1 << 1), &info, &node_b);
	free(upto);

	if (c_upto == NULL) {
		// Something has gone very wrong
		ARC_ATOMIC_DEC(node_a->ref_count);
		ARC_ATOMIC_DEC(node_b->ref_count);

		return -6;
	}

	// TODO: There is probably a better way to find out if this is the last
	//       component from the traversal function
	int sep_count = 0;
	while (*c_upto != 0) {
		if (*c_upto == '/') {
			sep_count++;
		}

		c_upto++;
	}

	if (sep_count > 0) {
		ARC_ATOMIC_DEC(node_a->ref_count);
		ARC_ATOMIC_DEC(node_b->ref_count);

		free(c_upto);
		return -7;
	}

	// TODO: Tell the drivers about this
	// TODO: What if A and B are on different mount points?
	mutex_lock(&node_a->parent->branch_lock);

	if (node_a->prev != NULL) {
		// Update Node A's prev->next pointer
		node_a->prev->next = node_a->next;
	} else {
		// Otherwise it is the first in the list, update parent
		node_a->parent->children = node_a->next;
	}

	// Update Node A's next->prev pointer
	if (node_a->next != NULL) {
		node_a->next->prev = node_a->prev;
	}

	mutex_unlock(&node_a->parent->branch_lock);

	// Update Node B's linked list
	mutex_lock(&node_b->branch_lock);

	if (node_b->children != NULL) {
		node_b->children->prev = node_a;
	}

	node_a->next = node_b->children;
	node_b->children = node_a;
	node_a->parent = node_b;

	mutex_unlock(&node_b->branch_lock);

	ARC_ATOMIC_DEC(node_a->ref_count);
	ARC_ATOMIC_DEC(node_b->ref_count);

	return 0;
}

static int internal_vfs_list(struct ARC_VFSNode *node, int level, int org) {
	if (node == NULL) {
		return -1;
	}

	struct ARC_VFSNode *children = node->children;

	if (children == NULL) {
		return 0;
	}

	const char *names[] = {
	        [ARC_VFS_N_DEV] = "Device",
	        [ARC_VFS_N_FILE] = "File",
	        [ARC_VFS_N_DIR] = "Directory",
	        [ARC_VFS_N_BUFF] = "Buffer",
	        [ARC_VFS_N_FIFO] = "FIFO",
	        [ARC_VFS_N_MOUNT] = "Mount",
	        [ARC_VFS_N_ROOT] = "Root",
	        [ARC_VFS_N_LINK] = "Link",
        };

	while (children != NULL) {
		for (int i = 0; i < org - level; i++) {
			printf("\t");
		}
		if (children->type != ARC_VFS_N_LINK) {
			printf("%s (%s, %o, 0x%"PRIx64" B)\n", children->name, names[children->type], children->stat.st_mode, children->stat.st_size);
		} else {
			if (children->link == NULL) {
				printf("%s (Broken Link, %o, 0x%"PRIx64" B) -/> NULL\n", children->name, children->stat.st_mode, children->stat.st_size);
			} else {
				printf("%s (Link, %o, 0x%"PRIx64" B) -> %s\n", children->name, children->stat.st_mode, children->stat.st_size, children->link->name);
			}
		}

		internal_vfs_list(children, level - 1, org);
		children = children->next;
	}

	return 0;
}

int vfs_list(char *path, int recurse) {
	if (path == NULL) {
		return -1;
	}

	struct ARC_VFSNode *node = NULL;
	char *upto = vfs_traverse_filepath(path, vfs_get_starting_node(path), 1, &node);

	if (upto == NULL || *upto != 0) {
		return -2;
	}

	internal_vfs_list(node, recurse, recurse);

	return 0;
}

char *vfs_get_path(char *a, char *b) {
	// Get path from A to B
	if (a == NULL || b == NULL) {
		return NULL;
	}

	size_t max = min(strlen(a), strlen(b));
	size_t delta = 0;
	for (size_t i = 0; i < max; i++) {
		if (a[i] != b[i]) {
			break;
		}

		if (a[i] == '/') {
			delta = i;
		}
	}

	//             + +
	// A: a/b/c/d/e/f/g.txt
	// B: a/b/c/d/x.txt
	//            ^

	int dot_dots = 0;
	for (size_t i = strlen(a) - 1; i > delta; i--) {
		if (a[i] == '/') {
			dot_dots++;
		}
	}

	size_t fin_size = (dot_dots * 3) + strlen(b + delta);

	char *path = (char *)alloc(fin_size + 1);
	memset(path, 0, fin_size + 1);

	for (int i = 0; i < dot_dots; i++) {
		sprintf(path + (i * 3), "../");
	}
	sprintf(path + (dot_dots * 3), "%s", b + delta + 1);

	return path;
}

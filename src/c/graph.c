/**
 * @file graph.c
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
 * The manager of the VFS's node graph.
*/
#include <fs/graph.h>
#include <fs/vfs.h>
#include <mm/allocator.h>
#include <lib/util.h>
#include <lib/perms.h>
#include <global.h>
#include <drivers/dri_defs.h>

static int vfs_mode2type(mode_t mode) {
	switch (mode & S_IFMT) {
		case S_IFDIR: {
			return ARC_VFS_N_DIR;
		}

		case S_IFLNK: {
			return ARC_VFS_N_LINK;
		}

		case S_IFREG: {
			return ARC_VFS_N_FILE;
		}

		default: {
			return ARC_VFS_NULL;
		}
	}
}

static int vfs_type2stat(int type) {
	switch (type) {
		case ARC_VFS_N_DIR: {
			return S_IFDIR;
		}

		case ARC_VFS_N_LINK: {
			return S_IFLNK;
		}

		case ARC_VFS_N_FILE: {
			return S_IFREG;
		}

		default: {
			return ARC_VFS_NULL;
		}
	}
}

static int vfs_type2dri_group(struct ARC_VFSNode *mount, int type) {
	if (type == ARC_VFS_N_DIR || type == ARC_VFS_NULL) {
		return -1;
	}

	if (mount == NULL) {
		return 0;
	}

	return mount->resource->dri_group;
}

static uint64_t vfs_type2dri_index(struct ARC_VFSNode *mount, int type) {
	if (type == ARC_VFS_N_DIR) {
		return 0;
	}

	if (mount == NULL) {
		return ARC_FDRI_BUFFER;
	}

	return mount->resource->dri_index + 1;
}

static int vfs_infer_driver(struct ARC_VFSNode *mount, struct ARC_VFSNodeInfo *info) {
	if (info == NULL) {
		ARC_DEBUG(ERR, "Failed to infer driver, no information provided\n");
		return -1;
	}

	if (info->driver_group == -1) {
		info->driver_group = vfs_type2dri_group(mount, info->type);
		info->driver_index = vfs_type2dri_index(mount, info->type);
	}
}

int vfs_delete_node(struct ARC_VFSNode *node, uint32_t flags) {
	if (node == NULL) {
		return -1;
	}

	if (node->ref_count > 0) {
		return -2;
	}

	// Branch lock
	// If node->type == LINK:
	//  Unincrement ref_count of resolve
	//
	// Change prev->next to node->next (more likely)
	// Change next->prev to prev (less likely)

	return 0;
}

struct ARC_VFSNode *vfs_create_node(struct ARC_VFSNode *parent, char *name, size_t name_len, struct ARC_VFSNodeInfo *info) {
	if (parent == NULL || name == NULL || name_len == 0 || info == NULL || info->type == ARC_VFS_NULL) {
		ARC_DEBUG(ERR, "Failed to create node, improper parameters (%p %p %d %d %p)\n", parent, name, name_len, info != NULL ? info->type : -1, info);
		return NULL;
	}

	struct ARC_VFSNode *node = (struct ARC_VFSNode *)alloc(sizeof(*node));

	if (node == NULL) {
		ARC_DEBUG(ERR, "Failed to allocate memory for new node (%.*s)\n", name, name_len);
		return NULL;
	}

	memset(node, 0, sizeof(*node));

	node->mount = parent->mount;

	if (parent->type == ARC_VFS_N_MOUNT) {
		node->mount = parent;
	}

	node->type = info->type;

	if (info->resource_overwrite == NULL) {
		node->resource = init_resource(info->driver_group, info->driver_index, info->driver_arg);
	} else {
		node->resource = info->resource_overwrite;
	}

	node->name = strndup(name, name_len);

	// NOTE: It is expected that the caller has locked the parent node's branch_lock
	node->parent = parent;
	struct ARC_VFSNode *next = parent->children;
	node->next = next;
	if (next != NULL && next->next != NULL) {
		next->next->prev = node;
	}
	parent->children = node;

	if (node->resource != NULL) {
		node->resource->driver->stat(node->resource, NULL, &node->stat);
	}

	return node;
}

static char *vfs_path_get_next_component(char *path) {
	if (path == NULL)  {
		return NULL;
	}

	if (*path == 0) {
		return NULL;
	}

	char *ret = path;

	if (*ret == '/') {
		ret++;
	}

	while (*ret != 0 && *ret != '/') {
		ret++;
	}

	return ret;
}

static char *vfs_read_link(struct ARC_VFSNode *link) {
	if (link == NULL || link->type != ARC_VFS_N_LINK) {
		ARC_DEBUG(ERR, "Cannot resolve link, improper parameters (%p, %d)\n", link, link == NULL ? ARC_VFS_NULL : link->type);
		return NULL;
	}

	if (link->link != NULL) {
		// No need to resolve it, as it is already resolved
		return NULL;
	}

	if (link->stat.st_size == 0) {
		ARC_DEBUG(WARN, "Not resolving link of zero bytes\n");
		return NULL;
	}

	char *path = (char *)alloc(link->stat.st_size);

	if (path == NULL) {
		ARC_DEBUG(ERR, "Cannot allocate return buffer for link resolution\n");
		return NULL;
	}

	struct ARC_File fake = { .mode = ARC_STD_PERM, .node = link };

	if (vfs_read(path, 1, link->stat.st_size, &fake) != link->stat.st_size) {
		free(path);
		return NULL;
	}

	if (*path == 0) {
		free(path);
		return NULL;
	}

	return path;
}

static char *internal_vfs_traverse(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNode **end,
				   struct ARC_VFSNode *(*callback)(struct ARC_VFSNode *, char *, size_t, char *, void *),
				   void *callback_args) {
	// Flags:
	//  Bit | Description
	//  0   | 1: Resolve links
	size_t lnk_counter = 0;
	struct ARC_VFSNode *org_node = NULL;

        re_iter:;

	if (filepath == NULL || start == NULL) {
		return filepath;
	}

	struct ARC_VFSNode *node = start;
	struct ARC_VFSNode *next = NULL;

	char *comp_base = *filepath == '/' ? filepath + 1 : filepath;
	char *comp_end = vfs_path_get_next_component(comp_base);
	char *mount_path = NULL;
	size_t comp_len = (size_t)comp_end - (size_t)comp_base;

	ARC_ATOMIC_INC(node->ref_count);

	while (comp_end != NULL) {
		if (node->type == ARC_VFS_N_MOUNT) {
			mount_path = comp_base;
		}

		if (strncmp(comp_base, "..", comp_len) == 0) {
			next = node->parent;
			goto next_iter;
		} else if (strncmp(comp_base, ".", comp_len) == 0) {
			next = node;
			goto next_iter;
		}

		struct ARC_VFSNode *children = node->children;
		while (children != NULL) {
			if (strncmp(comp_base, children->name, max(strlen(children->name), comp_len)) == 0) {
				break;
			}

			children = children->next;
		}

		next = children;

		if (callback != NULL && next == NULL) {
			next = callback(node, comp_base, comp_len, mount_path, callback_args);
		}

		if (next == NULL) {
			ARC_DEBUG(ERR, "Quiting traversal of %s, no next node found\n", filepath);

			if (lnk_counter > 0) {
				ARC_DEBUG(ERR, "\tBroken link!\n");
				ARC_ATOMIC_DEC(node->ref_count);
				node = NULL;
			}

			break;
		}

	        next_iter:;

		if (next != node) {
			ARC_ATOMIC_INC(next->ref_count);
			ARC_ATOMIC_DEC(node->ref_count);
			node = next;
		}

		comp_base = *comp_end == '/' ? comp_end + 1 : comp_end; // Skip over /
		comp_end = vfs_path_get_next_component(comp_base);
		comp_len = (size_t)comp_end - (size_t)comp_base;

		next = NULL;
	}

	if (MASKED_READ(flags, 0, 1) == 1) {
		char *new_path = vfs_read_link(node);

		if (new_path == NULL) {
			goto skip_link;
		}

		filepath = new_path;

		if (lnk_counter > 0) {
			free(filepath);
		} else {
			org_node = node;
		}
		lnk_counter++;

		start = node->parent;

		goto re_iter;
	}

	skip_link:;

	if (end != NULL) {
		*end = node;

		if (org_node != NULL) {
			org_node->link = node;
			*end = org_node;
		}
	} else {
		ARC_ATOMIC_DEC(node->ref_count);
		goto create_ret_buff;
	}

	create_ret_buff:;

	// It is the job of the caller to free this ret buffer
	size_t str_len = strlen(comp_base) + 1;
	char *ret = (char *)alloc(str_len);
	memset(ret, 0, str_len);
	memcpy(ret, comp_base, str_len - 1);

	return ret;
}

static struct ARC_VFSNode *callback_vfs_create_filepath(struct ARC_VFSNode *node, char *comp, size_t comp_len, char *mount_path, void *args) {
	if (node == NULL || comp == NULL || comp_len == 0 || args == NULL) {
		ARC_DEBUG(ERR, "Quiting create callback, improper parameters (%p %p %d %p)\n", node, comp, comp_len, args);
		return NULL;
	}

	struct ARC_VFSNodeInfo *info = (struct ARC_VFSNodeInfo *)args;
	struct ARC_VFSNode *ret = NULL;

	struct ARC_VFSNode *mount = node->mount;
	if (node->type == ARC_VFS_N_MOUNT) {
		mount = node;
	}

	void *ticket = ticket_lock(&node->branch_lock);
	ticket_lock_yield(ticket);

	struct ARC_VFSNode *children = node->children;
	while (children != NULL) {
		if (strncmp(comp, children->name, max(strlen(children->name), comp_len)) == 0) {
			break;
		}

		children = children->next;
	}

	if (children != NULL) {
		ticket_unlock(ticket);
		return children;
	}

	if (comp[comp_len] == 0) {
		int phys_ret = 0;

		vfs_infer_driver(mount, info);

		if (mount != NULL) {
			struct ARC_SuperDriverDef *def = (struct ARC_SuperDriverDef *)mount->resource->driver->driver;
			phys_ret = def->create(mount_path, info->mode, info->type);
		}

		// This is the last component, create according to args
		if (phys_ret == 0) {
			ret = vfs_create_node(node, comp, comp_len, info);
		}
	} else {
		// Create a directory
		struct ARC_VFSNodeInfo local_info = { .type = ARC_VFS_N_DIR, .driver_group = -1 };
		ret = vfs_create_node(node, comp, comp_len, &local_info);
	}

	ticket_unlock(ticket);

	return ret;
}

char *vfs_create_filepath(char *filepath, struct ARC_VFSNode *start, struct ARC_VFSNodeInfo *info, struct ARC_VFSNode **end) {
	if (filepath == NULL || start == NULL || info == NULL) {
		ARC_DEBUG(ERR, "Cannot create %s, imporper parameters (%p %p %p)\n", filepath, filepath, start ,info);
		return NULL;
	}

	ARC_DEBUG(INFO, "Creating %s\n", filepath);

	return internal_vfs_traverse(filepath, start, 1, end, callback_vfs_create_filepath, (void *)info);
}

static struct ARC_VFSNode *callback_vfs_load_filepath(struct ARC_VFSNode *node, char *comp, size_t comp_len, char *mount_path, void *args) {
	(void)args;

	if (node == NULL || comp == NULL || comp_len == 0) {
		ARC_DEBUG(ERR, "Cannot load %s, improper arguments (%p %p %d)\n", mount_path, node, comp, comp_len);
		return NULL;
	}

	struct ARC_VFSNode *ret = NULL;

	struct ARC_VFSNode *mount = node->mount;
	if (node->type == ARC_VFS_N_MOUNT) {
		mount = node;
	}

	if (mount == NULL) {
		// There is no mount so there is no reason to stat
		ARC_DEBUG(ERR, "No mountpoint found, quiting load of %s\n", mount_path);
		return NULL;
	}

	void *ticket = ticket_lock(&node->branch_lock);
	ticket_lock_yield(ticket);

	struct ARC_VFSNode *children = node->children;
	while (children != NULL) {
		if (strncmp(comp, children->name, max(strlen(children->name), comp_len)) == 0) {
			break;
		}

		children = children->next;
	}

	if (children != NULL) {
		ticket_unlock(ticket);
		return children;
	}

	// NOTE: This would stat the file each time that this callback
	//       is called. This is not ideal as it limits the speed, perhaps
	//       implement a way of caching the result of this stat
	struct ARC_DriverDef *def = (struct ARC_DriverDef *)mount->resource->driver;
	struct stat stat;

	if (def->stat(mount->resource, mount_path, &stat) != 0) {
		// The node does not exist on the physical filesystem
		ARC_DEBUG(ERR, "Path %s does not exist on the physical filesystem\n", mount_path);
		return NULL;
	}


	// The node does exist, proceed with creation
	if (comp[comp_len] == 0) {
		struct ARC_VFSNodeInfo info = { .driver_group = -1, .type = vfs_mode2type(stat.st_mode) };
		vfs_infer_driver(mount, &info);

		struct ARC_SuperDriverDef *def = mount->resource->driver->driver;
		info.driver_arg = def->locate(mount->resource, mount_path);

		ret = vfs_create_node(node, comp, comp_len, &info);
		memcpy(&ret->stat, &stat, sizeof(stat));
	} else {
		struct ARC_VFSNodeInfo info = { .type = ARC_VFS_N_DIR, .driver_group = -1 };
		ret = vfs_create_node(node, comp, comp_len, &info);
	}

	ticket_unlock(ticket);

	return ret;
}

char *vfs_load_filepath(char *filepath, struct ARC_VFSNode *start, struct ARC_VFSNode **end) {
	if (filepath == NULL || start == NULL) {
		ARC_DEBUG(ERR, "Cannot create %s, improper parameters (%p %p)", filepath, start);
		return NULL;
	}

	ARC_DEBUG(INFO, "Loading %s\n", filepath);

	return internal_vfs_traverse(filepath, start, 1, end, callback_vfs_load_filepath, NULL);
}

char *vfs_traverse_filepath(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNode **end) {
	if (filepath == NULL) {
		ARC_DEBUG(ERR, "Cannot traverse %s\n");
		return NULL;
	}

	ARC_DEBUG(INFO, "Traversing %s\n", filepath);

	return internal_vfs_traverse(filepath, start, flags, end, NULL, NULL);
}

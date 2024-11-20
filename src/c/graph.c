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

int vfs_delete_node(struct ARC_VFSNode *node, uint32_t flags) {
	if (node == NULL) {
		return -1;
	}

	return 0;
}

struct ARC_VFSNode *vfs_create_node(struct ARC_VFSNode *parent, char *name, size_t name_len, int type, struct ARC_VFSNodeInfo *info) {
	if (parent == NULL || name == NULL || name_len == 0 || type == ARC_VFS_NULL) {
		return NULL;
	}

	struct ARC_VFSNode *node = (struct ARC_VFSNode *)alloc(sizeof(*node));

	if (node == NULL) {
		return NULL;
	}

	memset(node, 0, sizeof(*node));

	node->mount = parent->mount;

	if (parent->type == ARC_VFS_N_MOUNT) {
		node->mount = parent;
	}
	
	node->resource = init_resource(info->driver_group, info->driver_index, info->driver_arg);

	node->name = strndup(name, name_len);

	node->parent = parent;
	parent->next->prev = node;
	node->next = parent->next;
	parent->next = node;

	node->type = type;

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
		return NULL;
	}

	char *path = (char *)alloc(link->stat.st_size);

	if (path == NULL) {
		return NULL;
	}

	struct ARC_File fake = { .mode = ARC_STD_PERM, .node = link };

	if (vfs_read(path, 1, link->stat.st_size, &fake) != link->stat.st_size) {
		free(path);
		return NULL;
	}

	return path;
}

static char *internal_vfs_traverse(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNode **end, void **ticket,
				   struct ARC_VFSNode *(*callback)(struct ARC_VFSNode *, char *, size_t, char *, void *),
				   void *callback_args) {
	// Flags:
	//  Bit | Description
	//  0   | 1: Resolve links
	size_t lnk_counter = 0;

        re_iter:;

	if (filepath == NULL || start == NULL) {
		return filepath;
	}

	struct ARC_VFSNode *node = start;
	void *node_ticket = ticket == NULL ? NULL : *ticket;
	struct ARC_VFSNode *next = NULL;

	char *comp_base = filepath;
	char *comp_end = vfs_path_get_next_component(comp_base);
	char *mount_path = NULL;
	size_t comp_len = (size_t)comp_end - (size_t)comp_base;

	if (node_ticket == NULL) {
		ticket_lock(&node->branch_lock);
		ARC_ATOMIC_INC(node->ref_count);
	}

	while (comp_end != NULL) {
		ticket_lock_yield(node_ticket);

		if (node->type == ARC_VFS_N_MOUNT) {
			// NOTE: -1 is included to get the starting '/' character
			mount_path = comp_base - 1;
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
			if (strncmp(comp_base, children->name, comp_len) == 0) {
				break;
			}

			children = children->next;
		}

		next = children;

		if (callback != NULL && next == NULL) {
			next = callback(node, comp_base, comp_len, mount_path, callback_args);
		}

		if (next == NULL) {
			break;
		}

	        next_iter:;

		if (next != node) {
			void *tmp = ticket_lock(&next->branch_lock);
			ARC_ATOMIC_INC(next->ref_count);
			ticket_unlock(node_ticket);
			ARC_ATOMIC_DEC(node->ref_count);
			node = next;
			node_ticket = tmp;
		}

		comp_base = comp_end;
		comp_end = vfs_path_get_next_component(comp_base);
		comp_len = (size_t)comp_end - (size_t)comp_base;
		next = NULL;
	}

	if (MASKED_READ(flags, 0, 1) == 1) {
		ticket_unlock(&node->branch_lock);

		char *new_path = vfs_read_link(node);
		filepath = new_path;

		if (lnk_counter > 0) {
			free(filepath);
		}
		lnk_counter++;

		start = node;

		goto re_iter;
	}

	if (end != NULL && ticket != NULL) {
		*end = node;
		*ticket = node_ticket;
	} else {
		// Node is returned with the ref_count automatically
		// incremented and the branch_lock held, so undo those
		ticket_unlock(node_ticket);
		ARC_ATOMIC_DEC(node->ref_count);
	}

	// It is the job of the caller to free this ret buffer
	size_t str_len = strlen(comp_base) + 1;
	char *ret = (char *)alloc(str_len);
	memset(ret, 0, str_len);
	memcpy(ret, comp_base, str_len - 1);

	return ret;
}

static struct ARC_VFSNode *callback_vfs_create_filepath(struct ARC_VFSNode *node, char *comp, size_t comp_len, char *mount_path, void *args) {
	if (node == NULL || comp == NULL || comp_len == 0 || args == NULL) {
		return NULL;
	}

	struct ARC_VFSNodeInfo *info = (struct ARC_VFSNodeInfo *)args;
	struct ARC_VFSNode *ret = NULL;

	struct ARC_VFSNode *mount = node->mount;
	if (node->type == ARC_VFS_N_MOUNT) {
		mount = node;
	}

	if (comp[comp_len] == 0) {
		int phys_ret = 0;

		if (mount != NULL) {
			struct ARC_SuperDriverDef *def = (struct ARC_SuperDriverDef *)mount->resource->driver->driver;
			phys_ret = def->create(mount_path, info->mode, info->type);
		}

		// This is the last component, create according to args
		if (phys_ret == 0) {
			ret = vfs_create_node(node, comp, comp_len, info->type, info);
		}
	} else {
		// Create a directory
		ret = vfs_create_node(node, comp, comp_len, ARC_VFS_N_DIR, NULL);
	}

	return ret;
}

char *vfs_create_filepath(char *filepath, struct ARC_VFSNode *start, struct ARC_VFSNodeInfo *info, struct ARC_VFSNode **end, void **ticket) {
	if (filepath == NULL || start == NULL || info == NULL) {
		return NULL;
	}

	return internal_vfs_traverse(filepath, start, 1, end, ticket, callback_vfs_create_filepath, (void *)info);
}

static struct ARC_VFSNode *callback_vfs_load_filepath(struct ARC_VFSNode *node, char *comp, size_t comp_len, char *mount_path, void *args) {
	(void)args;

	if (node == NULL || comp == NULL || comp_len == 0) {
		return NULL;
	}

	struct ARC_VFSNode *ret = NULL;

	struct ARC_VFSNode *mount = node->mount;
	if (node->type == ARC_VFS_N_MOUNT) {
		mount = node;
	}

	if (mount == NULL) {
		// There is no mount so there is no reason to stat
		return NULL;
	}

	// NOTE: This would stat the file each time that this callback
	//       is called. This is not ideal as it limits the speed, perhaps
	//       implement a way of caching the result of this stat
	struct ARC_DriverDef *def = (struct ARC_DriverDef *)mount->resource->driver;
	struct stat stat;

	if (def->stat(mount->resource, mount_path, &stat) != 0) {
		// The node does not exist on the physical filesystem
		return NULL;
	}

	// The node does exist, proceed with creation
	if (comp[comp_len] == 0) {
		struct ARC_VFSNodeInfo info = { 0 };

		ret = vfs_create_node(node, comp, comp_len, vfs_mode2type(stat.st_mode), &info);
		memcpy(&ret->stat, &stat, sizeof(stat));
	} else {
		ret = vfs_create_node(node, comp, comp_len, ARC_VFS_N_DIR, NULL);
	}

	return ret;
}

char *vfs_load_filepath(char *filepath, struct ARC_VFSNode *start, struct ARC_VFSNode **end, void **ticket) {
	if (filepath == NULL || start == NULL) {
		return NULL;
	}

	return internal_vfs_traverse(filepath, start, 1, end, ticket, callback_vfs_load_filepath, NULL);
}

char *vfs_traverse_filepath(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNode **end, void **ticket) {
	return internal_vfs_traverse(filepath, start, flags, end, ticket, NULL, NULL);
}

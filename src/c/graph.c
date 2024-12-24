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

static uint64_t vfs_type2dri_index(struct ARC_VFSNode *mount, int type) {
	if (type == ARC_VFS_N_DIR) {
		return 0;
	}

	if (mount == NULL) {
		return ARC_DRIDEF_BUFFER_FILE;
	}

	return mount->u1.resource->dri_index + 1;
}

static int vfs_infer_driver(struct ARC_VFSNode *mount, struct ARC_VFSNodeInfo *info) {
	if (info == NULL) {
		ARC_DEBUG(ERR, "Failed to infer driver, no information provided\n");
		return -1;
	}

	if (info->driver_index == (uint64_t)-1) {
		info->driver_index = vfs_type2dri_index(mount, info->type);
	}
}

int vfs_delete_node(struct ARC_VFSNode *node, uint32_t flags) {
        // Flags:
        //  Bit | Description
        //  0   | 1: Prune upwards
        //  1   | 1: Delete physically
	// TODO: Figure out return values
	int loop_count = -1;

	top:;
	if (node == NULL) {
		return loop_count;
	}

	if (node->type == ARC_VFS_N_DIR && node->children != NULL) {
		ARC_DEBUG(ERR, "Directory node, \"%s\", still has children, aborting\n", node->name);
		return -2;
	}

	if (node->mount == NULL && MASKED_READ(flags, 1, 1) != 1) {
		// Do not delete nodes that are have their data stored
		// in memory unless explicitly specified
		ARC_DEBUG(ERR, "Cannot delete memory-based node, \"%s\", without physical delete set\n", node->name);
		return -3;
	}

	struct ARC_VFSNode *parent = node->parent;
	void *ticket = ticket_lock(&parent->branch_lock);
	if (ticket == NULL) {
		ARC_DEBUG(ERR, "Failed to lock node\n");
		return -4;
	}
	ticket_lock_yield(ticket);

	if (node->ref_count > 0) {
		ARC_DEBUG(ERR, "Node is still in use\n");
		ticket_unlock(ticket);

		return -5;
	}

	if (node->prev != NULL) {
		node->prev->next = node->next;
	} else {
		parent->children = node->next;
	}

	if (node->next != NULL) {
		node->next->prev = node->prev;
	}

	if (node->type == ARC_VFS_N_LINK && node->link != NULL) {
		ARC_ATOMIC_DEC(node->link->ref_count);
	}

	if (node->type != ARC_VFS_N_DIR && node->u1.resource != NULL) {
		uninit_resource(node->u1.resource);
	} else if (node->type == ARC_VFS_N_DIR && node->u1.hint != NULL) {
		free(node->u1.hint);
	}

	if (node->mount != NULL && MASKED_READ(flags, 1, 1) == 1) {
		struct ARC_SuperDriverDef *def = node->mount->u1.resource->driver->driver;
		def->remove(parent->u1.hint == NULL ? vfs_get_path_from_nodes(node->mount, node) : node->name, parent->u1.hint);
	}

	ARC_DEBUG(INFO, "Deleted node, \"%s\", successfully\n", node->name);

	free(node->name);
	free(node);

	ticket_unlock(ticket);

	if (MASKED_READ(flags, 0, 1) == 1) {
		node = parent;
		goto top;
	}

	return 0;
}

static int internal_vfs_recursive_delete(struct ARC_VFSNode *node, uint32_t flags) {
	if (node == NULL) {
		return 0;
	}

	int in_use = 0;

	// NOTE: This works because when a node is deleted the parent node's children pointer
	//       is updated in the event it is the first node
	while (node->children != NULL) {
		in_use += internal_vfs_recursive_delete(node->children, flags);
	}

	if (in_use > 0) {
		return in_use;
	}

	// NOTE: Upwards prune must be disabled
	if (vfs_delete_node(node, flags & (~1)) != 0) {
		return 1;
	}
}

int vfs_delete_node_recursive(struct ARC_VFSNode *node, uint32_t flags) {
	void *parent = node->parent;

	internal_vfs_recursive_delete(node, flags & (~1));
	vfs_delete_node(parent, flags);

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
		node->u1.resource = init_resource(info->driver_index, info->driver_arg);
	} else {
		node->u1.resource = info->resource_overwrite;
	}

	node->name = strndup(name, name_len);

	// NOTE: It is expected that the caller has locked the parent node's branch_lock
	node->parent = parent;
	struct ARC_VFSNode *next = parent->children;
	node->next = next;

	if (next != NULL) {
		next->prev = node;
	}

	parent->children = node;

	if (node->u1.resource != NULL) {
		node->u1.resource->driver->stat(node->u1.resource, NULL, &node->stat, NULL);
	}

	return node;
}

static char *vfs_path_get_next_component(char *path, uint32_t *is_last) {
	if (path == NULL || is_last == NULL)  {
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

	if (*ret == 0) {
		*is_last = 1;
	}

	return ret;
}

// Note: This can probably just be merged with the internal traverse function
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
	//  1   | 1: Ignore last component
	size_t lnk_counter = 0;
	struct ARC_VFSNode *org_node = NULL;

        re_iter:;

	if (filepath == NULL || start == NULL) {
		return filepath;
	}

	struct ARC_VFSNode *node = start;
	struct ARC_VFSNode *next = NULL;

	uint32_t is_last = 0;
	char *comp_base = *filepath == '/' ? filepath + 1 : filepath;
	char *comp_end = vfs_path_get_next_component(comp_base, &is_last);
	char *mount_path = NULL;
	size_t comp_len = (size_t)comp_end - (size_t)comp_base;

	ARC_ATOMIC_INC(node->ref_count);

	while (comp_end != NULL) {
		if (MASKED_READ(flags, 1, 1) == 1 && is_last == 1) {
			break;
		}

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

		void *ticket = ticket_lock(&node->branch_lock);
		ticket_lock_yield(ticket);

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

		ticket_unlock(ticket);

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
		comp_end = vfs_path_get_next_component(comp_base, &is_last);
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

	struct ARC_VFSNodeInfo local_info = { .type = ARC_VFS_N_DIR, .driver_index = (uint64_t)-1 };
	struct ARC_VFSNodeInfo *info = (struct ARC_VFSNodeInfo *)args;
	struct ARC_VFSNode *ret = NULL;

	struct ARC_VFSNode *mount = node->mount;
	if (node->type == ARC_VFS_N_MOUNT) {
		mount = node;
	}

	char *use_path = strndup(comp, comp_len);
	void *use_hint = node->u1.hint;
	if (use_hint == NULL && mount_path != NULL) {
		printf("HINT IS NULL\n");
		free(use_path);
		use_path = strndup(mount_path, (uintptr_t)comp - (uintptr_t)mount_path + comp_len);
	}

	void *hint = node->u1.hint;

	if (comp[comp_len] == 0) {
		vfs_infer_driver(mount, info);
	} else {
		info = &local_info;
	}

	if (mount != NULL) {
		struct ARC_DriverDef *def = mount->u1.resource->driver;

		struct ARC_SuperDriverDef *sdef = def->driver;
		if (sdef->create(use_path, info->mode, info->type, &hint) != 0) {
			free(use_path);
			return NULL;
		}
	}

	free(use_path);

	ret = vfs_create_node(node, comp, comp_len, info);

	if (info->type == ARC_VFS_N_DIR) {
		ret->u1.hint = hint;
	}

	return ret;
}

char *vfs_create_filepath(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNodeInfo *info, struct ARC_VFSNode **end) {
	if (filepath == NULL || start == NULL || info == NULL) {
		ARC_DEBUG(ERR, "Cannot create %s, imporper parameters (%p %p %p)\n", filepath, filepath, start ,info);
		return NULL;
	}

	ARC_DEBUG(INFO, "Creating %s\n", filepath);

	return internal_vfs_traverse(filepath, start, flags | 1, end, callback_vfs_create_filepath, (void *)info);
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

	char *use_path = strndup(comp, comp_len);
	void *use_hint = node->u1.hint;
	if (use_hint == NULL && mount_path != NULL) {
		printf("HINT IS NULL\n");
		free(use_path);
		use_path = strndup(mount_path, (uintptr_t)comp - (uintptr_t)mount_path + comp_len);
	}

	struct ARC_DriverDef *def = mount->u1.resource->driver;
	struct stat stat = { 0 };
	void *hint = NULL;

	if (def->stat(mount->u1.resource, use_path, &stat, &hint) != 0) {
		ARC_DEBUG(ERR, "%s does not exist on the physical filesystem\n", use_path);
		free(use_path);
		return NULL;
	}

	if (comp[comp_len] == 0) {
		struct ARC_VFSNodeInfo info = { .driver_index = (uint64_t)-1, .type = vfs_mode2type(stat.st_mode) };
		vfs_infer_driver(mount, &info);

		struct ARC_SuperDriverDef *sdef = def->driver;
		info.driver_arg = sdef->locate(mount->u1.resource, use_path, use_hint);
		ret = vfs_create_node(node, comp, comp_len, &info);
	} else {
		struct ARC_VFSNodeInfo info = { .type = ARC_VFS_N_DIR, .driver_index = (uint64_t)-1 };
		ret = vfs_create_node(node, comp, comp_len, &info);
		ret->u1.hint = hint;
	}

	free(use_path);

	return ret;
}

char *vfs_load_filepath(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNode **end) {
	if (filepath == NULL || start == NULL) {
		ARC_DEBUG(ERR, "Cannot create %s, improper parameters (%p %p)", filepath, start);
		return NULL;
	}

	ARC_DEBUG(INFO, "Loading %s\n", filepath);

	return internal_vfs_traverse(filepath, start, flags | 1, end, callback_vfs_load_filepath, NULL);
}

char *vfs_traverse_filepath(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNode **end) {
	if (filepath == NULL) {
		ARC_DEBUG(ERR, "Cannot traverse %s\n");
		return NULL;
	}

	ARC_DEBUG(INFO, "Traversing %s\n", filepath);

	return internal_vfs_traverse(filepath, start, flags, end, NULL, NULL);
}


char *vfs_get_path_from_nodes(struct ARC_VFSNode *a, struct ARC_VFSNode *b) {
	if (a == NULL || b == NULL) {
		return NULL;
	}

	return NULL;
}

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
#include <global.h>
#include <stdint.h>
#include <mm/allocator.h>
#include <abi-bits/errno.h>
#include <abi-bits/stat.h>
#include <lib/util.h>
#include <drivers/dri_defs.h>
#include <lib/resource.h>
#include <lib/perms.h>

int vfs_stat2type(mode_t mode) {
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

mode_t vfs_type2mode(int type) {
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

uint64_t vfs_type2idx(int type, struct ARC_VFSNode *mount) {
	switch (type) {
		case ARC_VFS_N_BUFF: {
			return ARC_FDRI_BUFFER;
		}

		case ARC_VFS_N_FIFO: {
			return ARC_FDRI_FIFO;
		}

		default: {
			if (mount == NULL) {
				// If the mount is NULL, then create a buffer
				// that can hold some temporary data
				return ARC_FDRI_BUFFER;
			}

			return mount->resource->dri_index + 1;
		}
	}
}

int vfs_delete_node_recurse(struct ARC_VFSNode *node) {
	if (node == NULL || node->ref_count > 0 || node->is_open) {
		return 1;
	}

	int err = 0;

	struct ARC_VFSNode *child = node->children;
	while (child != NULL) {
		err += vfs_delete_node_recurse(child);
		child = child->next;
	}

	if (uninit_resource(node->resource) != 0) {
		return err + 1;
	}

	free(node->name);
	free(node);

	return err;
}

int vfs_delete_node(struct ARC_VFSNode *node, bool recurse) {
	if (node == NULL || node->ref_count > 0 || node->is_open) {
		return 1;
	}

	struct ARC_VFSNode *child = node->children;

	int err = 0;

	while (child != NULL && recurse == 1) {
		err = vfs_delete_node_recurse(child);
		child = child->next;
	}

	if (err != 0) {
		return err;
	}

	if (uninit_resource(node->resource) != 0) {
		return 1;
	}

	free(node->name);

	if (node->prev == NULL) {
		node->parent->children = node->next;
	} else {
		node->prev->next = node->next;
	}

	if (node->next != NULL) {
		node->next->prev = node->prev;
	}

	free(node);

	return 0;
}

int vfs_bottom_up_prune(struct ARC_VFSNode *bottom, struct ARC_VFSNode *top) {
	struct ARC_VFSNode *current = bottom;
	int freed = 0;

	// We have the topmost node held
	// Delete unused directories
	do {
		void *tmp = current->parent;

		if (current->children == NULL && current->ref_count == 0) {
			vfs_delete_node(current, 0);
			freed++;
		}

		current = tmp;
	} while (current != top && current != NULL && current->type != ARC_VFS_N_MOUNT);

	return freed;
}

static int vfs_top_down_prune_recurse(struct ARC_VFSNode *node, int depth) {
	if (node->ref_count > 0 || depth <= 0 || node->type == ARC_VFS_N_MOUNT) {
		// Set the sign bit to indicate that this
		// path cannot be freed as there is still
		// something being used on it. All other
		// unused files / folders / whatnot has
		// been freed
		return -1;
	}

	struct ARC_VFSNode *child = node->children;
	int count = 0;

	while (child != NULL) {
		int diff = vfs_top_down_prune_recurse(child, depth - 1);

		if (diff < 0 && count > 0) {
			count *= -1;
		}

		count += diff;

		child = child->next;
	}

	if (node->children == NULL) {
		// Free this node
	}

	return count;
}


int vfs_top_down_prune(struct ARC_VFSNode *top, int depth) {
	if (top == NULL) {
		ARC_DEBUG(ERR, "Start node is NULL\n");
		return -1;
	}

	if (depth == 0) {
		return 0;
	}

	struct ARC_VFSNode *node = top->children;
	int count = 0;

	while (node != NULL) {
		count += vfs_top_down_prune_recurse(node, depth - 1);
		node = node->next;
	}

	return count;
}

int vfs_traverse(char *filepath, struct arc_vfs_traverse_info *info, bool resolve_links) {
	char *link_path = NULL;

	if (filepath == NULL || info == NULL) {
		return -1;
	}

	struct ARC_VFSNode *node = info->start;

	top:;

	size_t max = strlen(filepath);

	if (max == 0) {
		return -1;
	}

	ARC_DEBUG(INFO, "Traversing %s\n", filepath);

	ARC_ATOMIC_INC(node->ref_count);

	void *ticket = ticket_lock(&node->branch_lock);

	if (ticket == NULL) {
		ARC_DEBUG(ERR, "Lock error\n");
		return -1;
	}

	size_t last_sep = 0;
	for (size_t i = 0; i < max; i++) {
		bool is_last = (i == max - 1);

		if (!is_last && filepath[i] != '/') {
			// Not at end or not on separator character
			// so skip
			continue;
		}

		if (is_last && (info->create_level & ARC_VFS_NOLCMP) != 0) {
			// This is the last compoenent, and the
			// ARC_VFS_NOLCMP is set, therefore skip
			continue;
		}
		// The following code will only run if:
		//     This is the last character (and therefore the last component)
		//     unless ARC_VFS_NOLCMP is set
		//     If a separator ('/') character is found

		// Determine the component which may or may not be
		// enclosed in separator characters
		//     COMPONENT
		//     ^-size--^
		//     /COMPONENT
		//      ^-size--^
		//     /COMPONENT/
		//      ^-size--^
		int component_size = i - last_sep;
		char *component = filepath + last_sep;

		last_sep = i;

		if (*component == '/') {
			component++;
			component_size--;
		}

		if (is_last && filepath[i] != '/') {
			component_size++;
		}

		if (component_size <= 0) {
			// There is no component
			continue;
		}

		// Wait to acquire lock on current node
		ticket_lock_yield(ticket);

		struct ARC_VFSNode *next = NULL;

		if (node->type == ARC_VFS_N_MOUNT) {
			info->mount = node;
			info->mountpath = component;
		}

		mutex_lock(&node->property_lock);
		int perm_check = check_permissions(&node->stat, info->mode);
		mutex_unlock(&node->property_lock);

		if (perm_check != 0) {
			ARC_ATOMIC_DEC(node->ref_count);
			ticket_unlock(ticket);

			if (info->node != NULL) {
				ARC_ATOMIC_DEC(info->node->ref_count);
				ticket_unlock(info->ticket);
			}

			return EPERM;
		}

		if (component_size == 2 && component[0] == '.' && component[1] == '.') {
			next = node->parent;
			goto resolve;
		} else if (component_size == 1 && component[0] == '.') {
			continue;
		}

		next = node->children;

		while (next != NULL) {
			if (strncmp(component, next->name, component_size) == 0) {
				break;
			}

			next = next->next;
		}

		if (next == NULL) {
			// Node is still NULL, cannot find it in
			// current context
			if ((info->create_level & ARC_VFS_NO_CREAT) != 0) {
				// The node does not exist, but the ARC_VFS_NO_CREAT
				// is set, therefore just return
				ARC_DEBUG(ERR, "ARC_VFS_NO_CREAT specified\n");

				ARC_ATOMIC_DEC(node->ref_count);
				ticket_unlock(ticket);

				if (info->node != NULL) {
					ARC_ATOMIC_DEC(info->node->ref_count);
					ticket_unlock(info->ticket);
				}

				return -3;
			}

			// NOTE: ARC_VFS_GR_CREAT is not really used

			next = (struct ARC_VFSNode *)alloc(sizeof(struct ARC_VFSNode));

			if (next == NULL) {
				ARC_DEBUG(ERR, "Failed to allocate new node\n");

				ARC_ATOMIC_DEC(node->ref_count);
				ticket_unlock(ticket);

				if (info->node != NULL) {
					ARC_ATOMIC_DEC(info->node->ref_count);
					ticket_unlock(info->ticket);
				}

				return i;
			}

			memset(next, 0, sizeof(struct ARC_VFSNode));

			// Insert it into graph
			next->parent = node;
			if (node->children != NULL) {
				next->next = node->children;
				node->children->prev = next;
			}
			node->children = next;

			// Set properties
			next->name = strndup(component, component_size);
			next->type = is_last ? info->type : ARC_VFS_N_DIR;
			init_static_ticket_lock(&next->branch_lock);
			init_static_mutex(&next->property_lock);

			next->stat.st_mode = (info->mode & 0777) | vfs_type2mode(next->type);
			// TODO: Set stat
			//       next->stat.st_uid = get_uid();
			//       ...

			ARC_DEBUG(INFO, "Created new node %s (%p)\n", next->name, next);

			// Check for node on physical filesystem
			if (info->mount == NULL) {
				goto skip_stat;
			}

			next->mount = info->mount;

			// This is the path to the file from the mountpoint
			// Now calculate the size, mounthpath starts at a component
			// therefore there is no need to advance info->mountpath, the length
			// between the current and mountpath pointers are calculated and one is
			// added if the current component is the last component
			size_t phys_path_size = (size_t)((uintptr_t)(filepath + i) - (uintptr_t)info->mountpath) + is_last;
			char *phys_path = strndup(info->mountpath, phys_path_size);

			ARC_DEBUG(INFO, "Node is under a mountpoint, statting %s on node %p\n", phys_path, info->mount);

			struct ARC_Resource *res = info->mount->resource;
			struct ARC_SuperDriverDef *def = (struct ARC_SuperDriverDef *)res->driver->driver;

			if (def->stat(res, phys_path, &next->stat) == 0) {
				// Stat succeeded, file exists on filesystem,
				// set type
				next->type = vfs_stat2type(next->stat.st_mode);
			} else if ((info->create_level & ARC_VFS_FS_CREAT) != 0){
				// ARC_VFS_FS_CREAT is specified and the stat failed,
				// create the node in the physical filesystem
				def->create(phys_path, 0, next->type);
			}

			skip_stat:;

			// Create resource for node if it needs one
			if (next->type == ARC_VFS_N_DIR) {
				goto skip_resource;
			}

			// Figure out the index from the given type
			uint64_t index = vfs_type2idx(next->type, info->mount);

			if (info->mount != NULL) {
				struct ARC_Resource *res = info->mount->resource;

				struct ARC_SuperDriverDef *super_def = (struct ARC_SuperDriverDef *)res->driver->driver;
				void *locate = super_def->locate(res, info->mountpath);

				next->resource = init_resource(res->dri_group, index, locate);
			} else {
				next->resource = init_resource(0, index, info->overwrite_arg);
			}

			skip_resource:;
		}

		resolve:;

		// Next node should not be NULL
		void *next_ticket = ticket_lock(&next->branch_lock);
		if (next_ticket == NULL) {
			ARC_DEBUG(ERR, "Lock error!\n");

			ARC_ATOMIC_DEC(node->ref_count);
			ticket_unlock(ticket);

			if (info->node != NULL) {
				ARC_ATOMIC_DEC(info->node->ref_count);
				ticket_unlock(info->ticket);
			}

			return -3;
		}
		ticket_unlock(ticket);
		ticket = next_ticket;
	
		ARC_ATOMIC_DEC(node->ref_count);
		node = next;
		ARC_ATOMIC_INC(node->ref_count);
	}

	ARC_DEBUG(INFO, "Successfully traversed %s\n", filepath);

	ticket_lock_yield(ticket);

	if (node->type == ARC_VFS_N_LINK && node->link == NULL && resolve_links) {
		struct ARC_File fake = { .flags = 0, .offset = 0, .mode = 0, .node = node, .reference = NULL };

		if (link_path != NULL) {
			free(link_path);
		}

		link_path = (char *)alloc(node->stat.st_size);
		vfs_read(link_path, 1, node->stat.st_size, &fake);

		ARC_DEBUG(INFO, "Resolving link to %s\n", link_path);

		max = strlen(link_path);
		filepath = link_path;

		if (info->node == NULL) {
			info->node = node;
			info->ticket = ticket;
		} else {
			ARC_ATOMIC_DEC(node->ref_count);
			ticket_unlock(ticket);
		}

		node = node->parent;

		goto top;
	}

	// Returned state of node:
	//    - branch_lock is held
	//    - ref_count is incremented by 1
	// Caller must release branch_lock and
	// decrement ref_count when it is done
	// wth the node

	if (link_path == NULL) {
		info->node = node;
		info->ticket = ticket;
	} else {
		info->node->link = node;
		ticket_unlock(ticket);
		free(link_path);
	}

	return 0;
}

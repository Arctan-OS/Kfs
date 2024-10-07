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
 * The implementation of the virtual filesystem's behavior.
*/
#include <abi-bits/stat.h>
#include <abi-bits/fcntl.h>
#include <lib/atomics.h>
#include <abi-bits/errno.h>
#include <lib/resource.h>
#include <mm/allocator.h>
#include <fs/vfs.h>
#include <global.h>
#include <lib/util.h>
#include <fs/graph.h>
#include <drivers/dri_defs.h>
#include <lib/perms.h>

// TODO: Implement error cases for all functions.
// TODO: Create a list of nodes marked for deletion.

static const char *root = "\0";
static const struct ARC_Resource root_res = { 0 };
static struct ARC_VFSNode vfs_root = { 0 };

int init_vfs() {
	vfs_root.name = (char *)root;
	vfs_root.resource = (struct ARC_Resource *)&root_res;
	vfs_root.type = ARC_VFS_N_ROOT;

	if (init_static_ticket_lock(&vfs_root.branch_lock) != 0) {
		return -1;
	}

	if (init_static_mutex(&vfs_root.property_lock) != 0) {
		return -1;
	}

	ARC_DEBUG(INFO, "Created VFS root (%p)\n", &vfs_root);

	return 0;
}

int vfs_mount(char *mountpoint, struct ARC_Resource *resource) {
	if (mountpoint == NULL || resource == NULL) {
		ARC_DEBUG(ERR, "Invalid arguments (%p, %p)\n", mountpoint, resource);
		return EINVAL;
	}

	// If the directory does not already exist, create it in graph, as a disk write could be saved
	struct arc_vfs_traverse_info info = { .type = ARC_VFS_N_DIR, .create_level = ARC_VFS_NO_CREAT };
	ARC_VFS_DETERMINE_START(info, mountpoint);

	if (vfs_traverse(mountpoint, &info, 0) != 0) {
		ARC_DEBUG(ERR, "Failed to traverse graph to mount resource %p\n", resource);
		return -1;
	}

	struct ARC_VFSNode *mount = info.node;

	if (mount->type != ARC_VFS_N_DIR) {
		ARC_DEBUG(ERR, "%s is not a directory (or already mounted), %p\n", mountpoint, resource);
		ARC_ATOMIC_DEC(mount->ref_count);
		ticket_unlock(info.ticket);

		return -2;
	}

	mutex_lock(&mount->property_lock);

	mount->type = (resource->dri_group == ARC_DRI_DEV ? ARC_VFS_N_DEV : ARC_VFS_N_MOUNT);
	mount->resource = resource;

	mutex_unlock(&mount->property_lock);

	// NOTE: ref_count is not decremented so that prune
	//       functions in graph.c have no chance of freeing
	//       a mountpoint
	ticket_unlock(info.ticket);

	ARC_DEBUG(INFO, "Mounted resource %p at %s (%p)\n", resource, mountpoint, mount);

	return 0;
}

int vfs_unmount(struct ARC_VFSNode *mount) {
	if (mount == NULL || mount->type != ARC_VFS_N_MOUNT) {
		ARC_DEBUG(ERR, "Given mount is NULL or not a mount\n");
		return EINVAL;
	}

	ARC_DEBUG(INFO, "Unmounting %p\n", mount);

	if (mutex_lock(&mount->property_lock) != 0) {
		ARC_DEBUG(ERR, "Lock error!\n");
		return -1;
	}

	void *ticket = ticket_lock(&mount->branch_lock);
	if (ticket == NULL) {
		ARC_DEBUG(ERR, "Lock error!\n");
		return -1;
	}
	ticket_lock_yield(ticket);

	if (ticket_lock_freeze(&mount->branch_lock) != 0) {
		ARC_DEBUG(ERR, "Could not freeze lock!\n");
		return -1;
	}

	ARC_ATOMIC_DEC(mount->ref_count);

	// NOTE: The recursive delete function will destroy all nodes
	//       from the top down. The closing procedure also unintiailizes
	//       the resource, which will synchronize nodes to disk
	vfs_delete_node(mount, 1);

	ticket_unlock(ticket);

	ARC_DEBUG(INFO, "Successfully unmount %p\n", mount);

	return 0;
}

int vfs_open(char *path, int flags, uint32_t mode, struct ARC_File **ret) {
	if (path == NULL || ret == NULL) {
		return EINVAL;
	}

	ARC_DEBUG(INFO, "Opening file %s (%d %d), returning to %p\n", path, flags, mode, ret);

	// Find file in node graph, create node graph if needed, do not create the file
	struct arc_vfs_traverse_info info = { .create_level = ARC_VFS_GR_CREAT };

	if (flags & O_CREAT) {
		info.create_level = ARC_VFS_FS_CREAT;
	}

	ARC_VFS_DETERMINE_START(info, path);

	int info_ret = vfs_traverse(path, &info, 1);

	if (info_ret != 0) {
		ARC_DEBUG(ERR, "Failed to traverse node graph (%d)\n", info_ret);
		return -1;
	}

	struct ARC_VFSNode *node = info.node;

	// Create file descriptor
	struct ARC_File *desc = (struct ARC_File *)alloc(sizeof(*desc));
	if (desc == NULL) {
		ARC_DEBUG_ERR("Failed to allocate file descriptor\n");
		ARC_ATOMIC_DEC(node->ref_count);
		ticket_unlock(info.ticket);

		return ENOMEM;
	}

	memset(desc, 0, sizeof(*desc));

	desc->mode = mode;
	desc->node = node;

	ARC_DEBUG(INFO, "Created file descriptor %p\n", desc);

	mutex_lock(&node->property_lock);
	if (node->is_open == 0 && node->type != ARC_VFS_N_DIR) {
		if (node->type == ARC_VFS_N_LINK) {
			node = node->link;
			mutex_lock(&node->property_lock);
		}

		struct ARC_Resource *res = node->resource;
		if (res == NULL || res->driver->open(desc, res, 0, mode) != 0) {
			ARC_DEBUG(ERR, "Failed to open file\n");
			return -2;
		}

		if (desc->node->type == ARC_VFS_N_LINK) {
			mutex_unlock(&node->property_lock);
		}

		node->is_open = 1;
		desc->node->is_open = 1;
		node = desc->node;
	}
	mutex_unlock(&node->property_lock);

	desc->reference = reference_resource(node->resource);

	ticket_unlock(info.ticket);

	*ret = desc;
	ARC_DEBUG(INFO, "Opened file successfully\n");

	return 0;
}

int vfs_read(void *buffer, size_t size, size_t count, struct ARC_File *file) {
	if (buffer == NULL || file == NULL) {
		return -1;
	}

	if (size == 0 || count == 0) {
		return 0;
	}

	struct ARC_Resource *res = NULL;

	if (file->node->type == ARC_VFS_N_LINK && file->node->link != NULL) {
		res = file->node->link->resource;
	} else {
		res = file->node->resource;
	}

	if (res == NULL) {
		ARC_DEBUG(ERR, "One or more is NULL: %p %p\n", res, res->driver->read);
		return -1;
	}

	int ret = res->driver->read(buffer, size, count, file, res);

	file->offset += ret;

	return ret;
}

int vfs_write(void *buffer, size_t size, size_t count, struct ARC_File *file) {
	if (buffer == NULL || file == NULL) {
		return -1;
	}

	if (size == 0 || count == 0) {
		return 0;
	}

	struct ARC_Resource *res = NULL;

	if (file->node->type == ARC_VFS_N_LINK && file->node->link != NULL) {
		res = file->node->link->resource;
	} else {
		res = file->node->resource;
	}

	if (res == NULL) {
		ARC_DEBUG(ERR, "One or more is NULL: %p %p\n", res, res->driver->write);
		return -1;
	}

	int ret = res->driver->write(buffer, size, count, file, res);

	file->offset += ret;

	return ret;
}

int vfs_seek(struct ARC_File *file, long offset, int whence) {
	if (file == NULL) {
		return -1;
	}

	struct ARC_Resource *res = NULL;

	if (file->node->type == ARC_VFS_N_LINK && file->node->link != NULL) {
		res = file->node->link->resource;
	} else {
		res = file->node->resource;
	}

	if (res == NULL) {
		return -1;
	}

	long size = file->node->stat.st_size;

	switch (whence) {
		case SEEK_SET: {
			if (offset < size) {
				file->offset = offset;
			}

			return 0;
		}

		case SEEK_CUR: {
			file->offset += offset;

			if (file->offset >= size) {
				file->offset = size;
			}

			return 0;
		}

		case SEEK_END: {
			file->offset = size - offset - 1;

			if (file->offset < 0) {
				file->offset = 0;
			}

			return 0;
		}
	}

	return res->driver->seek(file, res);
}

int vfs_close(struct ARC_File *file) {
	if (file == NULL) {
		return -1;
	}

	ARC_DEBUG(INFO, "Closing %p\n", file);

	struct ARC_VFSNode *node = file->node;

	mutex_lock(&node->property_lock);

	if (!node->is_open) {
		mutex_unlock(&node->property_lock);

		return -1;
	}

	unrefrence_resource(file->reference);

	// TODO: Account if node->type == ARC_VFS_N_LINK
	if (node->ref_count > 1 || (node->type != ARC_VFS_N_FILE && node->type != ARC_VFS_N_LINK)) {
		ARC_DEBUG(INFO, "ref_count (%lu) > 1 or is non-file, closing file descriptor\n", node->ref_count);

		free(file);
		ARC_ATOMIC_DEC(node->ref_count);

		mutex_unlock(&node->property_lock);

		return 0;
	}

	void *ticket = ticket_lock(&node->branch_lock);
	if (ticket == NULL) {
		ARC_DEBUG(ERR, "Lock error\n");
		return -1;
	}
	ticket_lock_yield(ticket);

	if (ticket_lock_freeze(ticket) != 0) {
		ARC_DEBUG(ERR, "Failed to freeze lock while closing fully\n");
	}

	if (node->ref_count > 1) {
		free(file);
		ARC_ATOMIC_DEC(node->ref_count);

		mutex_unlock(&node->property_lock);
		ticket_lock_thaw(ticket);

		return 0;
	}

	if (node->type == ARC_VFS_N_LINK) {
		ARC_ATOMIC_DEC(node->link->ref_count);
	}

	struct ARC_Resource *res = node->resource;

	if (res == NULL) {
		ARC_DEBUG(ERR, "Node has NULL resource\n");
		mutex_unlock(&node->property_lock);
		ticket_lock_thaw(ticket);
		return -2;
	}

	if (res->driver->close(file, res) != 0) {
		ARC_DEBUG(ERR, "Failed to physically close file\n");
	}

	ARC_ATOMIC_DEC(node->ref_count);
	node->is_open = 0;

	struct ARC_VFSNode *parent = node->parent;
	struct ARC_VFSNode *top = node->mount;
	vfs_delete_node(node, 0);
	vfs_bottom_up_prune(parent, top);
	free(file);

	ARC_DEBUG(INFO, "Closed file successfully\n");

	return 0;
}

int vfs_create(char *path, uint32_t mode, int type, void *arg) {
	if (path == NULL) {
		ARC_DEBUG(ERR, "No path given\n");
		return EINVAL;
	}

	ARC_DEBUG(INFO, "Creating %s (%o, %d)\n", path, mode, type);

	struct arc_vfs_traverse_info info = { .mode = mode, .type = type, .create_level = ARC_VFS_FS_CREAT, .overwrite_arg = arg };
	ARC_VFS_DETERMINE_START(info, path);

	ARC_DEBUG(INFO, "Creating node graph %s from node %p\n", path, info.start);

	int ret = vfs_traverse(path, &info, 0);

	if (ret == 0) {
		ticket_unlock(info.ticket);
		ARC_ATOMIC_DEC(info.node->ref_count);
	}

	return ret;
}

int vfs_remove(char *filepath, bool recurse) {
	if (filepath == NULL) {
		ARC_DEBUG(ERR, "No path given\n");
		return -1;
	}

	ARC_DEBUG(INFO, "Removing %s (%d)\n", filepath, recurse);

	struct arc_vfs_traverse_info info = { .create_level = ARC_VFS_NO_CREAT, .mode = ARC_STD_PERM };
	ARC_VFS_DETERMINE_START(info, filepath);

	int ret = vfs_traverse(filepath, &info, 0);

	if (ret != 0) {
		ARC_DEBUG(ERR, "%s does not exist in node graph\n", filepath);

		return -1;
	}

	ARC_ATOMIC_DEC(info.node->ref_count);

	if (recurse == 0 && info.node->type == ARC_VFS_N_DIR) {
		ARC_DEBUG(ERR, "Trying to non-recursively delete directory\n");

		ticket_unlock(info.ticket);

		return -1;
	}

	mutex_lock(&info.node->property_lock);

	if (info.node->ref_count > 0 || info.node->is_open == 1) {
		ARC_DEBUG(ERR, "Node %p is still in use\n", info.node);

		mutex_unlock(&info.node->property_lock);
		ticket_unlock(info.ticket);

		return -1;
	}

	if (ticket_lock_freeze(&info.node->branch_lock) != 0) {
		ARC_DEBUG(ERR, "Failed to freeze the lock\n");

		mutex_unlock(&info.node->property_lock);
		ticket_unlock(info.ticket);

		return -2;
	}

	// We now have the node completely under control

	if (info.mount != NULL) {
		ARC_DEBUG(INFO, "Removing %s physically on mount %p\n", info.mountpath, info.mount);

		struct ARC_Resource *res = info.mount->resource;

		if (res == NULL) {
			ARC_DEBUG(ERR, "Cannot physically remove path, mount resource is NULL\n");

			mutex_unlock(&info.node->property_lock);
			ticket_lock_thaw(info.ticket);
			ticket_unlock(info.ticket);

			return -1;
		}

		struct ARC_SuperDriverDef *def = (struct ARC_SuperDriverDef *)res->driver->driver;

		if (def->remove(info.mountpath) != 0) {
			ARC_DEBUG(WARN, "Cannot physically remove path\n");

			mutex_unlock(&info.node->property_lock);
			ticket_lock_thaw(info.ticket);
			ticket_unlock(info.ticket);

			return -1;
		}
	}

	struct ARC_VFSNode *parent = info.node->parent;
	vfs_delete_node(info.node, recurse);
	vfs_bottom_up_prune(parent, info.mount);

	return 0;
};

int vfs_link(char *a, char *b, uint32_t mode) {
	if (a == NULL || b == NULL) {
		ARC_DEBUG(ERR, "Invalid parameters given (%p, %p)\n", a, b);
		return EINVAL;
	}

	struct arc_vfs_traverse_info info_a = { .create_level = ARC_VFS_GR_CREAT };
	ARC_VFS_DETERMINE_START(info_a, a);

	ARC_DEBUG(INFO, "Linking %s -> %s (%o)\n", a, b, mode);

	int ret = vfs_traverse(a, &info_a, 0);

	if (ret != 0) {
		ARC_DEBUG(ERR, "Failed to find %s\n", a);
		return 1;
	}

	if (mode == (uint32_t)-1) {
		mode = info_a.node->stat.st_mode;
	}

	struct arc_vfs_traverse_info info_b = { .create_level = ARC_VFS_GR_CREAT, .mode = mode, .type = ARC_VFS_N_LINK };
	ARC_VFS_DETERMINE_START(info_b, b);

	ret = vfs_traverse(b, &info_b, 0);

	if (ret != 0) {
		ARC_DEBUG(ERR, "Failed to find or create %s\n", b);
		return 1;
	}

	struct ARC_VFSNode *src = info_a.node;
	struct ARC_VFSNode *lnk = info_b.node;

	ticket_unlock(info_a.ticket);
	ticket_unlock(info_b.ticket);

	// Resolve link if src is a link
	if (src->type == ARC_VFS_N_LINK) {
		src = src->link;
	}

	struct ARC_File fake = { .node = lnk, .offset = 0, .reference = NULL, .flags = 0, .mode = 0};
	char *relative_path = vfs_get_relative_path(b, a);
	vfs_write(relative_path, 1, strlen(relative_path), &fake);
	free(relative_path);

	mutex_lock(&src->property_lock);
	mutex_lock(&lnk->property_lock);

	src->stat.st_nlink++;
	// src->ref_count is already incremented from the traverse
	ARC_ATOMIC_DEC(lnk->ref_count);
	lnk->link = src;
	lnk->is_open = src->is_open;

	mutex_unlock(&src->property_lock);
	mutex_unlock(&lnk->property_lock);

	// TODO: Think about if b already exists

	ARC_DEBUG(INFO, "Linked %s (%p, %lu) -> %s (%p, %lu)\n", a, src, src->ref_count, b, lnk, lnk->ref_count);

	return 0;
}

int vfs_rename(char *a, char *b) {
	if (a == NULL || b == NULL) {
		ARC_DEBUG(ERR, "Src (%p) or dest (%p) path NULL\n", a, b);
		return -1;
	}

	ARC_DEBUG(INFO, "Renaming %s -> %s\n", a, b);

	struct arc_vfs_traverse_info info_a = { .create_level = ARC_VFS_GR_CREAT };
	ARC_VFS_DETERMINE_START(info_a, a);
	int ret = vfs_traverse(a, &info_a, 0);

	if (ret != 0) {
		ARC_DEBUG(ERR, "Failed to find %s in node graph\n", a);
		return -1;
	}

	struct ARC_VFSNode *node_a = info_a.node;
	ARC_ATOMIC_DEC(node_a->ref_count);

	struct arc_vfs_traverse_info info_b = { .create_level = ARC_VFS_FS_CREAT | ARC_VFS_NOLCMP, .mode = node_a->stat.st_mode };
	ARC_VFS_DETERMINE_START(info_b, b);
	ret = vfs_traverse(b, &info_b, 0);

	if (ret != 0) {
		ARC_DEBUG(ERR, "Failed to find or create %s in node graph / on disk\n", b);
		ticket_unlock(info_a.ticket);

		return -1;
	}

	struct ARC_VFSNode *node_b = info_b.node;
	ARC_ATOMIC_DEC(node_b->ref_count);

	if (node_b == node_a->parent) {
		// Node A is already under B, just rename A.
		ticket_unlock(info_b.ticket);
		goto rename;
	}

	// Remove node_a from parent node in preparation for patching
	struct ARC_VFSNode *parent = node_a->parent;

	void *parent_ticket = ticket_lock(&parent->branch_lock);
	if (parent_ticket == NULL) {
		ARC_DEBUG(ERR, "Lock error\n");
	}
	ticket_lock_yield(parent_ticket);

	if (node_a->next != NULL) {
		node_a->next->prev = node_a->prev;
	}
	if (node_a->prev != NULL) {
		node_a->prev->next = node_a->next;
	} else {
		parent->children = node_a->next;
	}

	ticket_unlock(parent_ticket);

	// Patch node_a onto node_b
	// node_b->branch_lock is locked by vfs_traverse
	if (node_b->children != NULL) {
		node_b->children->prev = node_a;
	}

	// Let node_a see the patch
	// node_a->branch_lock is locked by vfs_traverse
	node_a->parent = node_b;
	node_a->next = node_b->children;
	node_a->prev = NULL;
	node_b->children = node_a;

	ticket_unlock(info_b.ticket);

	rename:;
	// Rename the node
	free(node_a->name);
	char *end = (char *)(a + strlen(a) - 1);
	// Advance back to before ending '/', if there is one
	// if a = /imaginary_path/, this ought to place end at the arrow
	//                      ^
	while (end != a && *end == '/') {
		end--;
	}

	int cnt = 1;

	// Advance back to next / or the beginning of string, from the last example:
	// /imaginary_path/
	// ^-----cnt-----^
	while (end != a && *end != '/') {
		end--;
		cnt++;
	}

	// /imaginary_path/
	//  ^-----cnt----^
	end++;
	cnt--;

	node_a->name = strndup(end, cnt);

	ticket_unlock(info_a.ticket);

	ARC_ATOMIC_DEC(node_a->ref_count);
	ARC_ATOMIC_DEC(node_b->ref_count);

	if (info_a.mount == NULL) {
		// Virtually renamed the files, nothing else
		// to do
		return 0;
	}

	if (info_a.mount != info_b.mount) {
		// Different mountpoints, we need to migrate the files
		// TODO: Physically copy A to B, delete A
		ARC_DEBUG(ERR, "Physical migration unimplemented\n");
		return 0;
	} else {
		// Physically rename file
		struct ARC_SuperDriverDef *def = (struct ARC_SuperDriverDef *)info_a.mount->resource->driver->driver;
		def->rename(info_a.mountpath, info_b.mountpath);
	}

	return 0;
}

static int list(struct ARC_VFSNode *node, int recurse, int org) {
	if (node == NULL) {
		return -1;
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

	struct ARC_VFSNode *child = node->children;
	while (child != NULL) {
		for (int i = 0; i < org - recurse; i++) {
			printf("\t");
		}

		if (child->type == ARC_VFS_N_LINK) {
			struct ARC_File fake = { .flags = 0, .mode = 0, .node = child, .offset = 0, .reference = NULL};
			char *link_to = (char *)alloc(child->stat.st_size);

			if (link_to == NULL) {
				goto default_condition;
			}

			memset(link_to, 0, child->stat.st_size);

			vfs_read(link_to, 1, child->stat.st_size, &fake);
			printf("%s (Link to %s, %o, 0x%"PRIx64" B)\n", child->name, link_to, child->stat.st_mode, child->stat.st_size);

			free(link_to);
		} else {
			default_condition:;
			printf("%s (%s, %o, 0x%"PRIx64" B)\n", child->name, names[child->type], child->stat.st_mode, child->stat.st_size);
		}

		if (recurse > 0) {
			list(child, recurse - 1, org);
		}

		child = child->next;
	}

	return 0;
}

int vfs_list(char *path, int recurse) {
	if (path == NULL) {
		return -1;
	}

	struct arc_vfs_traverse_info info = { .create_level = ARC_VFS_NO_CREAT };

	ARC_VFS_DETERMINE_START(info, path);

	if (vfs_traverse(path, &info, 0) != 0) {
		return -1;
	}

	ARC_ATOMIC_DEC(info.node->ref_count);

	printf("Listing of %s\n", path);
	list(&vfs_root, recurse, recurse);

	ticket_unlock(info.ticket);

	return 0;
}

struct ARC_VFSNode *vfs_create_rel(char *relative_path, struct ARC_VFSNode *start, uint32_t mode, int type, void *arg) {
	if (relative_path == NULL || start == NULL) {
		ARC_DEBUG(ERR, "No path given\n");
		return NULL;
	}

	ARC_DEBUG(INFO, "Creating %p/%s (%o, %d)\n", start, relative_path, mode, type);

	struct arc_vfs_traverse_info info = { .start = start, .mode = mode, .type = type, .create_level = ARC_VFS_GR_CREAT, .overwrite_arg = arg };

	int ret = vfs_traverse(relative_path, &info, 0);

	if (ret == 0) {
		ARC_ATOMIC_DEC(info.node->ref_count);
		ticket_unlock(info.ticket);
	} else {
		return NULL;
	}

	return info.node;
}

char *vfs_get_relative_path(char *from, char *to) {
	if (from == NULL || to == NULL) {
		return NULL;
	}

	ARC_VFS_TO_ABS_PATH(from);
	ARC_VFS_TO_ABS_PATH(to);

	size_t max_from = strlen(from);
	size_t max_to = strlen(to);
	size_t max = min(max_from, max_to);

	size_t i = 0;
	for (; i < max; i++) {
		if (from[i] != to[i]) {
			break;
		}
	}

	size_t dotdots = -1;
	for (size_t j = max_from - 1; j >= i - 1 && j < max_from; j--) {
		if (from[j] == '/') {
			dotdots++;
		}
	}

	to += i;

	if (dotdots == (size_t)-1) {
		dotdots = 0;
	}

	char *path = (char *)alloc(dotdots * 3 + strlen(to));

	for (size_t j = 0; j < dotdots; j++) {
		sprintf(path + j * 3, "../");
	}
	sprintf(path + dotdots * 3, "%s", to);

	return path;
}

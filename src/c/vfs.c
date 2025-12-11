/**
 * @file vfs.c
 *
 * @author awewsomegamer <awewsomegamer@gmail.com>
 *
 * @LICENSE
 * Arctan-OS/Kfs - Kernel Filesystem Implementation
 * Copyright (C) 2023-2025 awewsomegamer
 *
 * This file is part of Arctan-OS/Kfs.
 *
 * Arctan-OS/Kfs is free software; you can redistribute it and/or
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
#include "drivers/dri_defs.h"
#include "drivers/resource.h"
#include "fs/vfs.h"
#include "global.h"
#include "lib/atomics.h"
#include "lib/graph/base.h"
#include "lib/graph/path.h"
#include "lib/spinlock.h"
#include "lib/util.h"
#include "mm/allocator.h"
#include <stdio.h>

#define NODE_CACHE_SIZE 1024

typedef struct ARC_VFSGraphData {
	ARC_Resource *resource;
	ARC_GraphNode *link;
	ARC_GraphNode *mount;
	int type;
	struct stat stat;
} ARC_VFSGraphData;

static ARC_GraphNode *vfs_root = NULL;
static ARC_GraphNode *vfs_node_cache[1024] = { 0 };
static uint64_t vfs_node_cache_idx = 0;
static ARC_Spinlock vfs_node_cache_lock;

static ARC_GraphNode *vfs_get_root(char *path) {
	if (*path == '/') {
		return vfs_root;
	}

	ARC_DEBUG(INFO, "Path %s is non root\n", path);
	// TODO: if a process is running { root = pwd };
	return NULL;
}

static int vfs_mode2type(uint32_t mode) {
	uint32_t type = mode & S_IFMT;

	switch (type) {
		case S_IFBLK: return ARC_VFS_TYPE_DEV;
		case S_IFCHR: return ARC_VFS_TYPE_DEV;
		case S_IFDIR: return ARC_VFS_TYPE_DIR;
		case S_IFIFO: return ARC_VFS_TYPE_FIFO;
		case S_IFLNK: return ARC_VFS_TYPE_LINK;
		case S_IFREG: return ARC_VFS_TYPE_FILE;
		case S_IFSOCK: return ARC_VFS_TYPE_NULL;
	}

	return ARC_VFS_TYPE_NULL;
}

struct create_callback_args {
	uint32_t mode;
	bool create;
        bool no_links;
};

static ARC_GraphNode *vfs_create_callback(ARC_GraphNode *parent, char *name, char *remaining, void *_arg);

static int vfs_load_node(ARC_GraphNode *node, char *path, bool no_links) {
        ARC_DEBUG(INFO, "Loading node %p with path %s, no links ?= %d\n", node, path, no_links);
        
        ARC_VFSGraphData *node_data = (ARC_VFSGraphData *)&node->arb;
        ARC_GraphNode *mount = node_data->mount;
	ARC_VFSGraphData *mount_data = (ARC_VFSGraphData *)&mount->arb;
	ARC_Resource *mount_res = mount_data->resource;
        
        if (mount_res == NULL) {
                ARC_DEBUG(ERR, "No mounted resource\n");
                return -1;
        }

        int type = node_data->type;
        void *dri_arg = mount_res->driver->locate(mount_res, path);

        if (dri_arg == NULL) {
                ARC_DEBUG(ERR, "Could not locate\n");
                return -2;
        }
        
	int group = type == ARC_VFS_TYPE_DIR ? ARC_DRIGRP_FS_DIR : ARC_DRIGRP_FS_FILE;
        int index = mount_res->dri_index;
        // NOTE: Both file and directory group drivers should preform a stat upon init
        node_data->resource = init_resource(group, index, dri_arg);
        
	struct stat *st = &node_data->stat;

        type = node_data->type = vfs_mode2type(st->st_mode);
        
        if (!no_links && type == ARC_VFS_TYPE_LINK) {
                char *link_path = alloc(st->st_size);

                if (link_path == NULL) {
                        return -3;
                }

                ARC_File fake = { .node = node, .offset = 0 };
                if (vfs_read(link_path, st->st_size, 1, &fake) == 0) {
                        return -4;
                }
                
                struct create_callback_args args = { .mode = 0, .create = false, .no_links = true };
                node_data->link = path_traverse(node, link_path, vfs_create_callback, &args);

                return 1;
        }

        return 0;
}

// Will load and create (if requested) if the directories or end file or directory does not exist
static ARC_GraphNode *vfs_create_callback(ARC_GraphNode *parent, char *name, char *remaining, void *_arg) {
	struct create_callback_args *arg = _arg;

	ARC_GraphNode *node = graph_create(sizeof(ARC_VFSGraphData));

	if (node == NULL) {
		return NULL;
	}

	ARC_VFSGraphData *parent_data = (ARC_VFSGraphData *)&parent->arb;
	ARC_GraphNode *mount = parent_data->type == ARC_VFS_TYPE_MOUNT ? parent : parent_data->mount;

	ARC_VFSGraphData *node_data = (ARC_VFSGraphData *)&node->arb;
	node_data->mount = mount;

	int type = ARC_VFS_TYPE_DIR;

	if (*remaining == 0) {
		type = vfs_mode2type(arg->mode);
	}

	node_data->type = type;
	node_data->stat.st_mode = arg->mode;

	if (mount == NULL) {
                node_data->resource = type == ARC_VFS_TYPE_FILE ?
                                      init_resource(ARC_DRIGRP_FS_FILE, ARC_DRIDEF_FS_FILE_BUFFER, NULL)
                                      : NULL;
		return node;
	}

	ARC_DEBUG(INFO, "Mount is non-NULL, doing all the stuff\n");

	char *_path = path_get_abs(parent, mount);
	size_t path_len = strlen(_path) + strlen(name) + 1;
	char *path = alloc(path_len);

	if (path == NULL) {
		goto epic_fail;
	}

	sprintf(path, "%s/%s", _path, name);
        ARC_DEBUG(INFO, "Loading / creating path %s on device %p\n", path, mount);
        
        if (vfs_load_node(node, path, arg->no_links) < 0 && arg->create) {
                ARC_VFSGraphData *mount_data = (ARC_VFSGraphData *)&mount->arb;
                ARC_Resource *mount_res = mount_data->resource;

                if (mount_res->driver->create(mount_res, path, arg->mode, type) != 0) {
                        ARC_DEBUG(ERR, "Failed to create path\n");
                        goto epic_fail;
                }
        }

	return node;

	epic_fail:;
	if (node != NULL) {
		graph_remove(node, true);
	}

	if (_path != NULL) {
		free(_path);
	}

	if (path != NULL) {
		free(path);
	}

	return NULL;
}

static int vfs_remove_node(ARC_GraphNode *node) {
        ARC_VFSGraphData *data = (ARC_VFSGraphData *)&node->arb;
        ARC_GraphNode *link = data->link;
	ARC_GraphNode *mount = data->mount;
        
        char *path_from_mount = mount != NULL ? path_get_abs(node, mount) : NULL;

        ARC_ATOMIC_DEC(node->ref_count); // End the traverse operation
        
	if (graph_remove(node, true) != 0) {
		// TODO: Add to some sort of cache to try to remove again?
		if (mount != NULL) {
                        free(path_from_mount);
                }
		return -1;
	}

        if (link != NULL) {
                ARC_ATOMIC_DEC(link->ref_count);               
        }
        
	if (mount != NULL) {
		data = (ARC_VFSGraphData *)&mount->arb;
		ARC_Resource *res = data->resource;
		res->driver->remove(res, path_from_mount); // TODO: Does it really matter if this fails?
                free(path_from_mount);
	}

        return 0;
}

int init_vfs() {
	vfs_root = init_base_graph(sizeof(ARC_VFSGraphData));

	if (vfs_root == NULL) {
		return -1;
	}

	init_static_spinlock(&vfs_node_cache_lock);

	ARC_DEBUG(INFO, "Initialized VFS\n");

	return 0;
}

int vfs_mount(char *mountpoint, ARC_Resource *resource) {
	if (mountpoint == NULL || *mountpoint != '/' || resource == NULL) {
		ARC_DEBUG(ERR, "Invalid parameters (%p %p)\n", mountpoint, resource);
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(mountpoint);
	ARC_GraphNode *node = path_traverse(root, mountpoint, NULL, NULL);

	if (node == NULL) {
		ARC_DEBUG(ERR, "Could not find node\n");
		return -2;
	}

	ARC_VFSGraphData *data = (ARC_VFSGraphData *)&node->arb;

	if (data->type != ARC_VFS_TYPE_DIR) {
		ARC_DEBUG(ERR, "Node is not directory\n");
		return -3;
	}

        // NOTE: Leave ref_count incremented so the node may only be deleted by
        //       the unmount function
        
	ARC_GraphNode *dup = graph_duplicate(node);
	ARC_GraphNode *t = NULL;
	ARC_ATOMIC_XCHG(&dup->child, &node->child, &t);
	ARC_ATOMIC_DEC(dup->ref_count);

	data->resource = resource;
        ARC_DEBUG(INFO, "Mount %p resource is %p\n", node, data->resource);
	data->mount = dup;
	data->type = ARC_VFS_TYPE_MOUNT;
	// resource->driver->stat(resource, "/", &data->stat); // TODO: Does this work?

	return 0;
}

int vfs_unmount(char *mountpoint) {
	if (mountpoint == NULL || *mountpoint != '/') {
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(mountpoint);
	ARC_GraphNode *node = path_traverse(root, mountpoint, NULL, NULL);

	if (node == NULL) {
		return -2;
	}

	ARC_VFSGraphData *data = (ARC_VFSGraphData *)&node->arb;

	if (data->type != ARC_VFS_TYPE_MOUNT) {
		return -3;
	}

	ARC_Resource *res = data->resource;
	ARC_GraphNode *dup = data->mount;

        // NOTE: Decrement previously undecremented ref_count
	ARC_ATOMIC_DEC(node->ref_count); 
        
	if (graph_remove(node, true) != 0) {
		ARC_DEBUG(ERR, "Failed to remove node from node graph\n");
		// TODO: Try to continue to remove it, insert it into cache, or
		// forcibly remove (without deleting) return error and let process holding it open close it?
		ARC_HANG;
	} else {
		uninit_resource(res);
	}

	graph_add(dup->parent, dup, NULL);

	return 0;
}

int vfs_open(char *path, int flags, uint32_t mode, ARC_File **ret) {
	if (path == NULL || mode == 0 || ret == NULL) {
		ARC_DEBUG(ERR, "Invalid parameters (%p %d %p)\n", path, mode, ret);
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(path);

	struct create_callback_args args = { .create = false, .mode = mode }; // .create will depend on flags
	ARC_GraphNode *node = path_traverse(root, path, vfs_create_callback, &args);

	if (node == NULL) {
		ARC_DEBUG(ERR, "Failed to find node");
		return -2;
	}

	ARC_File *file = alloc(sizeof(*file));

	if (file == NULL) {
		ARC_DEBUG(ERR, "Failed to allocate file descriptor\n");
		return -3;
	}

	memset(file, 0, sizeof(*file));

	file->node = node;

	return 0;
}

size_t vfs_read(void *buffer, size_t size, size_t count, ARC_File *file) {
	if (buffer == NULL || size == 0 || count == 0 || file == NULL) {
		return 0;
	}

	ARC_GraphNode *node = file->node;
        ARC_ATOMIC_INC(node->ref_count);
	ARC_VFSGraphData *data = (ARC_VFSGraphData *)&node->arb;
	ARC_Resource *res = data->resource;

        if (res == NULL) {
                ARC_ATOMIC_DEC(node->ref_count);
                return 0;
        }
        
	ARC_File _file = { 0 };
	memcpy(&_file, file, sizeof(*file));

	if (data->link != NULL) {
		node = data->link;
		data = (ARC_VFSGraphData *)&node->arb;
	}

	_file.node = node;

        size_t ret = res->driver->read(buffer, size, count, file, res);

        ARC_ATOMIC_DEC(node->ref_count);
        
	return ret;
}

size_t vfs_write(void *buffer, size_t size, size_t count, ARC_File *file) {
	if (buffer == NULL || size == 0 || count == 0 || file == NULL) {
		return 0;
	}

	ARC_GraphNode *node = file->node;
        ARC_ATOMIC_INC(node->ref_count);
	ARC_VFSGraphData *data = (ARC_VFSGraphData *)&node->arb;
	ARC_Resource *res = data->resource;

        if (res == NULL) {
                ARC_ATOMIC_DEC(node->ref_count);
                return 0;
        }
        
	ARC_File _file = { 0 };
	memcpy(&_file, file, sizeof(*file));

	if (data->link != NULL) {
		node = data->link;
		data = (ARC_VFSGraphData *)&node->arb;
	}

	_file.node = node;

        size_t ret = res->driver->read(buffer, size, count, file, res);
        
        ARC_ATOMIC_DEC(node->ref_count);
        
	return ret;
}

int vfs_seek(ARC_File *file, long offset, int whence) {
	if (file == NULL) {
		return 0;
	}

	ARC_GraphNode *node = file->node;
	ARC_VFSGraphData *data = (ARC_VFSGraphData *)&node->arb;

	if (data->link != NULL) {
		node = data->link;
		data = (ARC_VFSGraphData *)&node->arb;
	}

	long size = data->stat.st_size;

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

	return 0;
}

int vfs_close(ARC_File *file) {
	if (file == NULL) {
		return -1;
	}

        ARC_GraphNode *node = file->node;
        ARC_ATOMIC_DEC(node->ref_count);

        if (vfs_remove_node(node) != 0) {
                ARC_ATOMIC_INC(node->ref_count);
                return -1;
        }

	return 0;
}

int vfs_stat(char *path, struct stat *stat) {
	if (path == NULL || stat == NULL) {
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(path);
	struct create_callback_args args = { .create = false };
	ARC_GraphNode *node = path_traverse(root, path, vfs_create_callback, &args);

	if (node == NULL) {
		return -2;
	}

	ARC_VFSGraphData *data = (ARC_VFSGraphData *)&node->arb;
	ARC_Resource *res = data->resource;
	res->driver->stat(res, NULL, &data->stat); // TODO: Does it really matter if this fails?
	memcpy(stat, &data->stat, sizeof(*stat));

        ARC_ATOMIC_DEC(node->ref_count);
        
	return 0;
}

int vfs_create(char *path, uint32_t mode) {
	if (path == NULL || mode == 0) {
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(path);
	struct create_callback_args args = { .create = true, .mode = mode };
        ARC_GraphNode *node = path_traverse(root, path, vfs_create_callback, &args);
        
	if (node == NULL) {
		return -1;
	}

        ARC_ATOMIC_DEC(node->ref_count);
        
	return 0;
}

int vfs_remove(char *path) {
	if (path == NULL) {
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(path);
	struct create_callback_args args = { .create = false };
	ARC_GraphNode *node = path_traverse(root, path, vfs_create_callback, &args);

	if (node == NULL) {
		return -1;
	}

        ARC_ATOMIC_DEC(node->ref_count);

	return vfs_remove_node(node);
}

int vfs_link(char *file, char *link, uint32_t mode) {
	if (file == NULL || link == NULL) {
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(file);
	struct create_callback_args args = { .create = false };
	ARC_GraphNode *_file = path_traverse(root, file, vfs_create_callback, &args);

	if (_file == NULL) {
		return -2;
	}

	ARC_VFSGraphData *file_data = (ARC_VFSGraphData *)&_file->arb;

	root = vfs_get_root(link);
	args.create = true;
	args.mode = mode == 0 ? file_data->stat.st_mode : mode;
	args.mode &= ~S_IFMT;
	args.mode |= S_IFLNK;
	ARC_GraphNode *_link = path_traverse(root, link, vfs_create_callback, &args);

	if (_link == NULL) {
		return -3;
	}

	ARC_VFSGraphData *link_data = (ARC_VFSGraphData *)&_link->arb;
	link_data->link = _file;

	char *rel_path = path_get_rel(_link, _file);
	if (rel_path != NULL) {
                ARC_File fake = { .node = _link, .offset = 0 };
                vfs_write(rel_path, strlen(rel_path), 1, &fake);
                
		free(rel_path);
	}

        // NOTE: Ref_count for _file is left incremented to signify that is in-use by
        //       the link
        ARC_ATOMIC_DEC(_link->ref_count);
        
	return 0;
}

// TODO: Account for links - currently if a link is renamed such that
//       the depth from the shared parent directory is changed it will break.
//       Shall the kernel be nice and recalculate the path of a symlink or let
//       it be broke?
int vfs_rename(char *from, char *to) {
	if (from == NULL || to == NULL) {
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(from);

	struct create_callback_args args = { .create = false };
	ARC_GraphNode *_from = path_traverse(root, from, vfs_create_callback, &args);

	if (_from == NULL) {
		return -2;
	}

	size_t to_len = strlen(to);
	size_t i = to_len - 2;
	for (; i > 0; i--) {
		if (to[i] == '/') {
			break;
		}
	}

	char *parent_path = strndup(to, i + 1);
	if (parent_path == NULL) {
		return -3;
	}

	root = vfs_get_root(parent_path);
	args.create = true;
	ARC_GraphNode *parent = path_traverse(root, parent_path, vfs_create_callback, &args);
	free(parent_path);

	if (parent == NULL) {
		return -4;
	}

        ARC_ATOMIC_DEC(_from->ref_count);
        
	if (graph_remove(_from, false) != 0) {
		return -5;
	}

	char *name = strdup(&to[i + 1]);
	if (name == NULL) {
		return -6;
	}

	size_t name_len = to_len - i;

	if (name[name_len - 1] == '/') {
		name[name_len - 1] = 0;
	}
        
	graph_add(parent, _from, name);

        ARC_ATOMIC_DEC(parent->ref_count);
        
	free(name);

	return 0;
}

static int internal_vfs_list(ARC_GraphNode *node, int level, int org) {
	if (node == NULL) {
		return -1;
	}

	ARC_GraphNode *children = node->child;

	if (children == NULL) {
		return 0;
	}

	const char *names[] = {
	        [ARC_VFS_TYPE_DEV] = "Device",
	        [ARC_VFS_TYPE_FILE] = "File",
	        [ARC_VFS_TYPE_DIR] = "Directory",
	        [ARC_VFS_TYPE_BUFF] = "Buffer",
	        [ARC_VFS_TYPE_FIFO] = "FIFO",
	        [ARC_VFS_TYPE_MOUNT] = "Mount",
	        [ARC_VFS_TYPE_ROOT] = "Root",
	        [ARC_VFS_TYPE_LINK] = "Link",
        };

	while (children != NULL) {
		for (int i = 0; i < org - level; i++) {
			printf("\t");
		}

		ARC_VFSGraphData *data = (ARC_VFSGraphData *)&children->arb;
		struct stat *stat = &data->stat;

		if (data->type != ARC_VFS_TYPE_LINK) {
			printf("%s (%s, %o, 0x%"PRIx64" B, %lu)\n", children->name, names[data->type], stat->st_mode, stat->st_size, children->ref_count);
		} else {
			if (data->link == NULL) {
				printf("%s (Broken Link, %o, 0x%"PRIx64" B, %lu) -/> NULL\n", children->name, stat->st_mode, stat->st_size, children->ref_count);
			} else {
				printf("%s (Link, %o, 0x%"PRIx64" B, %lu) -> %s\n", children->name, stat->st_mode, stat->st_size, children->ref_count, data->link->name);
			}
		}

		internal_vfs_list(children, level - 1, org);
		children = children->next;
	}

	return 0;
}

int vfs_list(char *path, int depth) {
	if (path == NULL || depth == 0) {
		return -1;
	}

	ARC_GraphNode *root = vfs_get_root(path);
	ARC_GraphNode *node = path_traverse(root, path, NULL, NULL);

	if (node == NULL) {
		return -2;
	}

	internal_vfs_list(node, depth, depth);

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

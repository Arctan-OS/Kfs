/**
 * @file graph.h
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
 * This file handles the management of the VFS's node graph, it is not intended
 * to be used by anything other than fs/vfs.c.
*/
#ifndef ARC_VFS_GRAPH_H
#define ARC_VFS_GRAPH_H

#include <stdbool.h>
#include <fs/vfs.h>

#define ARC_VFS_DETERMINE_START(__info, __path) \
        if (*__path == '/') { \
		__info.start = &vfs_root; \
	} else { \
		ARC_DEBUG(WARN, "Definitely getting current directory\n") \
	}

#define ARC_VFS_TO_ABS_PATH(__relative) \
	if (*__relative != '/') {					\
		ARC_DEBUG(WARN, "Definitely converting relative path to absolute\n"); \
	}

struct arc_vfs_traverse_info {
	/// The node to start traversal from.
	struct ARC_VFSNode *start;
	/// The sought after nodes.
	struct ARC_VFSNode *node;
	/// Node that node is mounted on.
	struct ARC_VFSNode *mount;
	/// Path to node from the mountpoint.
	char *mountpath;
	/// Mode to create new nodes with.
	uint32_t mode;
	/// Type of node to create.
	int type;
	/// Creation level.
#define ARC_VFS_NO_CREAT 0x1 // Don't create anything, just traverse.
#define ARC_VFS_GR_CREAT 0x2 // Create node graph from physical file system.
#define ARC_VFS_FS_CREAT 0x4 // Create node graph and physical file system from path.
#define ARC_VFS_NOLCMP   0x8 // Create node graph except for last component in path.
	int create_level;
	/// Overwrite for final node's resource if it needs to be created (NULL = no overwrite).
	// TODO: This is fairly hacky, try to come up with a better way
	// to do this
	void *overwrite_arg;
	// Ticket for the node->branch_lock.
	void *ticket;
};

/**
 * Convert stat.st_type to node->type.
 * */
int vfs_stat2type(mode_t mode);

/**
 * Convert node->type to stat.st_type.
 * */
mode_t vfs_type2mode(int type);

/**
 * Convert node->type nad node->mount to a group 3 driver index.
 * */
uint64_t vfs_type2idx(int type, struct ARC_VFSNode *mount);

/**
 * Delete a node from the node graph.
 *
 * General function for deleting nodes. Parent
 * node's children are updated to remove the link
 * to this node.
 *
 * NOTE: It is expected that the branch and property locks are
 *       held by the caller and the branch lock is frozen.
 *
 * @param struct ARC_VFSNode *node - The node which to destroy.
 * @param bool recurse - Whether to recurse or not.
 * @return number of nodes which were not destroyed.
 * */
int vfs_delete_node(struct ARC_VFSNode *node, bool recurse);

/**
 * Prune unused nodes from bottom to top.
 *
 * A node, starting at a lower level of the graph
 * starts a chain of destruction upward of unused nodes.
 * This frees up memory, at the cost of some caching.
 *
 * NOTE: It is expected for the branch lock of the top node to be held by
 *       the caller prior to calling this function.
 *
 * @param struct ARC_VFSNode *bottom - Bottom-most node to start from.
 * @param struct ARC_VFSNode *top - Topmost node to end on.
 * @return positive integer indicating the number of nodes freed.
 * */
int vfs_bottom_up_prune(struct ARC_VFSNode *bottom, struct ARC_VFSNode *top);

/**
 * Prune unused nodes from top to a depth.
 *
 * Given a starting node, top, work downwards to free
 * any unused nodes to save on some memory. It takes
 * quite a big lock (the top node is locked).
 *
 * NOTE: It is expected for the branch lock of the top node to be held by
 *       the caller prior to calling this function.
 *
 * @param struct ARC_VFSNode *top - The starting node.
 * @param int depth - How far down the function can traverse.
 * @return poisitive integer indicating the number of nodes freed.
 * */
int vfs_top_down_prune(struct ARC_VFSNode *top, int depth);

/**
 * Traverse the given filepath in the VFS.
 *
 * Traverse the nodes of the VFS to reach the final node in the given
 * filepath using the given information structure.
 *
 * @param char *filepath - The filepath to traverse.
 * @param struct arc_vfs_traverse_info *info - The information structure.
 * @param bool resolve_links - 1: Resolve links encountered while traversing.
 * @return zero upon success. When a filepath has been sucessfully traversed, the
 * found node's (info->node) branch_lock is returned held (node->ticket) and the node's reference
 * count is left incremented. The caller will need to decrement these values once
 * finished using the node. Unsuccessful traversals, on whatever node they've ended up
 * on, will automatically decrement the reference counter and release the branch_lock.
 * */
int vfs_traverse(char *filepath, struct arc_vfs_traverse_info *info, bool resolve_links);

#endif

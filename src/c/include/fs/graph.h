/**
 * @file graph.h
 *
 * @author awewsomegamer <awewsomegamer@gmail.com>
 *
 * @LICENSE
 * Arctan-OS/Kernel - Operating System Kernel
 * Copyright (C) 2023-2025 awewsomegamer
 *
 * This file is part of Arctan-OS/Kernel.
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

/**
 * Deletes a
 *
 * Flags:
 *  Bit | Description
 *  0   | Recursive delete
 *  1   | Physical delete
 * */
int vfs_delete_node(struct ARC_VFSNode *node, uint32_t flags);
int vfs_delete_node_recursive(struct ARC_VFSNode *node, uint32_t flags);
// NOTE: Flags is bitwise OR'd with 1, setting link resolution, do not depend on this behavior, set it yourself
char *vfs_create_filepath(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNodeInfo *info, struct ARC_VFSNode **end);
// NOTE: Flags is bitwise OR'd with 1, setting link resolution, do not depend on this behavior, set it yourself
char *vfs_load_filepath(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNode **end);
char *vfs_traverse_filepath(char *filepath, struct ARC_VFSNode *start, uint32_t flags, struct ARC_VFSNode **end);
// NOTE: Expects a and b ref_count to be incremented by caller or have both nodes' branch_locks
//       held
char *vfs_get_path_from_nodes(struct ARC_VFSNode *a, struct ARC_VFSNode *b);

#endif

/**
 * @file vfs.h
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
 * Abstract virtual file system driver. Is able to create and delete virtual
 * file systems for caching files on disk.
*/
#ifndef ARC_VFS_H
#define ARC_VFS_H

#define ARC_VFS_NULL  0

#define ARC_VFS_N_FILE  1
#define ARC_VFS_N_DIR   2
#define ARC_VFS_N_MOUNT 3
#define ARC_VFS_N_ROOT  4
#define ARC_VFS_N_LINK  5
#define ARC_VFS_N_BUFF  6
#define ARC_VFS_N_FIFO  7
#define ARC_VFS_N_DEV   8

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <lib/resource.h>
#include <lib/atomics.h>
#include <stdbool.h>
#include <abi-bits/seek-whence.h>

/**
 * A single node in a VFS tree.
 * */
struct ARC_VFSNode {
	/// Pointer to the link.
	struct ARC_VFSNode *link;
	/// Pointer to the mount structure this node is or is under.
	struct ARC_VFSNode *mount;
	/// Pointer to the parent of the current node.
	struct ARC_VFSNode *parent;
	/// Pointer to the head of the children linked list.
	struct ARC_VFSNode *children;
	/// Pointer to the next element in the current linked list.
	struct ARC_VFSNode *next;
	/// Pointer to the previous element in the current linked list.
	struct ARC_VFSNode *prev;
	/// Lock on branching of this node (link, parent, children, next, prev, name)
	struct ARC_TicketLock branch_lock;
	/// Pointer to the device. References can be found through consulting resource.
	struct ARC_Resource *resource;
	/// The name of this node.
	char *name;
	/// Number of references to this node (> 0 means node and children cannot be destroyed).
	uint64_t ref_count;
	/// Lock on the properties of this node (type, mount, stat, is_open)
	ARC_GenericMutex property_lock;
	/// The type of node.
	int type;
	bool is_open;
	// Stat
	struct stat stat;
};

/**
 * Initalize the VFS root.
 *
 * @return 0: success
 * */
int init_vfs();

/**
 * Create a new mounted device under the given mountpoint.
 *
 * i.e. mount A at /mounts/A
 *
 * @param struct ARC_VFSNode *mountpoint - The VFS node under which to mount (/mounts).
 * @param char *name - The name of the mountpoint (A).
 * @param struct ARC_Resource *resource - The resource by which to address the mountpoint.
 * @return zero on success.
 * */
int vfs_mount(char *mountpoint, struct ARC_Resource *resource);

/**
 * Unmounts the given mountpoint
 *
 * All nodes under the given node will be destroyed and their
 * resources will be uninitialized and closed.
 *
 * @param struct ARC_VFSNode *mount - The mount to unmount
 * @return zero on success.
 * */
int vfs_unmount(struct ARC_VFSNode *mount);

/**
 * Open the given file with the given perms.
 *
 * @param char *filepath - Path to the file to open.
 * @param int flags - Flags to open the file with.
 * @param uint32_t mode - Permissions to open the file with.
 * @param struct ARC_File **ret - The pointer to write the address of the resultant file descriptor to.
 * @return zero upon success.
 * */
int vfs_open(char *path, int flags, uint32_t mode, struct ARC_File **ret);

/**
 * Read the given file.
 *
 * Reads /count words of /a size bytes from /a file into
 * /a buffer.
 *
 * @param void *buffer - The buffer into which to read the file data.
 * @param size_t size - The size of each word to read.
 * @param size_t count - The number of words to read.
 * @param struct ARC_File *file - The file to read.
 * @return the number of bytes read.
 * */
int vfs_read(void *buffer, size_t size, size_t count, struct ARC_File *file);

/**
 * Write to the given file.
 *
 * Writes /count words of /a size bytes from /a buffer into
 * /a file.
 *
 * @param void *buffer - The buffer from which to read the data.
 * @param size_t size - The size of each word to write.
 * @param size_t count - The number of words to write.
 * @param struct ARC_VFSNode *file - The file to write.
 * @return the number of bytes written.
 * */
int vfs_write(void *buffer, size_t size, size_t count, struct ARC_File *file);

/**
 * Change the offset in the given file.
 *
 * @param struct ARC_VFSNode *file - The file in which to seek
 * @param long offset - The offset from whence
 * @param int whence - The position from which to apply the offset to.
 * @return zero on success.
 * */
int vfs_seek(struct ARC_File *file, long offset, int whence);

/**
 * Close the given file in the VFS.
 *
 * @param struct ARC_File *file - The file to close.
 * @return zero on success.
 * */
int vfs_close(struct ARC_File *file);

/**
 * Get the status of a file
 *
 * Returns the status of the file found at the given
 * filepath in the stat.
 *
 * @param char *filepath - The filepath of the file to stat.
 * @param struct stat *stat - The place where to put the information.
 * @return zero on success.
 * */
int vfs_stat(char *filepath, struct stat *stat);

int vfs_create(char *path, uint32_t mode, int type, void *arg);
int vfs_remove(char *filepath, bool recurse);
int vfs_link(char *a, char *b, uint32_t mode);
int vfs_rename(char *a, char *b);
int vfs_list(char *path, int recurse);

struct ARC_VFSNode *vfs_create_rel(char *relative_path, struct ARC_VFSNode *start, uint32_t mode, int type, void *arg);

/**
 * Get the relative path from B to A.
 * */
char *vfs_get_relative_path(char *a, char *b);

#endif

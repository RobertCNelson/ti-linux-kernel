/*
 * Copyright (C) 2006, 2010  Novell, Inc.
 * Copyright (C) 2015  Red Hat, Inc.
 * Written by Andreas Gruenbacher <agruenba@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/sched.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/richacl.h>

MODULE_LICENSE("GPL");

/**
 * richacl_alloc  -  allocate a richacl
 * @count:	number of entries
 */
struct richacl *
richacl_alloc(int count, gfp_t gfp)
{
	size_t size = sizeof(struct richacl) + count * sizeof(struct richace);
	struct richacl *acl = kzalloc(size, gfp);

	if (acl) {
		atomic_set(&acl->a_refcount, 1);
		acl->a_count = count;
	}
	return acl;
}
EXPORT_SYMBOL_GPL(richacl_alloc);

/**
 * richacl_clone  -  create a copy of a richacl
 */
struct richacl *
richacl_clone(const struct richacl *acl, gfp_t gfp)
{
	int count = acl->a_count;
	size_t size = sizeof(struct richacl) + count * sizeof(struct richace);
	struct richacl *dup = kmalloc(size, gfp);

	if (dup) {
		memcpy(dup, acl, size);
		atomic_set(&dup->a_refcount, 1);
	}
	return dup;
}

/**
 * richace_copy  -  copy an acl entry
 */
void
richace_copy(struct richace *to, const struct richace *from)
{
	memcpy(to, from, sizeof(struct richace));
}

/*
 * richacl_mask_to_mode  -  compute the file permission bits from mask
 * @mask:	%RICHACE_* permission mask
 *
 * Compute the file permission bits corresponding to a particular set of
 * richacl permissions.
 *
 * See richacl_masks_to_mode().
 */
static int
richacl_mask_to_mode(unsigned int mask)
{
	int mode = 0;

	if (mask & RICHACE_POSIX_MODE_READ)
		mode |= S_IROTH;
	if (mask & RICHACE_POSIX_MODE_WRITE)
		mode |= S_IWOTH;
	if (mask & RICHACE_POSIX_MODE_EXEC)
		mode |= S_IXOTH;

	return mode;
}

/**
 * richacl_masks_to_mode  -  compute file permission bits from file masks
 *
 * When setting a richacl, we set the file permission bits to indicate maximum
 * permissions: for example, we set the Write permission when a mask contains
 * RICHACE_APPEND_DATA even if it does not also contain RICHACE_WRITE_DATA.
 *
 * Permissions which are not in RICHACE_POSIX_MODE_READ,
 * RICHACE_POSIX_MODE_WRITE, or RICHACE_POSIX_MODE_EXEC cannot be represented
 * in the file permission bits.  Such permissions can still be effective, but
 * not for new files or after a chmod(); they must be explicitly enabled in the
 * richacl.
 */
int
richacl_masks_to_mode(const struct richacl *acl)
{
	return richacl_mask_to_mode(acl->a_owner_mask) << 6 |
	       richacl_mask_to_mode(acl->a_group_mask) << 3 |
	       richacl_mask_to_mode(acl->a_other_mask);
}
EXPORT_SYMBOL_GPL(richacl_masks_to_mode);

/**
 * richacl_mode_to_mask  - compute a file mask from the lowest three mode bits
 * @mode:	mode to convert to richacl permissions
 *
 * When the file permission bits of a file are set with chmod(), this specifies
 * the maximum permissions that processes will get.  All permissions beyond
 * that will be removed from the file masks, and become ineffective.
 */
unsigned int
richacl_mode_to_mask(umode_t mode)
{
	unsigned int mask = 0;

	if (mode & S_IROTH)
		mask |= RICHACE_POSIX_MODE_READ;
	if (mode & S_IWOTH)
		mask |= RICHACE_POSIX_MODE_WRITE;
	if (mode & S_IXOTH)
		mask |= RICHACE_POSIX_MODE_EXEC;

	return mask;
}

/**
 * richacl_want_to_mask  - convert the iop->permission want argument to a mask
 * @want:	@want argument of the permission inode operation
 *
 * When checking for append, @want is (MAY_WRITE | MAY_APPEND).
 *
 * Richacls use the iop->may_create and iop->may_delete hooks which are used
 * for checking if creating and deleting files is allowed.  These hooks do not
 * use richacl_want_to_mask(), so we do not have to deal with mapping MAY_WRITE
 * to RICHACE_ADD_FILE, RICHACE_ADD_SUBDIRECTORY, and RICHACE_DELETE_CHILD
 * here.
 */
unsigned int
richacl_want_to_mask(unsigned int want)
{
	unsigned int mask = 0;

	if (want & MAY_READ)
		mask |= RICHACE_READ_DATA;
	if (want & MAY_DELETE_SELF)
		mask |= RICHACE_DELETE;
	if (want & MAY_TAKE_OWNERSHIP)
		mask |= RICHACE_WRITE_OWNER;
	if (want & MAY_CHMOD)
		mask |= RICHACE_WRITE_ACL;
	if (want & MAY_SET_TIMES)
		mask |= RICHACE_WRITE_ATTRIBUTES;
	if (want & MAY_EXEC)
		mask |= RICHACE_EXECUTE;
	/*
	 * differentiate MAY_WRITE from these request
	 */
	if (want & (MAY_APPEND |
		    MAY_CREATE_FILE | MAY_CREATE_DIR |
		    MAY_DELETE_CHILD)) {
		if (want & MAY_APPEND)
			mask |= RICHACE_APPEND_DATA;
		if (want & MAY_CREATE_FILE)
			mask |= RICHACE_ADD_FILE;
		if (want & MAY_CREATE_DIR)
			mask |= RICHACE_ADD_SUBDIRECTORY;
		if (want & MAY_DELETE_CHILD)
			mask |= RICHACE_DELETE_CHILD;
	} else if (want & MAY_WRITE)
		mask |= RICHACE_WRITE_DATA;
	return mask;
}
EXPORT_SYMBOL_GPL(richacl_want_to_mask);

/*
 * Copyright (C) 2010  Novell, Inc.
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

struct richacl *get_cached_richacl(struct inode *inode)
{
	struct base_acl *acl;

	acl = ACCESS_ONCE(inode->i_acl);
	if (acl && IS_RICHACL(inode)) {
		spin_lock(&inode->i_lock);
		acl = inode->i_acl;
		if (acl != ACL_NOT_CACHED)
			base_acl_get(acl);
		spin_unlock(&inode->i_lock);
	}
	return container_of(acl, struct richacl, a_base);
}
EXPORT_SYMBOL_GPL(get_cached_richacl);

struct richacl *get_cached_richacl_rcu(struct inode *inode)
{
	struct base_acl *acl = rcu_dereference(inode->i_acl);

	return container_of(acl, struct richacl, a_base);
}
EXPORT_SYMBOL_GPL(get_cached_richacl_rcu);

void set_cached_richacl(struct inode *inode, struct richacl *acl)
{
	struct base_acl *old = NULL;

	spin_lock(&inode->i_lock);
	old = inode->i_acl;
	rcu_assign_pointer(inode->i_acl, &richacl_get(acl)->a_base);
	spin_unlock(&inode->i_lock);
	if (old != ACL_NOT_CACHED)
		base_acl_put(old);
}
EXPORT_SYMBOL_GPL(set_cached_richacl);

void forget_cached_richacl(struct inode *inode)
{
	struct base_acl *old = NULL;

	spin_lock(&inode->i_lock);
	old = inode->i_acl;
	inode->i_acl = ACL_NOT_CACHED;
	spin_unlock(&inode->i_lock);
	if (old != ACL_NOT_CACHED)
		base_acl_put(old);
}
EXPORT_SYMBOL_GPL(forget_cached_richacl);

struct richacl *get_richacl(struct inode *inode)
{
	struct richacl *acl;

	acl = get_cached_richacl(inode);
	if (acl != ACL_NOT_CACHED)
		return acl;

	if (!IS_RICHACL(inode))
		return NULL;

	/*
	 * A filesystem can force a ACL callback by just never filling the
	 * ACL cache. But normally you'd fill the cache either at inode
	 * instantiation time, or on the first ->get_richacl call.
	 *
	 * If the filesystem doesn't have a get_richacl() function at all,
	 * we'll just create the negative cache entry.
	 */
	if (!inode->i_op->get_richacl) {
		set_cached_richacl(inode, NULL);
		return NULL;
	}
	return inode->i_op->get_richacl(inode);
}
EXPORT_SYMBOL_GPL(get_richacl);

/**
 * richacl_permission  -  richacl permission check algorithm
 * @inode:	inode to check
 * @acl:	rich acl of the inode
 * @want:	requested access (MAY_* flags)
 *
 * Checks if the current process is granted @mask flags in @acl.
 */
int
richacl_permission(struct inode *inode, const struct richacl *acl,
		   int want)
{
	const struct richace *ace;
	unsigned int mask = richacl_want_to_mask(want);
	unsigned int requested = mask, denied = 0;
	int in_owning_group = in_group_p(inode->i_gid);
	int in_owner_or_group_class = in_owning_group;

	/*
	 * A process is
	 *   - in the owner file class if it owns the file,
	 *   - in the group file class if it is in the file's owning group or
	 *     it matches any of the user or group entries, and
	 *   - in the other file class otherwise.
	 * The file class is only relevant for determining which file mask to
	 * apply, which only happens for masked acls.
	 */
	if (acl->a_flags & RICHACL_MASKED) {
		if ((acl->a_flags & RICHACL_WRITE_THROUGH) &&
		    uid_eq(current_fsuid(), inode->i_uid)) {
			denied = requested & ~acl->a_owner_mask;
			goto out;
		}
	} else {
		/*
		 * When the acl is not masked, there is no need to determine if
		 * the process is in the group class and we can break out
		 * earlier of the loop below.
		 */
		in_owner_or_group_class = 1;
	}

	/*
	 * Check if the acl grants the requested access and determine which
	 * file class the process is in.
	 */
	richacl_for_each_entry(ace, acl) {
		unsigned int ace_mask = ace->e_mask;

		if (richace_is_inherit_only(ace))
			continue;
		if (richace_is_owner(ace)) {
			if (!uid_eq(current_fsuid(), inode->i_uid))
				continue;
			goto entry_matches_owner;
		} else if (richace_is_group(ace)) {
			if (!in_owning_group)
				continue;
		} else if (richace_is_unix_user(ace)) {
			if (!uid_eq(current_fsuid(), ace->e_id.uid))
				continue;
			if (uid_eq(current_fsuid(), inode->i_uid))
				goto entry_matches_owner;
		} else if (richace_is_unix_group(ace)) {
			if (!in_group_p(ace->e_id.gid))
				continue;
		} else
			goto entry_matches_everyone;

		/*
		 * Apply the group file mask to entries other than owner@ and
		 * everyone@ or user entries matching the owner.  This ensures
		 * that we grant the same permissions as the acl computed by
		 * richacl_apply_masks().
		 *
		 * Without this restriction, the following richacl would grant
		 * rw access to processes which are both the owner and in the
		 * owning group, but not to other users in the owning group,
		 * which could not be represented without masks:
		 *
		 *  owner:rw::mask
		 *  group@:rw::allow
		 */
		if ((acl->a_flags & RICHACL_MASKED) && richace_is_allow(ace))
			ace_mask &= acl->a_group_mask;

entry_matches_owner:
		/* The process is in the owner or group file class. */
		in_owner_or_group_class = 1;

entry_matches_everyone:
		/* Check which mask flags the ACE allows or denies. */
		if (richace_is_deny(ace))
			denied |= ace_mask & mask;
		mask &= ~ace_mask;

		/*
		 * Keep going until we know which file class
		 * the process is in.
		 */
		if (!mask && in_owner_or_group_class)
			break;
	}
	denied |= mask;

	if (acl->a_flags & RICHACL_MASKED) {
		/*
		 * The file class a process is in determines which file mask
		 * applies.  Check if that file mask also grants the requested
		 * access.
		 */
		if (uid_eq(current_fsuid(), inode->i_uid))
			denied |= requested & ~acl->a_owner_mask;
		else if (in_owner_or_group_class)
			denied |= requested & ~acl->a_group_mask;
		else {
			if (acl->a_flags & RICHACL_WRITE_THROUGH)
				denied = requested & ~acl->a_other_mask;
			else
				denied |= requested & ~acl->a_other_mask;
		}
	}

out:
	return denied ? -EACCES : 0;
}
EXPORT_SYMBOL_GPL(richacl_permission);

/**
 * richacl_chmod  -  filesystem chmod helper
 * @inode:	inode whose file permission bits to change
 * @mode:	new file permission bits including the file type
 *
 * Helper for filesystems to use to perform a chmod on the richacl of an inode.
 */
int
richacl_chmod(struct inode *inode, umode_t mode)
{
	struct richacl *acl;
	int retval;

	if (S_ISLNK(mode))
		return -EOPNOTSUPP;
	if (!inode->i_op->set_richacl)
		return -EOPNOTSUPP;
	acl = get_richacl(inode);
	if (IS_ERR_OR_NULL(acl))
		return PTR_ERR(acl);
	acl = __richacl_chmod(acl, mode);
	if (IS_ERR(acl))
		return PTR_ERR(acl);
	retval = inode->i_op->set_richacl(inode, acl);
	richacl_put(acl);

	return retval;
}
EXPORT_SYMBOL(richacl_chmod);

/*
 * richacl_inherit_inode  -  compute inherited acl and file mode
 * @dir_acl:	acl of the containing directory
 * @mode_p:	mode of the new inode
 *
 * The file permission bits in @mode_p must be set to the create mode by the
 * caller.
 *
 * If there is an inheritable acl, the maximum permissions that the acl grants
 * are computed and the file masks of the new acl are set accordingly.
 */
static struct richacl *
richacl_inherit_inode(const struct richacl *dir_acl, umode_t *mode_p)
{
	struct richacl *acl;
	umode_t mode = *mode_p;

	acl = richacl_inherit(dir_acl, S_ISDIR(mode));
	if (acl) {
		if (richacl_equiv_mode(acl, &mode) == 0) {
			*mode_p &= mode;
			richacl_put(acl);
			acl = NULL;
		} else {
			richacl_compute_max_masks(acl);
			/*
			 * Ensure that the acl will not grant any permissions
			 * beyond the create mode.
			 */
			acl->a_flags |= RICHACL_MASKED;
			acl->a_owner_mask &=
				richacl_mode_to_mask(mode >> 6);
			acl->a_group_mask &=
				richacl_mode_to_mask(mode >> 3);
			acl->a_other_mask &=
				richacl_mode_to_mask(mode);
		}
	} else
		*mode_p &= ~current_umask();

	return acl;
}

/**
 * richacl_create  -  filesystem create helper
 * @mode_p:	mode of the new inode
 * @dir:	containing directory
 *
 * Compute the inherited acl for a new inode.  If there is no acl to inherit,
 * apply the umask.  Use when creating a new inode on a richacl enabled file
 * system.
 */
struct richacl *richacl_create(umode_t *mode_p, struct inode *dir)
{
	struct richacl *dir_acl, *acl = NULL;

	if (S_ISLNK(*mode_p))
		return NULL;
	dir_acl = get_richacl(dir);
	if (dir_acl) {
		if (IS_ERR(dir_acl))
			return dir_acl;
		acl = richacl_inherit_inode(dir_acl, mode_p);
		richacl_put(dir_acl);
	} else
		*mode_p &= ~current_umask();
	return acl;
}
EXPORT_SYMBOL_GPL(richacl_create);

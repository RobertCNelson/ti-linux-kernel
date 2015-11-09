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
		base_acl_init(&acl->a_base);
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
		base_acl_init(&dup->a_base);
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

/*
 * Note: functions like richacl_allowed_to_who(), richacl_group_class_allowed(),
 * and richacl_compute_max_masks() iterate through the entire acl in reverse
 * order as an optimization.
 *
 * In the standard algorithm, aces are considered in forward order.  When a
 * process matches an ace, the permissions in the ace are either allowed or
 * denied depending on the ace type.  Once a permission has been allowed or
 * denied, it is no longer considered in further aces.
 *
 * By iterating through the acl in reverse order, we can compute the same
 * result without having to keep track of which permissions have been allowed
 * and denied already.
 */

/**
 * richacl_allowed_to_who  -  permissions allowed to a specific who value
 *
 * Compute the maximum mask values allowed to a specific who value, taking
 * everyone@ aces into account.
 */
static unsigned int richacl_allowed_to_who(struct richacl *acl,
					   struct richace *who)
{
	struct richace *ace;
	unsigned int allowed = 0;

	richacl_for_each_entry_reverse(ace, acl) {
		if (richace_is_inherit_only(ace))
			continue;
		if (richace_is_same_identifier(ace, who) ||
		    richace_is_everyone(ace)) {
			if (richace_is_allow(ace))
				allowed |= ace->e_mask;
			else if (richace_is_deny(ace))
				allowed &= ~ace->e_mask;
		}
	}
	return allowed;
}

/**
 * richacl_group_class_allowed  -  maximum permissions of the group class
 *
 * Compute the maximum mask values allowed to a process in the group class
 * (i.e., a process which is not the owner but is in the owning group or
 * matches a user or group acl entry).  This includes permissions granted or
 * denied by everyone@ aces.
 *
 * See richacl_compute_max_masks().
 */
static unsigned int richacl_group_class_allowed(struct richacl *acl)
{
	struct richace *ace;
	unsigned int everyone_allowed = 0, group_class_allowed = 0;
	int had_group_ace = 0;

	richacl_for_each_entry_reverse(ace, acl) {
		if (richace_is_inherit_only(ace) ||
		    richace_is_owner(ace))
			continue;

		if (richace_is_everyone(ace)) {
			if (richace_is_allow(ace))
				everyone_allowed |= ace->e_mask;
			else if (richace_is_deny(ace))
				everyone_allowed &= ~ace->e_mask;
		} else {
			group_class_allowed |=
				richacl_allowed_to_who(acl, ace);

			if (richace_is_group(ace))
				had_group_ace = 1;
		}
	}
	/*
	 * If the acl doesn't contain any group@ aces, richacl_allowed_to_who()
	 * wasn't called for the owning group.  We could make that call now, but
	 * we already know the result (everyone_allowed).
	 */
	if (!had_group_ace)
		group_class_allowed |= everyone_allowed;
	return group_class_allowed;
}

/**
 * richacl_compute_max_masks  -  compute upper bound masks
 *
 * Computes upper bound owner, group, and other masks so that none of the
 * permissions allowed by the acl are disabled.
 *
 * We don't make assumptions about who the owner is so that the owner can
 * change with no effect on the file masks or file mode permission bits; this
 * means that we must assume that all entries can match the owner.
 */
void richacl_compute_max_masks(struct richacl *acl)
{
	unsigned int gmask = ~0;
	struct richace *ace;

	/*
	 * @gmask contains all permissions which the group class is ever
	 * allowed.  We use it to avoid adding permissions to the group mask
	 * from everyone@ allow aces which the group class is always denied
	 * through other aces.  For example, the following acl would otherwise
	 * result in a group mask of rw:
	 *
	 *	group@:w::deny
	 *	everyone@:rw::allow
	 *
	 * Avoid computing @gmask for acls which do not include any group class
	 * deny aces: in such acls, the group class is never denied any
	 * permissions from everyone@ allow aces, and the group class cannot
	 * have fewer permissions than the other class.
	 */

restart:
	acl->a_owner_mask = 0;
	acl->a_group_mask = 0;
	acl->a_other_mask = 0;

	richacl_for_each_entry_reverse(ace, acl) {
		if (richace_is_inherit_only(ace))
			continue;

		if (richace_is_owner(ace)) {
			if (richace_is_allow(ace))
				acl->a_owner_mask |= ace->e_mask;
			else if (richace_is_deny(ace))
				acl->a_owner_mask &= ~ace->e_mask;
		} else if (richace_is_everyone(ace)) {
			if (richace_is_allow(ace)) {
				acl->a_owner_mask |= ace->e_mask;
				acl->a_group_mask |= ace->e_mask & gmask;
				acl->a_other_mask |= ace->e_mask;
			} else if (richace_is_deny(ace)) {
				acl->a_owner_mask &= ~ace->e_mask;
				acl->a_group_mask &= ~ace->e_mask;
				acl->a_other_mask &= ~ace->e_mask;
			}
		} else {
			if (richace_is_allow(ace)) {
				acl->a_owner_mask |= ace->e_mask & gmask;
				acl->a_group_mask |= ace->e_mask & gmask;
			} else if (richace_is_deny(ace) && gmask == ~0) {
				gmask = richacl_group_class_allowed(acl);
				if (likely(gmask != ~0))
					/* should always be true */
					goto restart;
			}
		}
	}

	acl->a_flags &= ~(RICHACL_WRITE_THROUGH | RICHACL_MASKED);
}
EXPORT_SYMBOL_GPL(richacl_compute_max_masks);

/**
 * __richacl_chmod  -  update the file masks to reflect the new mode
 * @acl:	access control list
 * @mode:	new file permission bits including the file type
 *
 * Return a copy of @acl where the file masks have been replaced by the file
 * masks corresponding to the file permission bits in @mode, or returns @acl
 * itself if the file masks are already up to date.  Takes over a reference
 * to @acl.
 */
struct richacl *
__richacl_chmod(struct richacl *acl, umode_t mode)
{
	unsigned int x = S_ISDIR(mode) ? 0 : RICHACE_DELETE_CHILD;
	unsigned int owner_mask, group_mask, other_mask;
	struct richacl *clone;

	owner_mask = richacl_mode_to_mask(mode >> 6) & ~x;
	group_mask = richacl_mode_to_mask(mode >> 3) & ~x;
	other_mask = richacl_mode_to_mask(mode)      & ~x;

	if (acl->a_owner_mask == owner_mask &&
	    acl->a_group_mask == group_mask &&
	    acl->a_other_mask == other_mask &&
	    (acl->a_flags & RICHACL_MASKED) &&
	    (acl->a_flags & RICHACL_WRITE_THROUGH) &&
	    (!richacl_is_auto_inherit(acl) || richacl_is_protected(acl)))
		return acl;

	clone = richacl_clone(acl, GFP_KERNEL);
	richacl_put(acl);
	if (!clone)
		return ERR_PTR(-ENOMEM);

	clone->a_flags |= (RICHACL_WRITE_THROUGH | RICHACL_MASKED);
	clone->a_owner_mask = owner_mask;
	clone->a_group_mask = group_mask;
	clone->a_other_mask = other_mask;
	if (richacl_is_auto_inherit(clone))
		clone->a_flags |= RICHACL_PROTECTED;

	return clone;
}
EXPORT_SYMBOL_GPL(__richacl_chmod);

/**
 * richacl_equiv_mode  -  compute the mode equivalent of @acl
 *
 * An acl is considered equivalent to a file mode if it only consists of
 * owner@, group@, and everyone@ entries and the owner@ permissions do not
 * depend on whether the owner is a member in the owning group.
 */
int
richacl_equiv_mode(const struct richacl *acl, umode_t *mode_p)
{
	umode_t mode = *mode_p;

	/*
	 * The RICHACE_DELETE_CHILD flag is meaningless for non-directories, so
	 * we ignore it.
	 */
	unsigned int x = S_ISDIR(mode) ? 0 : RICHACE_DELETE_CHILD;
	struct {
		unsigned int allowed;
		unsigned int defined;  /* allowed or denied */
	} owner = {
		.defined = RICHACE_POSIX_ALWAYS_ALLOWED |
			   RICHACE_POSIX_OWNER_ALLOWED  | x,
	}, group = {
		.defined = RICHACE_POSIX_ALWAYS_ALLOWED | x,
	}, everyone = {
		.defined = RICHACE_POSIX_ALWAYS_ALLOWED | x,
	};
	const struct richace *ace;

	if (acl->a_flags & ~(RICHACL_WRITE_THROUGH | RICHACL_MASKED))
		return -1;

	richacl_for_each_entry(ace, acl) {
		if (ace->e_flags & ~RICHACE_SPECIAL_WHO)
			return -1;

		if (richace_is_owner(ace) || richace_is_everyone(ace)) {
			x = ace->e_mask & ~owner.defined;
			if (richace_is_allow(ace)) {
				unsigned int group_denied =
					group.defined & ~group.allowed;

				if (x & group_denied)
					return -1;
				owner.allowed |= x;
			} else /* if (richace_is_deny(ace)) */ {
				if (x & group.allowed)
					return -1;
			}
			owner.defined |= x;

			if (richace_is_everyone(ace)) {
				x = ace->e_mask;
				if (richace_is_allow(ace)) {
					group.allowed |=
						x & ~group.defined;
					everyone.allowed |=
						x & ~everyone.defined;
				}
				group.defined |= x;
				everyone.defined |= x;
			}
		} else if (richace_is_group(ace)) {
			x = ace->e_mask & ~group.defined;
			if (richace_is_allow(ace))
				group.allowed |= x;
			group.defined |= x;
		} else
			return -1;
	}

	if (group.allowed & ~owner.defined)
		return -1;

	if (acl->a_flags & RICHACL_MASKED) {
		if (acl->a_flags & RICHACL_WRITE_THROUGH) {
			owner.allowed = acl->a_owner_mask;
			everyone.allowed = acl->a_other_mask;
		} else {
			owner.allowed &= acl->a_owner_mask;
			everyone.allowed &= acl->a_other_mask;
		}
		group.allowed &= acl->a_group_mask;
	}

	mode = (mode & ~S_IRWXUGO) |
	       (richacl_mask_to_mode(owner.allowed) << 6) |
	       (richacl_mask_to_mode(group.allowed) << 3) |
		richacl_mask_to_mode(everyone.allowed);

	/* Mask flags we can ignore */
	x = S_ISDIR(mode) ? 0 : RICHACE_DELETE_CHILD;

	if (((richacl_mode_to_mask(mode >> 6) ^ owner.allowed)    & ~x) ||
	    ((richacl_mode_to_mask(mode >> 3) ^ group.allowed)    & ~x) ||
	    ((richacl_mode_to_mask(mode)      ^ everyone.allowed) & ~x))
		return -1;

	*mode_p = mode;
	return 0;
}
EXPORT_SYMBOL_GPL(richacl_equiv_mode);

/**
 * richacl_inherit  -  compute the inherited acl of a new file
 * @dir_acl:	acl of the containing directory
 * @isdir:	inherit by a directory or non-directory?
 *
 * A directory can have acl entries which files and/or directories created
 * inside the directory will inherit.  This function computes the acl for such
 * a new file.  If there is no inheritable acl, it will return %NULL.
 */
struct richacl *
richacl_inherit(const struct richacl *dir_acl, int isdir)
{
	const struct richace *dir_ace;
	struct richacl *acl = NULL;
	struct richace *ace;
	int count = 0;

	if (isdir) {
		richacl_for_each_entry(dir_ace, dir_acl) {
			if (!richace_is_inheritable(dir_ace))
				continue;
			count++;
		}
		if (!count)
			return NULL;
		acl = richacl_alloc(count, GFP_KERNEL);
		if (!acl)
			return ERR_PTR(-ENOMEM);
		ace = acl->a_entries;
		richacl_for_each_entry(dir_ace, dir_acl) {
			if (!richace_is_inheritable(dir_ace))
				continue;
			richace_copy(ace, dir_ace);
			if (dir_ace->e_flags & RICHACE_NO_PROPAGATE_INHERIT_ACE)
				ace->e_flags &= ~RICHACE_INHERITANCE_FLAGS;
			else if (!(dir_ace->e_flags & RICHACE_DIRECTORY_INHERIT_ACE))
				ace->e_flags |= RICHACE_INHERIT_ONLY_ACE;
			ace++;
		}
	} else {
		richacl_for_each_entry(dir_ace, dir_acl) {
			if (!(dir_ace->e_flags & RICHACE_FILE_INHERIT_ACE))
				continue;
			count++;
		}
		if (!count)
			return NULL;
		acl = richacl_alloc(count, GFP_KERNEL);
		if (!acl)
			return ERR_PTR(-ENOMEM);
		ace = acl->a_entries;
		richacl_for_each_entry(dir_ace, dir_acl) {
			if (!(dir_ace->e_flags & RICHACE_FILE_INHERIT_ACE))
				continue;
			richace_copy(ace, dir_ace);
			ace->e_flags &= ~RICHACE_INHERITANCE_FLAGS;
			/*
			 * RICHACE_DELETE_CHILD is meaningless for
			 * non-directories, so clear it.
			 */
			ace->e_mask &= ~RICHACE_DELETE_CHILD;
			ace++;
		}
	}
	if (richacl_is_auto_inherit(dir_acl)) {
		acl->a_flags = RICHACL_AUTO_INHERIT;
		richacl_for_each_entry(ace, acl)
			ace->e_flags |= RICHACE_INHERITED_ACE;
	}

	return acl;
}

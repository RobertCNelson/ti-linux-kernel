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
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/xattr.h>
#include <linux/richacl_xattr.h>
#include <uapi/linux/xattr.h>

MODULE_LICENSE("GPL");

/**
 * richacl_from_xattr  -  convert a richacl xattr into the in-memory representation
 */
struct richacl *
richacl_from_xattr(struct user_namespace *user_ns,
		   const void *value, size_t size)
{
	const struct richacl_xattr *xattr_acl = value;
	const struct richace_xattr *xattr_ace = (void *)(xattr_acl + 1);
	struct richacl *acl;
	struct richace *ace;
	int count;

	if (size < sizeof(*xattr_acl) ||
	    xattr_acl->a_version != RICHACL_XATTR_VERSION ||
	    (xattr_acl->a_flags & ~RICHACL_VALID_FLAGS))
		return ERR_PTR(-EINVAL);
	size -= sizeof(*xattr_acl);
	count = le16_to_cpu(xattr_acl->a_count);
	if (count > RICHACL_XATTR_MAX_COUNT)
		return ERR_PTR(-EINVAL);
	if (size != count * sizeof(*xattr_ace))
		return ERR_PTR(-EINVAL);

	acl = richacl_alloc(count, GFP_NOFS);
	if (!acl)
		return ERR_PTR(-ENOMEM);

	acl->a_flags = xattr_acl->a_flags;
	acl->a_owner_mask = le32_to_cpu(xattr_acl->a_owner_mask);
	if (acl->a_owner_mask & ~RICHACE_VALID_MASK)
		goto fail_einval;
	acl->a_group_mask = le32_to_cpu(xattr_acl->a_group_mask);
	if (acl->a_group_mask & ~RICHACE_VALID_MASK)
		goto fail_einval;
	acl->a_other_mask = le32_to_cpu(xattr_acl->a_other_mask);
	if (acl->a_other_mask & ~RICHACE_VALID_MASK)
		goto fail_einval;

	richacl_for_each_entry(ace, acl) {
		ace->e_type  = le16_to_cpu(xattr_ace->e_type);
		ace->e_flags = le16_to_cpu(xattr_ace->e_flags);
		ace->e_mask  = le32_to_cpu(xattr_ace->e_mask);

		if (ace->e_flags & ~RICHACE_VALID_FLAGS)
			goto fail_einval;
		if (ace->e_flags & RICHACE_SPECIAL_WHO) {
			ace->e_id.special = le32_to_cpu(xattr_ace->e_id);
			if (ace->e_id.special > RICHACE_EVERYONE_SPECIAL_ID)
				goto fail_einval;
		} else if (ace->e_flags & RICHACE_IDENTIFIER_GROUP) {
			u32 id = le32_to_cpu(xattr_ace->e_id);

			ace->e_id.gid = make_kgid(user_ns, id);
			if (!gid_valid(ace->e_id.gid))
				goto fail_einval;
		} else {
			u32 id = le32_to_cpu(xattr_ace->e_id);

			ace->e_id.uid = make_kuid(user_ns, id);
			if (!uid_valid(ace->e_id.uid))
				goto fail_einval;
		}
		if (ace->e_type > RICHACE_ACCESS_DENIED_ACE_TYPE ||
		    (ace->e_mask & ~RICHACE_VALID_MASK))
			goto fail_einval;

		xattr_ace++;
	}

	return acl;

fail_einval:
	richacl_put(acl);
	return ERR_PTR(-EINVAL);
}
EXPORT_SYMBOL_GPL(richacl_from_xattr);

/**
 * richacl_xattr_size  -  compute the size of the xattr representation of @acl
 */
size_t
richacl_xattr_size(const struct richacl *acl)
{
	size_t size = sizeof(struct richacl_xattr);

	size += sizeof(struct richace_xattr) * acl->a_count;
	return size;
}
EXPORT_SYMBOL_GPL(richacl_xattr_size);

/**
 * richacl_to_xattr  -  convert @acl into its xattr representation
 * @acl:	the richacl to convert
 * @buffer:	buffer for the result
 * @size:	size of @buffer
 */
int
richacl_to_xattr(struct user_namespace *user_ns,
		 const struct richacl *acl, void *buffer, size_t size)
{
	struct richacl_xattr *xattr_acl = buffer;
	struct richace_xattr *xattr_ace;
	const struct richace *ace;
	size_t real_size;

	real_size = richacl_xattr_size(acl);
	if (!buffer)
		return real_size;
	if (real_size > size)
		return -ERANGE;

	xattr_acl->a_version = RICHACL_XATTR_VERSION;
	xattr_acl->a_flags = acl->a_flags;
	xattr_acl->a_count = cpu_to_le16(acl->a_count);

	xattr_acl->a_owner_mask = cpu_to_le32(acl->a_owner_mask);
	xattr_acl->a_group_mask = cpu_to_le32(acl->a_group_mask);
	xattr_acl->a_other_mask = cpu_to_le32(acl->a_other_mask);

	xattr_ace = (void *)(xattr_acl + 1);
	richacl_for_each_entry(ace, acl) {
		xattr_ace->e_type = cpu_to_le16(ace->e_type);
		xattr_ace->e_flags = cpu_to_le16(ace->e_flags);
		xattr_ace->e_mask = cpu_to_le32(ace->e_mask);
		if (ace->e_flags & RICHACE_SPECIAL_WHO)
			xattr_ace->e_id = cpu_to_le32(ace->e_id.special);
		else if (ace->e_flags & RICHACE_IDENTIFIER_GROUP)
			xattr_ace->e_id =
				cpu_to_le32(from_kgid(user_ns, ace->e_id.gid));
		else
			xattr_ace->e_id =
				cpu_to_le32(from_kuid(user_ns, ace->e_id.uid));
		xattr_ace++;
	}
	return real_size;
}
EXPORT_SYMBOL_GPL(richacl_to_xattr);

static size_t
richacl_xattr_list(const struct xattr_handler *handler,
		   struct dentry *dentry, char *list, size_t list_len,
		   const char *name, size_t name_len)
{
	const size_t size = sizeof(XATTR_NAME_RICHACL);

	if (!IS_RICHACL(d_backing_inode(dentry)))
		return 0;
	if (list && size <= list_len)
		memcpy(list, XATTR_NAME_RICHACL, size);
	return size;
}

static int
richacl_xattr_get(const struct xattr_handler *handler,
		  struct dentry *dentry, const char *name, void *buffer,
		  size_t buffer_size)
{
	struct inode *inode = d_backing_inode(dentry);
	struct richacl *acl;
	int error;

	if (strcmp(name, "") != 0)
		return -EINVAL;
	if (!IS_RICHACL(inode))
		return EOPNOTSUPP;
	if (S_ISLNK(inode->i_mode))
		return -EOPNOTSUPP;
	acl = get_richacl(inode);
	if (IS_ERR(acl))
		return PTR_ERR(acl);
	if (acl == NULL)
		return -ENODATA;
	error = richacl_to_xattr(&init_user_ns, acl, buffer, buffer_size);
	richacl_put(acl);
	return error;
}

static int
richacl_xattr_set(const struct xattr_handler *handler,
		  struct dentry *dentry, const char *name,
		  const void *value, size_t size, int flags)
{
	struct inode *inode = d_backing_inode(dentry);
	struct richacl *acl = NULL;
	int ret;

	if (strcmp(name, "") != 0)
		return -EINVAL;
	if (!IS_RICHACL(inode))
		return -EOPNOTSUPP;
	if (!inode->i_op->set_richacl)
		return -EOPNOTSUPP;

	if (!uid_eq(current_fsuid(), inode->i_uid) &&
	    inode_permission(inode, MAY_CHMOD) &&
	    !capable(CAP_FOWNER))
		return -EPERM;

	if (value) {
		acl = richacl_from_xattr(&init_user_ns, value, size);
		if (IS_ERR(acl))
			return PTR_ERR(acl);
	}

	ret = inode->i_op->set_richacl(inode, acl);
	richacl_put(acl);
	return ret;
}

struct xattr_handler richacl_xattr_handler = {
	.prefix = XATTR_NAME_RICHACL,
	.list = richacl_xattr_list,
	.get = richacl_xattr_get,
	.set = richacl_xattr_set,
};
EXPORT_SYMBOL(richacl_xattr_handler);

/*
 * Fix up the uids and gids in richacl extended attributes in place.
 */
static void richacl_fix_xattr_userns(
	struct user_namespace *to, struct user_namespace *from,
	void *value, size_t size)
{
	struct richacl_xattr *xattr_acl = value;
	struct richace_xattr *xattr_ace =
		(struct richace_xattr *)(xattr_acl + 1);
	unsigned int count;

	if (!value)
		return;
	if (size < sizeof(*xattr_acl))
		return;
	if (xattr_acl->a_version != cpu_to_le32(RICHACL_XATTR_VERSION))
		return;
	size -= sizeof(*xattr_acl);
	if (size % sizeof(*xattr_ace))
		return;
	count = size / sizeof(*xattr_ace);
	for (; count; count--, xattr_ace++) {
		if (xattr_ace->e_flags & cpu_to_le16(RICHACE_SPECIAL_WHO))
			continue;
		if (xattr_ace->e_flags &
		    cpu_to_le16(RICHACE_IDENTIFIER_GROUP)) {
			u32 id = le32_to_cpu(xattr_ace->e_id);
			kgid_t gid = make_kgid(from, id);

			xattr_ace->e_id = cpu_to_le32(from_kgid(to, gid));
		} else {
			u32 id = le32_to_cpu(xattr_ace->e_id);
			kuid_t uid = make_kuid(from, id);

			xattr_ace->e_id = cpu_to_le32(from_kuid(to, uid));
		}
	}
}

void richacl_fix_xattr_from_user(void *value, size_t size)
{
	struct user_namespace *user_ns = current_user_ns();

	if (user_ns == &init_user_ns)
		return;
	richacl_fix_xattr_userns(&init_user_ns, user_ns, value, size);
}

void richacl_fix_xattr_to_user(void *value, size_t size)
{
	struct user_namespace *user_ns = current_user_ns();

	if (user_ns == &init_user_ns)
		return;
	richacl_fix_xattr_userns(user_ns, &init_user_ns, value, size);
}

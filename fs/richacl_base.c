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

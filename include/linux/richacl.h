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

#ifndef __RICHACL_H
#define __RICHACL_H

#include <uapi/linux/richacl.h>

struct richace {
	unsigned short	e_type;
	unsigned short	e_flags;
	unsigned int	e_mask;
	union {
		kuid_t		uid;
		kgid_t		gid;
		unsigned int	special;
	} e_id;
};

struct richacl {
	atomic_t	a_refcount;
	unsigned int	a_owner_mask;
	unsigned int	a_group_mask;
	unsigned int	a_other_mask;
	unsigned short	a_count;
	unsigned short	a_flags;
	struct richace	a_entries[0];
};

#define richacl_for_each_entry(_ace, _acl)			\
	for (_ace = (_acl)->a_entries;				\
	     _ace != (_acl)->a_entries + (_acl)->a_count;	\
	     _ace++)

#define richacl_for_each_entry_reverse(_ace, _acl)		\
	for (_ace = (_acl)->a_entries + (_acl)->a_count - 1;	\
	     _ace != (_acl)->a_entries - 1;			\
	     _ace--)

/**
 * richacl_get  -  grab another reference to a richacl handle
 */
static inline struct richacl *
richacl_get(struct richacl *acl)
{
	if (acl)
		atomic_inc(&acl->a_refcount);
	return acl;
}

/**
 * richacl_put  -  free a richacl handle
 */
static inline void
richacl_put(struct richacl *acl)
{
	if (acl && atomic_dec_and_test(&acl->a_refcount))
		kfree(acl);
}

/**
 * richace_is_owner  -  check if @ace is an OWNER@ entry
 */
static inline bool
richace_is_owner(const struct richace *ace)
{
	return (ace->e_flags & RICHACE_SPECIAL_WHO) &&
	       ace->e_id.special == RICHACE_OWNER_SPECIAL_ID;
}

/**
 * richace_is_group  -  check if @ace is a GROUP@ entry
 */
static inline bool
richace_is_group(const struct richace *ace)
{
	return (ace->e_flags & RICHACE_SPECIAL_WHO) &&
	       ace->e_id.special == RICHACE_GROUP_SPECIAL_ID;
}

/**
 * richace_is_everyone  -  check if @ace is an EVERYONE@ entry
 */
static inline bool
richace_is_everyone(const struct richace *ace)
{
	return (ace->e_flags & RICHACE_SPECIAL_WHO) &&
	       ace->e_id.special == RICHACE_EVERYONE_SPECIAL_ID;
}

/**
 * richace_is_unix_user  -  check if @ace applies to a specific user
 */
static inline bool
richace_is_unix_user(const struct richace *ace)
{
	return !(ace->e_flags & RICHACE_SPECIAL_WHO) &&
	       !(ace->e_flags & RICHACE_IDENTIFIER_GROUP);
}

/**
 * richace_is_unix_group  -  check if @ace applies to a specific group
 */
static inline bool
richace_is_unix_group(const struct richace *ace)
{
	return !(ace->e_flags & RICHACE_SPECIAL_WHO) &&
	       (ace->e_flags & RICHACE_IDENTIFIER_GROUP);
}

/**
 * richace_is_inherit_only  -  check if @ace is for inheritance only
 *
 * ACEs with the %RICHACE_INHERIT_ONLY_ACE flag set have no effect during
 * permission checking.
 */
static inline bool
richace_is_inherit_only(const struct richace *ace)
{
	return ace->e_flags & RICHACE_INHERIT_ONLY_ACE;
}

/**
 * richace_is_inheritable  -  check if @ace is inheritable
 */
static inline bool
richace_is_inheritable(const struct richace *ace)
{
	return ace->e_flags & (RICHACE_FILE_INHERIT_ACE |
			       RICHACE_DIRECTORY_INHERIT_ACE);
}

/**
 * richace_is_allow  -  check if @ace is an %ALLOW type entry
 */
static inline bool
richace_is_allow(const struct richace *ace)
{
	return ace->e_type == RICHACE_ACCESS_ALLOWED_ACE_TYPE;
}

/**
 * richace_is_deny  -  check if @ace is a %DENY type entry
 */
static inline bool
richace_is_deny(const struct richace *ace)
{
	return ace->e_type == RICHACE_ACCESS_DENIED_ACE_TYPE;
}

/**
 * richace_is_same_identifier  -  are both identifiers the same?
 */
static inline bool
richace_is_same_identifier(const struct richace *a, const struct richace *b)
{
	return !((a->e_flags ^ b->e_flags) &
		 (RICHACE_SPECIAL_WHO | RICHACE_IDENTIFIER_GROUP)) &&
	       !memcmp(&a->e_id, &b->e_id, sizeof(a->e_id));
}

extern struct richacl *richacl_alloc(int, gfp_t);
extern struct richacl *richacl_clone(const struct richacl *, gfp_t);
extern void richace_copy(struct richace *, const struct richace *);

#endif /* __RICHACL_H */

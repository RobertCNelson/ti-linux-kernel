/*
 * Copyright IBM Corporation, 2010
 * Copyright (C)  2015 Red Hat, Inc.
 * Author Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#ifndef __FS_EXT4_RICHACL_H
#define __FS_EXT4_RICHACL_H

#include <linux/richacl.h>

#ifdef CONFIG_EXT4_FS_RICHACL

extern struct richacl *ext4_get_richacl(struct inode *);
extern int ext4_set_richacl(struct inode *, struct richacl *);

extern int ext4_init_richacl(handle_t *, struct inode *, struct inode *);

#else  /* CONFIG_EXT4_FS_RICHACL */

#define ext4_get_richacl NULL
#define ext4_set_richacl NULL

static inline int
ext4_init_richacl(handle_t *handle, struct inode *inode, struct inode *dir)
{
	return 0;
}

#endif  /* CONFIG_EXT4_FS_RICHACL */
#endif  /* __FS_EXT4_RICHACL_H */

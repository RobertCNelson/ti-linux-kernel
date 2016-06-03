/*
 * Copyright (c) 2016 Trond Myklebust
 *
 * I/O and data path helper functionality.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/rwsem.h>
#include <linux/fs.h>
#include <linux/nfs_fs.h>

#include "internal.h"

void
nfs_lock_bio(struct nfs_inode *nfsi)
{
	/* Be an optimist! */
	down_read(&nfsi->io_lock);
	if (test_bit(NFS_INO_ODIRECT, &nfsi->flags) == 0)
		return;
	up_read(&nfsi->io_lock);
	/* Slow path.... */
	down_write(&nfsi->io_lock);
	clear_bit(NFS_INO_ODIRECT, &nfsi->flags);
	downgrade_write(&nfsi->io_lock);
}

void
nfs_unlock_bio(struct nfs_inode *nfsi)
{
	up_read(&nfsi->io_lock);
}

void
nfs_lock_dio(struct nfs_inode *nfsi)
{
	/* Be an optimist! */
	down_read(&nfsi->io_lock);
	if (test_bit(NFS_INO_ODIRECT, &nfsi->flags) != 0)
		return;
	up_read(&nfsi->io_lock);
	/* Slow path.... */
	down_write(&nfsi->io_lock);
	set_bit(NFS_INO_ODIRECT, &nfsi->flags);
	downgrade_write(&nfsi->io_lock);
}

void
nfs_unlock_dio(struct nfs_inode *nfsi)
{
	up_read(&nfsi->io_lock);
}

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

/**
 * nfs_start_io_buffered - declare the file is being used for buffered i/o
 * @nfsi - nfs_inode of the file
 *
 * Declare that a buffered I/O operation is about to start, and ensure
 * that we block all direct I/O.
 * On exit, the function ensures that the NFS_INO_ODIRECT flag is unset,
 * and holds a shared lock on nfsi->io_lock to ensure that the flag
 * cannot be changed.
 * In practice, this means that buffered I/O operations are allowed to
 * execute in parallel, thanks to the shared lock, whereas direct I/O
 * operations need to wait to grab an exclusive lock in order to set
 * NFS_INO_ODIRECT.
 */
void
nfs_start_io_buffered(struct nfs_inode *nfsi)
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

/**
 * nfs_end_io_buffered - declare that the buffered i/o operation is done
 * @nfsi - nfs_inode of the file
 *
 * Declare that a buffered I/O operation is done, and release the shared
 * lock on nfsi->io_lock.
 */
void
nfs_end_io_buffered(struct nfs_inode *nfsi)
{
	up_read(&nfsi->io_lock);
}

/**
 * nfs_end_io_direct - declare the file is being used for direct i/o
 * @nfsi - nfs_inode of the file
 *
 * Declare that a direct I/O operation is about to start, and ensure
 * that we block all buffered I/O.
 * On exit, the function ensures that the NFS_INO_ODIRECT flag is set,
 * and holds a shared lock on nfsi->io_lock to ensure that the flag
 * cannot be changed.
 * In practice, this means that direct I/O operations are allowed to
 * execute in parallel, thanks to the shared lock, whereas buffered I/O
 * operations need to wait to grab an exclusive lock in order to clear
 * NFS_INO_ODIRECT.
 */
void
nfs_start_io_direct(struct nfs_inode *nfsi)
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

/**
 * nfs_end_io_direct - declare that the direct i/o operation is done
 * @nfsi - nfs_inode of the file
 *
 * Declare that a direct I/O operation is done, and release the shared
 * lock on nfsi->io_lock.
 */
void
nfs_end_io_direct(struct nfs_inode *nfsi)
{
	up_read(&nfsi->io_lock);
}

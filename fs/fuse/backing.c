// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE-BPF: Filesystem in Userspace with BPF
 * Copyright (c) 2021 Google LLC
 */

#include "fuse_i.h"

#include <linux/fdtable.h>
#include <linux/filter.h>
#include <linux/fs_stack.h>
#include <linux/namei.h>

#include "../internal.h"

/* Reimplement these functions since fget_task is not exported */
static struct file *fuse__fget_files(struct files_struct *files,
		unsigned int fd, fmode_t mask, unsigned int refs)
{
	struct file *file;

	rcu_read_lock();
loop:
	file = fcheck_files(files, fd);
	if (file) {
		/* File object ref couldn't be taken.
		 * dup2() atomicity guarantee is the reason
		 * we loop to catch the new file (or NULL pointer)
		 */
		if (file->f_mode & mask)
			file = NULL;
		else if (!get_file_rcu_many(file, refs))
			goto loop;
	}
	rcu_read_unlock();
	return file;
}

static struct file *fuse_fget_task(struct task_struct *task, unsigned int fd)
{
	struct file *file = NULL;

	task_lock(task);
	if (task->files)
		file = fuse__fget_files(task->files, fd, 0, 1);
	task_unlock(task);

	return file;
}

struct file *fuse_fget(struct fuse_conn *fc, unsigned int fd)
{
	return fuse_fget_task(fc->task, fd);
}

struct bpf_prog *fuse_get_bpf_prog(struct fuse_conn *fc, unsigned int fd)
{
	struct file *bpf_file = fuse_fget(fc, fd);
	struct bpf_prog *bpf_prog = ERR_PTR(-EINVAL);

	if (!bpf_file)
		goto out;
	/**
	 * Two ways of getting a bpf prog from another task's fd, since
	 * bpf_prog_get_type_dev only works with an fd
	 *
	 * 1) Duplicate a little of the needed code. Requires access to
	 *    bpf_prog_fops for validation, which is not exported for modules
	 * 2) Insert the bpf_file object into a fd from the current task
	 *    Stupidly complex, but I think OK, as security checks are not run
	 *    during the existence of the handle
	 *
	 * Best would be to upstream 1) into kernel/bpf/syscall.c and export it
	 * for use here. Failing that, we have to use 2, since fuse must be
	 * compilable as a module.
	 */
#if 0
	if (bpf_file->f_op != &bpf_prog_fops)
		goto out;

	bpf_prog = bpf_file->private_data;
	if (bpf_prog->type == BPF_PROG_TYPE_FUSE)
		bpf_prog_inc(bpf_prog);
	else
		bpf_prog = ERR_PTR(-EINVAL);

#else
	{
		int task_fd = get_unused_fd_flags(bpf_file->f_flags);

		if (task_fd < 0)
			goto out;
		fd_install(task_fd, bpf_file);

		bpf_prog = bpf_prog_get_type_dev(task_fd, BPF_PROG_TYPE_FUSE,
						 false);
		__close_fd(current->files, task_fd);

		/* TODO I think this file is probably being leaked */
		bpf_file = NULL;
	}
#endif

out:
	if (bpf_file)
		fput(bpf_file);
	return bpf_prog;
}

int fuse_open_initialize(struct fuse_args *fa, struct fuse_open_io *foio,
			 struct inode *inode, struct file *file, bool isdir)
{
	foio->foi = (struct fuse_open_in) {
		.flags = file->f_flags & ~(O_CREAT | O_EXCL | O_NOCTTY),
	};

	foio->foo = (struct fuse_open_out) {0};

	*fa = (struct fuse_args) {
		.nodeid = get_fuse_inode(inode)->nodeid,
		.opcode = isdir ? FUSE_OPENDIR : FUSE_OPEN,
		.in_numargs = 1,
		.out_numargs = 1,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(foio->foi),
			.value = &foio->foi,
		},
		.out_args[0] = (struct fuse_arg) {
			.size = sizeof(foio->foo),
			.value = &foio->foo,
		},
	};

	return 0;
}

int fuse_open_backing(struct fuse_args *fa,
		      struct inode *inode, struct file *file, bool isdir)
{
	struct fuse_mount *fm = get_fuse_mount(inode);
	const struct fuse_open_in *foi = fa->in_args[0].value;
	struct fuse_file *ff;
	int retval;
	int mask;
	struct fuse_dentry *fd = get_fuse_dentry(file->f_path.dentry);
	struct file *backing_file;

	ff = fuse_file_alloc(fm);
	if (!ff)
		return -ENOMEM;
	file->private_data = ff;

	switch (foi->flags & O_ACCMODE) {
	case O_RDONLY:
		mask = MAY_READ;
		break;

	case O_WRONLY:
		mask = MAY_WRITE;
		break;

	case O_RDWR:
		mask = MAY_READ | MAY_WRITE;
		break;

	default:
		return -EINVAL;
	}

	retval = inode_permission(get_fuse_inode(inode)->backing_inode, mask);
	if (retval)
		return retval;

	backing_file = dentry_open(&fd->backing_path,
				   foi->flags,
				   current_cred());

	if (IS_ERR(backing_file)) {
		fuse_file_free(ff);
		file->private_data = NULL;
		return PTR_ERR(backing_file);
	}
	ff->backing_file = backing_file;

	return 0;
}

void *fuse_open_finalize(struct fuse_args *fa,
		       struct inode *inode, struct file *file, bool isdir)
{
	struct fuse_file *ff = file->private_data;
	struct fuse_open_out *foo = fa->out_args[0].value;

	ff->fh = foo->fh;
	return 0;
}

int fuse_create_open_initialize(
		struct fuse_args *fa, struct fuse_create_open_io *fcoio,
		struct inode *dir, struct dentry *entry,
		struct file *file, unsigned int flags, umode_t mode)
{
	fcoio->fci = (struct fuse_create_in) {
		.flags = file->f_flags & ~(O_CREAT | O_EXCL | O_NOCTTY),
		.mode = mode,
	};

	fcoio->feo = (struct fuse_entry_out) {0};
	fcoio->foo = (struct fuse_open_out) {0};

	*fa = (struct fuse_args) {
		.nodeid = get_node_id(dir),
		.opcode = FUSE_CREATE,
		.in_numargs = 2,
		.out_numargs = 2,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(fcoio->fci),
			.value = &fcoio->fci,
		},
		.in_args[1] = (struct fuse_in_arg) {
			.size = entry->d_name.len + 1,
			.value = entry->d_name.name,
		},
		.out_args[0] = (struct fuse_arg) {
			.size = sizeof(fcoio->feo),
			.value = &fcoio->feo,
		},
		.out_args[1] = (struct fuse_arg) {
			.size = sizeof(fcoio->foo),
			.value = &fcoio->foo,
		},
	};

	return 0;
}

static int fuse_open_file_backing(struct inode *inode, struct file *file)
{
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct dentry *entry = file->f_path.dentry;
	struct fuse_dentry *fuse_dentry = get_fuse_dentry(entry);
	struct fuse_file *fuse_file;
	struct file *backing_file;

	fuse_file = fuse_file_alloc(fm);
	if (!fuse_file)
		return -ENOMEM;
	file->private_data = fuse_file;

	backing_file = dentry_open(&fuse_dentry->backing_path, file->f_flags,
				   current_cred());
	if (IS_ERR(backing_file)) {
		fuse_file_free(fuse_file);
		file->private_data = NULL;
		return PTR_ERR(backing_file);
	}
	fuse_file->backing_file = backing_file;

	return 0;
}

int fuse_create_open_backing(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry,
		struct file *file, unsigned int flags, umode_t mode)
{
	struct fuse_inode *dir_fuse_inode = get_fuse_inode(dir);
	struct fuse_dentry *dir_fuse_dentry = get_fuse_dentry(entry->d_parent);
	struct dentry *backing_dentry = NULL;
	struct inode *inode = NULL;
	struct dentry *newent;
	int err = 0;
	const struct fuse_create_in *fci = fa->in_args[0].value;

	if (!dir_fuse_inode || !dir_fuse_dentry)
		return -EIO;

	inode_lock_nested(dir_fuse_inode->backing_inode, I_MUTEX_PARENT);
	backing_dentry = lookup_one_len(fa->in_args[1].value,
					dir_fuse_dentry->backing_path.dentry,
					strlen(fa->in_args[1].value));
	inode_unlock(dir_fuse_inode->backing_inode);

	if (IS_ERR(backing_dentry))
		return PTR_ERR(backing_dentry);

	if (d_really_is_positive(backing_dentry)) {
		err = -EIO;
		goto out;
	}

	err = vfs_create(dir_fuse_inode->backing_inode, backing_dentry,
			 fci->mode, true);
	if (err)
		goto out;

	if (get_fuse_dentry(entry)->backing_path.dentry)
		path_put(&get_fuse_dentry(entry)->backing_path);
	get_fuse_dentry(entry)->backing_path = (struct path) {
		.mnt = dir_fuse_dentry->backing_path.mnt,
		.dentry = backing_dentry,
	};
	path_get(&get_fuse_dentry(entry)->backing_path);

	inode = fuse_iget_backing(dir->i_sb,
			get_fuse_dentry(entry)->backing_path.dentry->d_inode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}

	if (get_fuse_inode(inode)->bpf)
		bpf_prog_put(get_fuse_inode(inode)->bpf);
	get_fuse_inode(inode)->bpf = dir_fuse_inode->bpf;
	if (get_fuse_inode(inode)->bpf)
		bpf_prog_inc(dir_fuse_inode->bpf);

	newent = d_splice_alias(inode, entry);
	if (IS_ERR(newent)) {
		err = PTR_ERR(newent);
		goto out;
	}

	entry = newent ? newent : entry;
	err = finish_open(file, entry, fuse_open_file_backing);

out:
	dput(backing_dentry);
	return err;
}

void *fuse_create_open_finalize(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry,
		struct file *file, unsigned int flags, umode_t mode)
{
	struct fuse_file *ff = file->private_data;
	struct fuse_inode *fi = get_fuse_inode(file->f_inode);
	struct fuse_entry_out *feo = fa->out_args[0].value;
	struct fuse_open_out *foo = fa->out_args[1].value;

	fi->nodeid = feo->nodeid;
	ff->fh = foo->fh;
	return 0;
}

int fuse_release_initialize(struct fuse_args *fa, struct fuse_release_in *fri,
			    struct inode *inode, struct file *file)
{
	struct fuse_file *fuse_file = file->private_data;

	/* Always put backing file whatever bpf/userspace says */
	fput(fuse_file->backing_file);

	*fri = (struct fuse_release_in) {
		.fh = ((struct fuse_file *)(file->private_data))->fh,
	};

	*fa = (struct fuse_args) {
		.nodeid = get_fuse_inode(inode)->nodeid,
		.opcode = FUSE_RELEASE,
		.in_numargs = 1,
		.in_args[0].size = sizeof(*fri),
		.in_args[0].value = fri,
	};

	return 0;
}

int fuse_releasedir_initialize(struct fuse_args *fa,
			struct fuse_release_in *fri,
			struct inode *inode, struct file *file)
{
	struct fuse_file *fuse_file = file->private_data;

	/* Always put backing file whatever bpf/userspace says */
	fput(fuse_file->backing_file);

	*fri = (struct fuse_release_in) {
		.fh = ((struct fuse_file *)(file->private_data))->fh,
	};

	*fa = (struct fuse_args) {
		.nodeid = get_fuse_inode(inode)->nodeid,
		.opcode = FUSE_RELEASEDIR,
		.in_numargs = 1,
		.in_args[0].size = sizeof(*fri),
		.in_args[0].value = fri,
	};

	return 0;
}

int fuse_release_backing(struct fuse_args *fa,
			 struct inode *inode, struct file *file)
{
	return 0;
}

void *fuse_release_finalize(struct fuse_args *fa,
			    struct inode *inode, struct file *file)
{
	fuse_file_free(file->private_data);
	return NULL;
}

int fuse_flush_initialize(struct fuse_args *fa, struct fuse_flush_in *ffi,
			   struct file *file, fl_owner_t id)
{
	struct fuse_file *fuse_file = file->private_data;

	*ffi = (struct fuse_flush_in) {
		.fh = fuse_file->fh,
	};

	*fa = (struct fuse_args) {
		.nodeid = get_node_id(file->f_inode),
		.opcode = FUSE_FLUSH,
		.in_numargs = 1,
		.in_args[0].size = sizeof(*ffi),
		.in_args[0].value = ffi,
		.force = true,
	};

	return 0;
}

int fuse_flush_backing(struct fuse_args *fa, struct file *file, fl_owner_t id)
{
	struct fuse_file *fuse_file = file->private_data;
	struct file *backing_file = fuse_file->backing_file;

	if (backing_file->f_op->flush)
		return backing_file->f_op->flush(backing_file, id);
	return 0;
}

void *fuse_flush_finalize(struct fuse_args *fa, struct file *file, fl_owner_t id)
{
	return NULL;
}

int fuse_fsync_initialize(struct fuse_args *fa, struct fuse_fsync_in *ffi,
		   struct file *file, loff_t start, loff_t end, int datasync)
{
	struct fuse_file *fuse_file = file->private_data;

	*ffi = (struct fuse_fsync_in) {
		.fh = fuse_file->fh,
		.fsync_flags = datasync ? FUSE_FSYNC_FDATASYNC : 0,
	};

	*fa = (struct fuse_args) {
		.nodeid = get_fuse_inode(file->f_inode)->nodeid,
		.opcode = FUSE_FSYNC,
		.in_numargs = 1,
		.in_args[0].size = sizeof(*ffi),
		.in_args[0].value = ffi,
		.force = true,
	};

	return 0;
}

int fuse_fsync_backing(struct fuse_args *fa,
		   struct file *file, loff_t start, loff_t end, int datasync)
{
	struct fuse_file *fuse_file = file->private_data;
	struct file *backing_file = fuse_file->backing_file;
	const struct fuse_fsync_in *ffi = fa->in_args[0].value;
	int new_datasync = (ffi->fsync_flags & FUSE_FSYNC_FDATASYNC) ? 1 : 0;

	return vfs_fsync(backing_file, new_datasync);
}

void *fuse_fsync_finalize(struct fuse_args *fa,
		   struct file *file, loff_t start, loff_t end, int datasync)
{
	return NULL;
}

int fuse_dir_fsync_initialize(struct fuse_args *fa, struct fuse_fsync_in *ffi,
		   struct file *file, loff_t start, loff_t end, int datasync)
{
	struct fuse_file *fuse_file = file->private_data;

	*ffi = (struct fuse_fsync_in) {
		.fh = fuse_file->fh,
		.fsync_flags = datasync ? FUSE_FSYNC_FDATASYNC : 0,
	};

	*fa = (struct fuse_args) {
		.nodeid = get_fuse_inode(file->f_inode)->nodeid,
		.opcode = FUSE_FSYNCDIR,
		.in_numargs = 1,
		.in_args[0].size = sizeof(*ffi),
		.in_args[0].value = ffi,
		.force = true,
	};

	return 0;
}

int fuse_getxattr_initialize(struct fuse_args *fa,
		struct fuse_getxattr_io *fgio,
		struct dentry *dentry, const char *name, void *value,
		size_t size)
{
	*fgio = (struct fuse_getxattr_io) {
		.fgi.size = size,
	};

	*fa = (struct fuse_args) {
		.nodeid = get_fuse_inode(dentry->d_inode)->nodeid,
		.opcode = FUSE_GETXATTR,
		.in_numargs = 2,
		.out_numargs = 1,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(fgio->fgi),
			.value = &fgio->fgi,
		},
		.in_args[1] = (struct fuse_in_arg) {
			.size = strlen(name) + 1,
			.value = name,
		},
		.out_argvar = size ? true : false,
		.out_args[0].size = size ? size : sizeof(fgio->fgo),
		.out_args[0].value = size ? value : &fgio->fgo,
	};

	return 0;
}

int fuse_getxattr_backing(struct fuse_args *fa,
		struct dentry *dentry, const char *name, void *value,
		size_t size)
{
	ssize_t ret = vfs_getxattr(get_fuse_dentry(dentry)->backing_path.dentry,
				   fa->in_args[1].value, value, size);

	if (fa->out_argvar)
		fa->out_args[0].size = ret;
	else
		((struct fuse_getxattr_out *)fa->out_args[0].value)->size = ret;

	return 0;
}

void *fuse_getxattr_finalize(struct fuse_args *fa,
		struct dentry *dentry, const char *name, void *value,
		size_t size)
{
	struct fuse_getxattr_out *fgo;

	if (fa->out_argvar)
		return ERR_PTR(fa->out_args[0].size);

	fgo = fa->out_args[0].value;

	return ERR_PTR(fgo->size);

}

int fuse_listxattr_initialize(struct fuse_args *fa,
			      struct fuse_getxattr_io *fgio,
			      struct dentry *dentry, char *list, size_t size)
{
	*fgio = (struct fuse_getxattr_io){
		.fgi.size = size,
	};

	*fa = (struct fuse_args){
		.nodeid = get_fuse_inode(dentry->d_inode)->nodeid,
		.opcode = FUSE_LISTXATTR,
		.in_numargs = 1,
		.out_numargs = 1,
		.in_args[0] =
			(struct fuse_in_arg){
				.size = sizeof(fgio->fgi),
				.value = &fgio->fgi,
			},
		.out_argvar = size ? true : false,
		.out_args[0].size = size ? size : sizeof(fgio->fgo),
		.out_args[0].value = size ? (void *)list : &fgio->fgo,
	};

	return 0;
}

int fuse_listxattr_backing(struct fuse_args *fa, struct dentry *dentry,
			   char *list, size_t size)
{
	ssize_t ret =
		vfs_listxattr(get_fuse_dentry(dentry)->backing_path.dentry,
			      list, size);

	if (fa->out_argvar)
		fa->out_args[0].size = ret;
	else
		((struct fuse_getxattr_out *)fa->out_args[0].value)->size = ret;

	return 0;
}

void *fuse_listxattr_finalize(struct fuse_args *fa, struct dentry *dentry,
			      char *list, size_t size)
{
	struct fuse_getxattr_out *fgo;

	if (fa->out_argvar)
		return ERR_PTR(fa->out_args[0].size);

	fgo = fa->out_args[0].value;

	return ERR_PTR(fgo->size);
}

int fuse_setxattr_initialize(struct fuse_args *fa,
			     struct fuse_setxattr_in *fsxi,
			     struct dentry *dentry, const char *name,
			     const void *value, size_t size, int flags)
{
	*fsxi = (struct fuse_setxattr_in) {
		.size = size,
		.flags = flags,
	};

	*fa = (struct fuse_args) {
		.nodeid = get_fuse_inode(dentry->d_inode)->nodeid,
		.opcode = FUSE_SETXATTR,
		.in_numargs = 3,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(*fsxi),
			.value = fsxi,
		},
		.in_args[1] = (struct fuse_in_arg) {
			.size = strlen(name) + 1,
			.value = name,
		},
		.in_args[2] = (struct fuse_in_arg) {
			.size = size,
			.value = value,
		},
	};

	return 0;
}

int fuse_setxattr_backing(struct fuse_args *fa, struct dentry *dentry,
			  const char *name, const void *value, size_t size,
			  int flags)
{
	return vfs_setxattr(get_fuse_dentry(dentry)->backing_path.dentry, name,
			    value, size, flags);
}

void *fuse_setxattr_finalize(struct fuse_args *fa, struct dentry *dentry,
			     const char *name, const void *value, size_t size,
			     int flags)
{
	return NULL;
}

int fuse_file_read_iter_initialize(
		struct fuse_args *fa, struct fuse_read_in *fri,
		struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;

	*fri = (struct fuse_read_in) {
		.fh = ff->fh,
		.offset = iocb->ki_pos,
		.size = to->count,
	};

	/* TODO we can't assume 'to' is a kvec */
	/* TODO we also can't assume the vector has only one component */
	*fa = (struct fuse_args) {
		.opcode = FUSE_READ,
		.nodeid = ff->nodeid,
		.in_numargs = 1,
		.in_args[0].size = sizeof(*fri),
		.in_args[0].value = fri,
		.out_numargs = 1,
		.out_args[0].size = fri->size,
		.out_args[0].value = to->kvec->iov_base,
		/*
		 * TODO Design this properly.
		 * Possible approach: do not pass buf to bpf
		 * If going to userland, do a deep copy
		 * For extra credit, do that to/from the vector, rather than
		 * making an extra copy in the kernel
		 */
	};

	return 0;
}

int fuse_file_read_iter_backing(struct fuse_args *fa,
		struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;
	ssize_t result;

	/* TODO This just plain ignores any change to fuse_read_in */
	result = vfs_iter_read(ff->backing_file, to, &iocb->ki_pos, 0);

	if (result < 0)
		return result;

	/* TODO Need to point value at the buffer for post-modification */
	fa->out_args[0].size = result;
	return result;
}

void *fuse_file_read_iter_finalize(struct fuse_args *fa,
		struct kiocb *iocb, struct iov_iter *to)
{
	return ERR_PTR(fa->out_args[0].size);
}

int fuse_file_write_iter_initialize(
		struct fuse_args *fa, struct fuse_file_write_iter_io *fwio,
		struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;

	*fwio = (struct fuse_file_write_iter_io) {
		.fwi.fh = ff->fh,
		.fwi.offset = iocb->ki_pos,
		.fwi.size = from->count,
	};

	/* TODO we can't assume 'from' is a kvec */
	*fa = (struct fuse_args) {
		.opcode = FUSE_WRITE,
		.nodeid = ff->nodeid,
		.in_numargs = 2,
		.in_args[0].size = sizeof(fwio->fwi),
		.in_args[0].value = &fwio->fwi,
		.in_args[1].size = fwio->fwi.size,
		.in_args[1].value = from->kvec->iov_base,
		.out_numargs = 1,
		.out_args[0].size = sizeof(fwio->fwo),
		.out_args[0].value = &fwio->fwo,
	};

	return 0;
}

int fuse_file_write_iter_backing(struct fuse_args *fa,
		struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct fuse_file *ff = file->private_data;
	struct fuse_write_out *fwo = fa->out_args[0].value;

	/* TODO This just plain ignores any change to fuse_write_in */
	fwo->size = vfs_iter_write(ff->backing_file, from, &iocb->ki_pos, 0);

	if (fwo->size < 0)
		return fwo->size;
	return 0;
}

void *fuse_file_write_iter_finalize(struct fuse_args *fa,
		struct kiocb *iocb, struct iov_iter *from)
{
	struct fuse_write_out *fwo = fa->out_args[0].value;

	return ERR_PTR(fwo->size);
}

ssize_t fuse_backing_mmap(struct file *file, struct vm_area_struct *vma)
{
	int ret;
	struct fuse_file *ff = file->private_data;
	struct inode *fuse_inode = file_inode(file);
	struct file *backing_file = ff->backing_file;
	struct inode *backing_inode = file_inode(backing_file);

	if (!backing_file->f_op->mmap)
		return -ENODEV;

	if (WARN_ON(file != vma->vm_file))
		return -EIO;

	vma->vm_file = get_file(backing_file);

	ret = call_mmap(vma->vm_file, vma);

	if (ret)
		fput(backing_file);
	else
		fput(file);

	if (file->f_flags & O_NOATIME)
		return ret;

	if ((!timespec64_equal(&fuse_inode->i_mtime,
			       &backing_inode->i_mtime) ||
	     !timespec64_equal(&fuse_inode->i_ctime,
			       &backing_inode->i_ctime))) {
		fuse_inode->i_mtime = backing_inode->i_mtime;
		fuse_inode->i_ctime = backing_inode->i_ctime;
	}
	touch_atime(&file->f_path);

	return ret;
}

int fuse_file_fallocate_initialize(struct fuse_args *fa,
		struct fuse_fallocate_in *ffi,
		struct file *file, int mode, loff_t offset, loff_t length)
{
	struct fuse_file *ff = file->private_data;

	*ffi = (struct fuse_fallocate_in) {
		.fh = ff->fh,
		.offset = offset,
		.length = length,
		.mode = mode
	};

	*fa = (struct fuse_args) {
		.opcode = FUSE_FALLOCATE,
		.nodeid = ff->nodeid,
		.in_numargs = 1,
		.in_args[0].size = sizeof(*ffi),
		.in_args[0].value = ffi,
	};

	return 0;
}

int fuse_file_fallocate_backing(struct fuse_args *fa,
		struct file *file, int mode, loff_t offset, loff_t length)
{
	const struct fuse_fallocate_in *ffi = fa->in_args[0].value;
	struct fuse_file *ff = file->private_data;

	return vfs_fallocate(ff->backing_file, ffi->mode, ffi->offset,
			     ffi->length);
}

void *fuse_file_fallocate_finalize(struct fuse_args *fa,
		struct file *file, int mode, loff_t offset, loff_t length)
{
	return NULL;
}

/*******************************************************************************
 * Directory operations after here                                             *
 ******************************************************************************/

int fuse_lookup_initialize(struct fuse_args *fa, struct fuse_lookup_io *fli,
	       struct inode *dir, struct dentry *entry, unsigned int flags)
{
	*fa = (struct fuse_args) {
		.nodeid = get_fuse_inode(dir)->nodeid,
		.opcode = FUSE_LOOKUP,
		.in_numargs = 1,
		.out_numargs = 2,
		.out_argvar = 1,
		.in_args[0] = (struct fuse_in_arg) {
			.size = entry->d_name.len + 1,
			.value = entry->d_name.name,
		},
		.out_args[0] = (struct fuse_arg) {
			.size = sizeof(fli->feo),
			.value = &fli->feo,
		},
		.out_args[1] = (struct fuse_arg) {
			.size = sizeof(fli->febo),
			.value = &fli->febo,
		},
	};

	return 0;
}

int fuse_lookup_backing(struct fuse_args *fa, struct inode *dir,
			  struct dentry *entry, unsigned int flags)
{
	struct fuse_dentry *fuse_entry = get_fuse_dentry(entry);
	struct fuse_dentry *dir_fuse_entry = get_fuse_dentry(entry->d_parent);
	struct dentry *dir_backing_entry = dir_fuse_entry->backing_path.dentry;
	struct inode *dir_backing_inode = dir_backing_entry->d_inode;
	struct dentry *backing_entry;

	/* TODO this will not handle lookups over mount points */
	inode_lock_nested(dir_backing_inode, I_MUTEX_PARENT);
	backing_entry = lookup_one_len(entry->d_name.name, dir_backing_entry,
					strlen(entry->d_name.name));
	inode_unlock(dir_backing_inode);

	if (IS_ERR(backing_entry))
		return PTR_ERR(backing_entry);

	fuse_entry->backing_path = (struct path) {
		.dentry = backing_entry,
		.mnt = dir_fuse_entry->backing_path.mnt,
	};

	mntget(fuse_entry->backing_path.mnt);
	return 0;
}

struct dentry *fuse_lookup_finalize(struct fuse_args *fa, struct inode *dir,
			   struct dentry *entry, unsigned int flags)
{
	struct fuse_dentry *fd;
	struct dentry *bd;
	struct inode *inode, *backing_inode;
	struct fuse_entry_out *feo = fa->out_args[0].value;
	struct fuse_entry_bpf_out *febo = fa->out_args[1].value;

	fd = get_fuse_dentry(entry);
	if (!fd)
		return ERR_PTR(-EIO);
	bd = fd->backing_path.dentry;
	if (!bd)
		return ERR_PTR(-ENOENT);
	backing_inode = bd->d_inode;
	if (!backing_inode)
		return 0;

	inode = fuse_iget_backing(dir->i_sb, backing_inode);

	if (IS_ERR(inode))
		return ERR_PTR(PTR_ERR(inode));

	/* TODO Make sure this handles invalid handles */
	/* TODO Do we need the same code in revalidate */
	if (get_fuse_inode(inode)->bpf) {
		bpf_prog_put(get_fuse_inode(inode)->bpf);
		get_fuse_inode(inode)->bpf = NULL;
	}

	switch (febo->bpf_action) {
	case FUSE_ACTION_KEEP:
		get_fuse_inode(inode)->bpf = get_fuse_inode(dir)->bpf;
		if (get_fuse_inode(inode)->bpf)
			bpf_prog_inc(get_fuse_inode(inode)->bpf);
		break;

	case FUSE_ACTION_REMOVE:
		get_fuse_inode(inode)->bpf = NULL;
		break;

	case FUSE_ACTION_REPLACE: {
		struct fuse_conn *fc = get_fuse_mount(dir)->fc;
		struct bpf_prog *bpf_prog = fuse_get_bpf_prog(fc, febo->bpf_fd);

		if (IS_ERR(bpf_prog))
			return ERR_PTR(PTR_ERR(bpf_prog));

		get_fuse_inode(inode)->bpf = bpf_prog;
		break;
	}

	default:
		return ERR_PTR(-EIO);
	}

	switch (febo->backing_action) {
	case FUSE_ACTION_KEEP:
		/* backing inode/path are added in fuse_lookup_backing */
		break;

	case FUSE_ACTION_REMOVE:
		iput(get_fuse_inode(inode)->backing_inode);
		get_fuse_inode(inode)->backing_inode = NULL;
		path_put_init(&get_fuse_dentry(entry)->backing_path);
		break;

	case FUSE_ACTION_REPLACE: {
		struct fuse_conn *fc;
		struct file *backing_file;

		fc = get_fuse_mount(dir)->fc;
		backing_file = fuse_fget(fc, febo->backing_fd);
		__close_fd(fc->task->files, febo->backing_fd);
		if (!backing_file)
			return ERR_PTR(-EIO);

		iput(get_fuse_inode(inode)->backing_inode);
		get_fuse_inode(inode)->backing_inode =
			backing_file->f_inode;
		ihold(get_fuse_inode(inode)->backing_inode);

		path_put(&get_fuse_dentry(entry)->backing_path);
		get_fuse_dentry(entry)->backing_path = backing_file->f_path;
		path_get(&get_fuse_dentry(entry)->backing_path);

		fput(backing_file);
		break;
	}

	default:
		return ERR_PTR(-EIO);
	}

	get_fuse_inode(inode)->nodeid = feo->nodeid;

	return d_splice_alias(inode, entry);
}

int fuse_revalidate_backing(struct fuse_args *fa, struct inode *dir,
			   struct dentry *entry, unsigned int flags)
{
	struct fuse_dentry *fuse_dentry = get_fuse_dentry(entry);
	struct dentry *backing_entry = fuse_dentry->backing_path.dentry;

	if (unlikely(backing_entry->d_flags & DCACHE_OP_REVALIDATE))
		return backing_entry->d_op->d_revalidate(backing_entry, flags);
	return 1;
}

void *fuse_revalidate_finalize(struct fuse_args *fa, struct inode *dir,
			   struct dentry *entry, unsigned int flags)
{
	return 0;
}

int fuse_canonical_path_initialize(struct fuse_args *fa,
				   struct fuse_dummy_io *fdi,
				   const struct path *path,
				   struct path *canonical_path)
{
	fa->opcode = FUSE_CANONICAL_PATH;
	return 0;
}

int fuse_canonical_path_backing(struct fuse_args *fa, const struct path *path,
				struct path *canonical_path)
{
	get_fuse_backing_path(path->dentry, canonical_path);
	return 0;
}

void *fuse_canonical_path_finalize(struct fuse_args *fa,
				   const struct path *path,
				   struct path *canonical_path)
{
	return NULL;
}

int fuse_mknod_initialize(
		struct fuse_args *fa, struct fuse_mknod_in *fmi,
		struct inode *dir, struct dentry *entry, umode_t mode, dev_t rdev)
{
	*fmi = (struct fuse_mknod_in) {
		.mode = mode,
		.rdev = new_encode_dev(rdev),
		.umask = current_umask(),
	};
	*fa = (struct fuse_args) {
		.nodeid = get_node_id(dir),
		.opcode = FUSE_MKNOD,
		.in_numargs = 2,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(*fmi),
			.value = fmi,
		},
		.in_args[1] = (struct fuse_in_arg) {
			.size = entry->d_name.len + 1,
			.value = entry->d_name.name,
		},
	};

	return 0;
}

int fuse_mknod_backing(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry, umode_t mode, dev_t rdev)
{
	int err = 0;
	const struct fuse_mknod_in *fmi = fa->in_args[0].value;
	struct inode *backing_inode = get_fuse_inode(dir)->backing_inode;
	struct path backing_path = {};
	struct inode *inode = NULL;

	//TODO Actually deal with changing the backing entry in mknod
	get_fuse_backing_path(entry, &backing_path);
	if (!backing_path.dentry)
		return -EBADF;

	inode_lock_nested(backing_inode, I_MUTEX_PARENT);
	err = vfs_mknod(backing_inode, backing_path.dentry,
			fmi->mode & ~fmi->umask, new_decode_dev(fmi->rdev));
	inode_unlock(backing_inode);
	if (err)
		goto out;
	if (d_really_is_negative(backing_path.dentry) ||
		unlikely(d_unhashed(backing_path.dentry))) {
		err = -EINVAL;
		/**
		 * TODO: overlayfs responds to this situation with a
		 * lookupOneLen. Should we do that too?
		 */
		goto out;
	}
	inode = fuse_iget_backing(dir->i_sb, backing_inode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	d_instantiate(entry, inode);
out:
	path_put(&backing_path);
	return err;
}

void *fuse_mknod_finalize(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry, umode_t mode, dev_t rdev)
{
	return NULL;
}

int fuse_mkdir_initialize(
		struct fuse_args *fa, struct fuse_mkdir_in *fmi,
		struct inode *dir, struct dentry *entry, umode_t mode)
{
	*fmi = (struct fuse_mkdir_in) {
		.mode = mode,
		.umask = current_umask(),
	};
	*fa = (struct fuse_args) {
		.nodeid = get_node_id(dir),
		.opcode = FUSE_MKDIR,
		.in_numargs = 2,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(*fmi),
			.value = fmi,
		},
		.in_args[1] = (struct fuse_in_arg) {
			.size = entry->d_name.len + 1,
			.value = entry->d_name.name,
		},
	};

	return 0;
}

int fuse_mkdir_backing(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry, umode_t mode)
{
	int err = 0;
	const struct fuse_mkdir_in *fmi = fa->in_args[0].value;
	struct inode *backing_inode = get_fuse_inode(dir)->backing_inode;
	struct path backing_path = {};
	struct inode *inode = NULL;
	struct dentry *d;

	//TODO Actually deal with changing the backing entry in mkdir
	get_fuse_backing_path(entry, &backing_path);
	if (!backing_path.dentry)
		return -EBADF;

	inode_lock_nested(backing_inode, I_MUTEX_PARENT);
	err = vfs_mkdir(backing_inode, backing_path.dentry, fmi->mode & ~fmi->umask);
	if (err)
		goto out;
	if (d_really_is_negative(backing_path.dentry) ||
		unlikely(d_unhashed(backing_path.dentry))) {
		d = lookup_one_len(entry->d_name.name, backing_path.dentry->d_parent,
				entry->d_name.len);
		if (IS_ERR(d)) {
			err = PTR_ERR(d);
			goto out;
		}
		dput(backing_path.dentry);
		backing_path.dentry = d;
	}
	inode = fuse_iget_backing(dir->i_sb, backing_inode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	d_instantiate(entry, inode);
out:
	inode_unlock(backing_inode);
	path_put(&backing_path);
	return err;
}

void *fuse_mkdir_finalize(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry, umode_t mode)
{
	return NULL;
}

int fuse_rmdir_initialize(
		struct fuse_args *fa, struct fuse_dummy_io *dummy,
		struct inode *dir, struct dentry *entry)
{
	*fa = (struct fuse_args) {
		.nodeid = get_node_id(dir),
		.opcode = FUSE_RMDIR,
		.in_numargs = 1,
		.in_args[0] = (struct fuse_in_arg) {
			.size = entry->d_name.len + 1,
			.value = entry->d_name.name,
		},
	};

	return 0;
}

int fuse_rmdir_backing(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry)
{
	int err = 0;
	struct path backing_path = {};
	struct dentry *backing_parent_dentry;
	struct inode *backing_inode;

	/* TODO Actually deal with changing the backing entry in rmdir */
	get_fuse_backing_path(entry, &backing_path);
	if (!backing_path.dentry)
		return -EBADF;

	/* TODO Not sure if we should reverify like overlayfs, or get inode from d_parent */
	backing_parent_dentry = dget_parent(backing_path.dentry);
	backing_inode = d_inode(backing_parent_dentry);

	inode_lock_nested(backing_inode, I_MUTEX_PARENT);
	err = vfs_rmdir(backing_inode, backing_path.dentry);
	inode_unlock(backing_inode);

	dput(backing_parent_dentry);
	if (!err)
		d_drop(entry);
	path_put(&backing_path);
	return err;
}

void *fuse_rmdir_finalize(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry)
{
	return NULL;
}

static int fuse_rename_backing_common(
			 struct inode *olddir, struct dentry *oldent,
			 struct inode *newdir, struct dentry *newent,
			 unsigned int flags)
{
	int err = 0;
	struct path old_backing_path;
	struct path new_backing_path;
	struct dentry *old_backing_dir_dentry;
	struct dentry *old_backing_dentry;
	struct dentry *new_backing_dir_dentry;
	struct dentry *new_backing_dentry;
	struct dentry *trap = NULL;
	struct inode *target_inode;

	//TODO Actually deal with changing anything that isn't a flag
	get_fuse_backing_path(oldent, &old_backing_path);
	if (!old_backing_path.dentry)
		return -EBADF;
	get_fuse_backing_path(newent, &new_backing_path);
	if (!new_backing_path.dentry) {
		/*
		 * TODO A file being moved from a backing path to another
		 * backing path which is not yet instrumented with FUSE-BPF.
		 * This may be slow and should be substituted with something
		 * more clever.
		 */
		err = -EXDEV;
		goto put_old_path;
	}
	if (new_backing_path.mnt != old_backing_path.mnt) {
		err = -EXDEV;
		goto put_new_path;
	}
	old_backing_dentry = old_backing_path.dentry;
	new_backing_dentry = new_backing_path.dentry;
	old_backing_dir_dentry = dget_parent(old_backing_dentry);
	new_backing_dir_dentry = dget_parent(new_backing_dentry);
	target_inode = d_inode(newent);

	trap = lock_rename(old_backing_dir_dentry, new_backing_dir_dentry);
	if (trap == old_backing_dentry) {
		err = -EINVAL;
		goto put_parents;
	}
	if (trap == new_backing_dentry) {
		err = -ENOTEMPTY;
		goto put_parents;
	}
	err = vfs_rename(d_inode(old_backing_dir_dentry), old_backing_dentry,
			 d_inode(new_backing_dir_dentry), new_backing_dentry,
			 NULL, flags);
	if (err)
		goto unlock;
	if (target_inode)
		fsstack_copy_attr_all(target_inode,
				get_fuse_inode(target_inode)->backing_inode);
	fsstack_copy_attr_all(newdir, d_inode(new_backing_dir_dentry));
unlock:
	unlock_rename(old_backing_dir_dentry, new_backing_dir_dentry);
put_parents:
	dput(new_backing_dir_dentry);
	dput(old_backing_dir_dentry);
put_new_path:
	path_put(&new_backing_path);
put_old_path:
	path_put(&old_backing_path);
	return err;
}

int fuse_rename2_initialize(struct fuse_args *fa, struct fuse_rename2_in *fri,
			    struct inode *olddir, struct dentry *oldent,
			    struct inode *newdir, struct dentry *newent,
			    unsigned int flags)
{
	*fri = (struct fuse_rename2_in) {
		.newdir = get_node_id(newdir),
		.flags = flags,
	};
	*fa = (struct fuse_args) {
		.nodeid = get_node_id(olddir),
		.opcode = FUSE_RENAME2,
		.in_numargs = 3,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(*fri),
			.value = fri,
		},
		.in_args[1] = (struct fuse_in_arg) {
			.size = oldent->d_name.len + 1,
			.value = oldent->d_name.name,
		},
		.in_args[2] = (struct fuse_in_arg) {
			.size = newent->d_name.len + 1,
			.value = newent->d_name.name,
		},
	};

	return 0;
}

int fuse_rename2_backing(struct fuse_args *fa,
			 struct inode *olddir, struct dentry *oldent,
			 struct inode *newdir, struct dentry *newent,
			 unsigned int flags)
{
	const struct fuse_rename2_in *fri = fa->in_args[0].value;

	/* TODO: deal with changing dirs/ents */
	return fuse_rename_backing_common(olddir, oldent, newdir, newent, fri->flags);
}

void *fuse_rename2_finalize(struct fuse_args *fa,
			    struct inode *olddir, struct dentry *oldent,
			    struct inode *newdir, struct dentry *newent,
			    unsigned int flags)
{
	return NULL;
}

int fuse_rename_initialize(struct fuse_args *fa, struct fuse_rename_in *fri,
			   struct inode *olddir, struct dentry *oldent,
			   struct inode *newdir, struct dentry *newent)
{
	*fri = (struct fuse_rename_in) {
		.newdir = get_node_id(newdir),
	};
	*fa = (struct fuse_args) {
		.nodeid = get_node_id(olddir),
		.opcode = FUSE_RENAME,
		.in_numargs = 3,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(*fri),
			.value = fri,
		},
		.in_args[1] = (struct fuse_in_arg) {
			.size = oldent->d_name.len + 1,
			.value = oldent->d_name.name,
		},
		.in_args[2] = (struct fuse_in_arg) {
			.size = newent->d_name.len + 1,
			.value = newent->d_name.name,
		},
	};

	return 0;
}

int fuse_rename_backing(struct fuse_args *fa,
			struct inode *olddir, struct dentry *oldent,
			struct inode *newdir, struct dentry *newent)
{
	/* TODO: deal with changing dirs/ents */
	return fuse_rename_backing_common(olddir, oldent, newdir, newent, 0);
}

void *fuse_rename_finalize(struct fuse_args *fa,
			   struct inode *olddir, struct dentry *oldent,
			   struct inode *newdir, struct dentry *newent)
{
	return NULL;
}

int fuse_unlink_initialize(
		struct fuse_args *fa, struct fuse_dummy_io *dummy,
		struct inode *dir, struct dentry *entry)
{
	*fa = (struct fuse_args) {
		.nodeid = get_node_id(dir),
		.opcode = FUSE_UNLINK,
		.in_numargs = 1,
		.in_args[0] = (struct fuse_in_arg) {
			.size = entry->d_name.len + 1,
			.value = entry->d_name.name,
		},
	};

	return 0;
}

int fuse_unlink_backing(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry)
{
	int err = 0;
	struct path backing_path = {};
	struct dentry *backing_parent_dentry;
	struct inode *backing_inode;

	/* TODO Actually deal with changing the backing entry in unlink */
	get_fuse_backing_path(entry, &backing_path);
	if (!backing_path.dentry)
		return -EBADF;

	/* TODO Not sure if we should reverify like overlayfs, or get inode from d_parent */
	backing_parent_dentry = dget_parent(backing_path.dentry);
	backing_inode = d_inode(backing_parent_dentry);

	inode_lock_nested(backing_inode, I_MUTEX_PARENT);
	err = vfs_unlink(backing_inode, backing_path.dentry, NULL);
	inode_unlock(backing_inode);

	dput(backing_parent_dentry);
	if (!err)
		d_drop(entry);
	path_put(&backing_path);
	return err;
}

void *fuse_unlink_finalize(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry)
{
	return NULL;
}

int fuse_link_initialize(struct fuse_args *fa, struct fuse_link_in *fli,
			 struct dentry *entry, struct inode *dir,
			 struct dentry *newent)
{
	struct inode *src_inode = entry->d_inode;

	*fli = (struct fuse_link_in){
		.oldnodeid = get_node_id(src_inode),
	};

	fa->opcode = FUSE_LINK;
	fa->in_numargs = 2;
	fa->in_args[0].size = sizeof(*fli);
	fa->in_args[0].value = fli;
	fa->in_args[1].size = newent->d_name.len + 1;
	fa->in_args[1].value = newent->d_name.name;

	return 0;
}

int fuse_link_backing(struct fuse_args *fa, struct dentry *entry,
		      struct inode *dir, struct dentry *newent)
{
	int err = 0;
	struct path backing_old_path = {};
	struct path backing_new_path = {};
	struct dentry *backing_dir_dentry;
	struct inode *fuse_new_inode = NULL;
	struct inode *backing_dir_inode = get_fuse_inode(dir)->backing_inode;

	get_fuse_backing_path(entry, &backing_old_path);
	if (!backing_old_path.dentry)
		return -EBADF;

	get_fuse_backing_path(newent, &backing_new_path);
	if (!backing_new_path.dentry) {
		err = -EBADF;
		goto err_dst_path;
	}

	backing_dir_dentry = dget_parent(backing_new_path.dentry);
	backing_dir_inode = d_inode(backing_dir_dentry);

	inode_lock_nested(backing_dir_inode, I_MUTEX_PARENT);
	err = vfs_link(backing_old_path.dentry, backing_dir_inode, backing_new_path.dentry, NULL);
	inode_unlock(backing_dir_inode);
	if (err)
		goto out;

	if (d_really_is_negative(backing_new_path.dentry) ||
	    unlikely(d_unhashed(backing_new_path.dentry))) {
		err = -EINVAL;
		/**
		 * TODO: overlayfs responds to this situation with a
		 * lookupOneLen. Should we do that too?
		 */
		goto out;
	}

	fuse_new_inode = fuse_iget_backing(dir->i_sb, backing_dir_inode);
	if (IS_ERR(fuse_new_inode)) {
		err = PTR_ERR(fuse_new_inode);
		goto out;
	}
	d_instantiate(newent, fuse_new_inode);

out:
	dput(backing_dir_dentry);
	path_put(&backing_new_path);
err_dst_path:
	path_put(&backing_old_path);
	return err;
}

void *fuse_link_finalize(struct fuse_args *fa, struct dentry *entry,
			 struct inode *dir, struct dentry *newent)
{
	return NULL;
}

int fuse_getattr_initialize(struct fuse_args *fa, struct fuse_getattr_io *fgio,
			const struct dentry *entry, struct kstat *stat,
			u32 request_mask, unsigned int flags)
{
	fgio->fgi = (struct fuse_getattr_in) {
		.getattr_flags = flags,
		.fh = -1, /* TODO is this OK? */
	};

	fgio->fao = (struct fuse_attr_out) {0};

	*fa = (struct fuse_args) {
		.nodeid = get_node_id(entry->d_inode),
		.opcode = FUSE_GETATTR,
		.in_numargs = 1,
		.out_numargs = 1,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(fgio->fgi),
			.value = &fgio->fgi,
		},
		.out_args[0] = (struct fuse_arg) {
			.size = sizeof(fgio->fao),
			.value = &fgio->fao,
		},
	};

	return 0;
}

static void fuse_stat_to_attr(struct fuse_conn *fc, struct inode *inode,
		struct kstat *stat, struct fuse_attr *attr)
{
	unsigned int blkbits;

	/* see the comment in fuse_change_attributes() */
	if (fc->writeback_cache && S_ISREG(inode->i_mode)) {
		stat->size = i_size_read(inode);
		stat->mtime.tv_sec = inode->i_mtime.tv_sec;
		stat->mtime.tv_nsec = inode->i_mtime.tv_nsec;
		stat->ctime.tv_sec = inode->i_ctime.tv_sec;
		stat->ctime.tv_nsec = inode->i_ctime.tv_nsec;
	}

	attr->ino = stat->ino;
	attr->mode = (inode->i_mode & S_IFMT) | (stat->mode & 07777);
	attr->nlink = stat->nlink;
	attr->uid = from_kuid(fc->user_ns, stat->uid);
	attr->gid = from_kgid(fc->user_ns, stat->gid);
	attr->atime = stat->atime.tv_sec;
	attr->atimensec = stat->atime.tv_nsec;
	attr->mtime = stat->mtime.tv_sec;
	attr->mtimensec = stat->mtime.tv_nsec;
	attr->ctime = stat->ctime.tv_sec;
	attr->ctimensec = stat->ctime.tv_nsec;
	attr->size = stat->size;
	attr->blocks = stat->blocks;

	if (stat->blksize != 0)
		blkbits = ilog2(stat->blksize);
	else
		blkbits = inode->i_sb->s_blocksize_bits;

	attr->blksize = 1 << blkbits;
}

int fuse_getattr_backing(struct fuse_args *fa,
		const struct dentry *entry, struct kstat *stat,
			u32 request_mask, unsigned int flags)
{
	struct path *backing_path =
		&get_fuse_dentry(entry)->backing_path;
	struct inode *backing_inode = backing_path->dentry->d_inode;
	struct fuse_attr_out *fao = fa->out_args[0].value;
	struct kstat tmp;
	int err;

	if (!stat)
		stat = &tmp;

	err = vfs_getattr(backing_path, stat, request_mask, flags);

	if (!err)
		fuse_stat_to_attr(get_fuse_conn(entry->d_inode),
				  backing_inode, stat, &fao->attr);

	return err;
}

void *fuse_getattr_finalize(struct fuse_args *fa,
			const struct dentry *entry, struct kstat *stat,
			u32 request_mask, unsigned int flags)
{
	struct fuse_attr_out *outarg = fa->out_args[0].value;
	struct inode *inode = entry->d_inode;
	u64 attr_version = fuse_get_attr_version(get_fuse_mount(inode)->fc);
	int err = 0;

	/* TODO: Ensure this doesn't happen if we had an error getting attrs in
	 * backing.
	 */
	err = finalize_attr(inode, outarg, attr_version, stat);
	return ERR_PTR(err);
}

static void fattr_to_iattr(struct fuse_conn *fc,
			   const struct fuse_setattr_in *arg,
			   struct iattr *iattr)
{
	unsigned int fvalid = arg->valid;

	if (fvalid & FATTR_MODE)
		iattr->ia_valid |= ATTR_MODE, iattr->ia_mode = arg->mode;
	if (fvalid & FATTR_UID) {
		iattr->ia_valid |= ATTR_UID;
		iattr->ia_uid = make_kuid(fc->user_ns, arg->uid);
	}
	if (fvalid & FATTR_GID) {
		iattr->ia_valid |= ATTR_GID;
		iattr->ia_gid = make_kgid(fc->user_ns, arg->gid);
	}
	if (fvalid & FATTR_SIZE)
		iattr->ia_valid |= ATTR_SIZE,  iattr->ia_size = arg->size;
	if (fvalid & FATTR_ATIME) {
		iattr->ia_valid |= ATTR_ATIME;
		iattr->ia_atime.tv_sec = arg->atime;
		iattr->ia_atime.tv_nsec = arg->atimensec;
		if (!(fvalid & FATTR_ATIME_NOW))
			iattr->ia_valid |= ATTR_ATIME_SET;
	}
	if (fvalid & FATTR_MTIME) {
		iattr->ia_valid |= ATTR_MTIME;
		iattr->ia_mtime.tv_sec = arg->mtime;
		iattr->ia_mtime.tv_nsec = arg->mtimensec;
		if (!(fvalid & FATTR_MTIME_NOW))
			iattr->ia_valid |= ATTR_MTIME_SET;
	}
	if (fvalid & FATTR_CTIME) {
		iattr->ia_valid |= ATTR_CTIME;
		iattr->ia_ctime.tv_sec = arg->ctime;
		iattr->ia_ctime.tv_nsec = arg->ctimensec;
	}
}

int fuse_setattr_initialize(struct fuse_args *fa, struct fuse_setattr_io *fsio,
		struct dentry *dentry, struct iattr *attr, struct file *file)
{
	struct fuse_conn *fc = get_fuse_conn(dentry->d_inode);

	*fsio = (struct fuse_setattr_io) {0};
	iattr_to_fattr(fc, attr, &fsio->fsi, true);

	*fa = (struct fuse_args) {
		.opcode = FUSE_SETATTR,
		.nodeid = get_node_id(dentry->d_inode),
		.in_numargs = 1,
		.in_args[0].size = sizeof(fsio->fsi),
		.in_args[0].value = &fsio->fsi,
		.out_numargs = 1,
		.out_args[0].size = sizeof(fsio->fao),
		.out_args[0].value = &fsio->fao,
	};

	return 0;
}

int fuse_setattr_backing(struct fuse_args *fa,
		struct dentry *dentry, struct iattr *attr, struct file *file)
{
	struct fuse_conn *fc = get_fuse_conn(dentry->d_inode);
	const struct fuse_setattr_in *fsi = fa->in_args[0].value;
	struct iattr new_attr = {0};
	struct path *backing_path = &get_fuse_dentry(dentry)->backing_path;
	int res;

	fattr_to_iattr(fc, fsi, &new_attr);
	/* TODO: Some info doesn't get saved by the attr->fattr->attr transition
	 * When we actually allow the bpf to change these, we may have to consider
	 * the extra flags more, or pass more info into the bpf. Until then we can
	 * keep everything except for ATTR_FILE, since we'd need a file on the
	 * lower fs. For what it's worth, neither f2fs nor ext4 make use of that
	 * even if it is present.
	 */
	new_attr.ia_valid = attr->ia_valid & ~ATTR_FILE;
	inode_lock(d_inode(backing_path->dentry));
	res = notify_change(backing_path->dentry, &new_attr, NULL);
	inode_unlock(d_inode(backing_path->dentry));
	return res;
}

void *fuse_setattr_finalize(struct fuse_args *fa,
		struct dentry *dentry, struct iattr *attr, struct file *file)
{
	return NULL;
}

int fuse_get_link_initialize(struct fuse_args *fa, struct fuse_dummy_io *unused,
		struct inode *inode, struct dentry *dentry,
		struct delayed_call *callback, const char **out)
{
	/*
	 * TODO
	 * If we want to handle changing these things, we'll need to copy
	 * the lower fs's data into our own buffer, and provide our own callback
	 * to free that buffer.
	 *
	 * Pre could change the name we're looking at
	 * postfilter can change the name we return
	 *
	 * We ought to only make that buffer if it's been requested, so leaving
	 * this unimplemented for the moment
	 */
	*fa = (struct fuse_args) {
		.opcode = FUSE_READLINK,
		.nodeid = get_node_id(inode),
		.in_numargs = 1,
		.in_args[0] = (struct fuse_in_arg) {
			.size = dentry->d_name.len + 1,
			.value = dentry->d_name.name,
		},
		/*
		 * .out_argvar = 1,
		 * .out_numargs = 1,
		 * .out_args[0].size = ,
		 * .out_args[0].value = ,
		 */
	};

	return 0;
}

int fuse_get_link_backing(struct fuse_args *fa,
		struct inode *inode, struct dentry *dentry,
		struct delayed_call *callback, const char **out)
{
	struct path backing_path;

	if (!dentry) {
		*out = ERR_PTR(-ECHILD);
		return PTR_ERR(*out);
	}

	get_fuse_backing_path(dentry, &backing_path);
	if (!backing_path.dentry) {
		*out = ERR_PTR(-ECHILD);
		return PTR_ERR(*out);
	}

	/*
	 * TODO: If we want to do our own thing, copy the data and then call the
	 * callback
	 */
	*out = vfs_get_link(backing_path.dentry, callback);

	path_put(&backing_path);
	return 0;
}

void *fuse_get_link_finalize(struct fuse_args *fa,
		struct inode *inode, struct dentry *dentry,
		struct delayed_call *callback,  const char **out)
{
	return NULL;
}

int fuse_symlink_initialize(
		struct fuse_args *fa, struct fuse_dummy_io *unused,
		struct inode *dir, struct dentry *entry, const char *link, int len)
{
	*fa = (struct fuse_args) {
		.nodeid = get_node_id(dir),
		.opcode = FUSE_SYMLINK,
		.in_numargs = 2,
		.in_args[0] = (struct fuse_in_arg) {
			.size = entry->d_name.len + 1,
			.value = entry->d_name.name,
		},
		.in_args[1] = (struct fuse_in_arg) {
			.size = len,
			.value = link,
		},
	};

	return 0;
}

int fuse_symlink_backing(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry, const char *link, int len)
{
	int err = 0;
	struct inode *backing_inode = get_fuse_inode(dir)->backing_inode;
	struct path backing_path = {};
	struct inode *inode = NULL;

	//TODO Actually deal with changing the backing entry in symlink
	get_fuse_backing_path(entry, &backing_path);
	if (!backing_path.dentry)
		return -EBADF;

	inode_lock_nested(backing_inode, I_MUTEX_PARENT);
	err = vfs_symlink(backing_inode, backing_path.dentry, link);
	inode_unlock(backing_inode);
	if (err)
		goto out;
	if (d_really_is_negative(backing_path.dentry) ||
		unlikely(d_unhashed(backing_path.dentry))) {
		err = -EINVAL;
		/**
		 * TODO: overlayfs responds to this situation with a
		 * lookupOneLen. Should we do that too?
		 */
		goto out;
	}
	inode = fuse_iget_backing(dir->i_sb, backing_inode);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	d_instantiate(entry, inode);
out:
	path_put(&backing_path);
	return err;
}

void *fuse_symlink_finalize(
		struct fuse_args *fa,
		struct inode *dir, struct dentry *entry, const char *link, int len)
{
	return NULL;
}

int fuse_readdir_initialize(struct fuse_args *fa, struct fuse_read_io *frio,
			    struct file *file, struct dir_context *ctx,
			    bool *force_again, bool *allow_force)
{
	struct fuse_file *ff = file->private_data;
	u8 *page = (u8 *)__get_free_page(GFP_KERNEL);

	if (!page)
		return -ENOMEM;

	*fa = (struct fuse_args) {
		.nodeid = ff->nodeid,
		.opcode = FUSE_READDIR,
		.in_numargs = 1,
		.out_argvar = true,
		.out_numargs = 2,
		.in_args[0] = (struct fuse_in_arg) {
			.size = sizeof(frio->fri),
			.value = &frio->fri,
		},
		.out_args[0] = (struct fuse_arg) {
			.size = sizeof(frio->fro),
			.value = &frio->fro,
		},
		.out_args[1] = (struct fuse_arg) {
			.size = PAGE_SIZE,
			.value = page,
		},
	};

	frio->fri = (struct fuse_read_in) {
		.fh = ff->fh,
		.offset = ctx->pos,
		.size = PAGE_SIZE,
	};
	frio->fro = (struct fuse_read_out) {
		.again = 0,
		.offset = 0,
	};
	*force_again = false;
	*allow_force = true;
	return 0;
}

struct extfuse_ctx {
	struct dir_context ctx;
	u8 *addr;
	size_t offset;
};

static int filldir(struct dir_context *ctx, const char *name, int namelen,
				   loff_t offset, u64 ino, unsigned int d_type)
{
	struct extfuse_ctx *ec = container_of(ctx, struct extfuse_ctx, ctx);
	struct fuse_dirent *fd = (struct fuse_dirent *) (ec->addr + ec->offset);

	if (ec->offset + sizeof(struct fuse_dirent) + namelen > PAGE_SIZE)
		return -ENOMEM;

	*fd = (struct fuse_dirent) {
		.ino = ino,
		.off = offset,
		.namelen = namelen,
		.type = d_type,
	};

	strcpy(fd->name, name);
	ec->offset += FUSE_DIRENT_SIZE(fd);

	return 0;
}

int fuse_readdir_backing(struct fuse_args *fa,
			 struct file *file, struct dir_context *ctx,
			 bool *force_again, bool *allow_force)
{
	struct fuse_file *ff = file->private_data;
	struct file *backing_dir = ff->backing_file;
	struct fuse_read_out *fro = fa->out_args[0].value;
	struct extfuse_ctx ec;
	int err;

	ec = (struct extfuse_ctx) {
		.ctx.actor = filldir,
		.ctx.pos = ctx->pos,
		.addr = fa->out_args[1].value,
	};

	if (!ec.addr)
		return -ENOMEM;

	err = iterate_dir(backing_dir, &ec.ctx);
	if (ec.offset == 0)
		*allow_force = false;
	fa->out_args[1].size = ec.offset;

	fro->offset = ec.ctx.pos;
	fro->again = false;
	return err;
}

void *fuse_readdir_finalize(struct fuse_args *fa,
			    struct file *file, struct dir_context *ctx,
			    bool *force_again, bool *allow_force)
{
	int err = 0;
	struct fuse_file *ff = file->private_data;
	struct file *backing_dir = ff->backing_file;
	struct fuse_read_out *fro = fa->out_args[0].value;

	err = fuse_parse_dirfile(fa->out_args[1].value,
				 fa->out_args[1].size, file, ctx);
	*force_again = !!fro->again;
	if (*force_again && !*allow_force)
		err = -EINVAL;
	backing_dir->f_pos = fro->offset;

	free_page((unsigned long) fa->out_args[1].value);
	return ERR_PTR(err);
}

int fuse_access_initialize(struct fuse_args *fa, struct fuse_access_in *fai,
			    struct inode *inode, int mask)
{
	*fai = (struct fuse_access_in) {
		.mask = mask,
	};

	*fa = (struct fuse_args) {
		.opcode = FUSE_ACCESS,
		.nodeid = get_node_id(inode),
		.in_numargs = 1,
		.in_args[0].size = sizeof(*fai),
		.in_args[0].value = fai,
	};

	return 0;
}

int fuse_access_backing(struct fuse_args *fa, struct inode *inode, int mask)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	const struct fuse_access_in *fai = fa->in_args[0].value;

	return inode_permission(/* For mainline: init_user_ns,*/
				fi->backing_inode, fai->mask);
}

void *fuse_access_finalize(struct fuse_args *fa, struct inode *inode, int mask)
{
	return NULL;
}

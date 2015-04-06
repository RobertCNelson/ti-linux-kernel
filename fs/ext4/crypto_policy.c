/*
 * linux/fs/ext4/crypto_policy.c
 *
 * This contains encryption policy functions for ext4
 *
 * Written by Michael Halcrow, 2015.
 */

#include <linux/random.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ext4.h"
#include "xattr.h"

/**
 * ext4_to_hex() - Converts to hexadecimal characters
 * @dst: Buffer to take hex character representation of contents of
 *       src. Must be at least of size (src_size * 2).
 * @src: Buffer to be converted to a hex string respresentation.
 * @src_size: Number of bytes to convert.
 */
void ext4_to_hex(char *dst, char *src, size_t src_size)
{
	int x;

	for (x = 0; x < src_size; x++)
		sprintf(&dst[x * 2], "%.2x", (unsigned char)src[x]);
}

/**
 *
 */
static int ext4_inode_has_encryption_context(struct inode *inode)
{
	int res = ext4_xattr_get(inode, EXT4_XATTR_INDEX_ENCRYPTION,
				 EXT4_XATTR_NAME_ENCRYPTION_CONTEXT, NULL, 0);
	return (res > 0);
}

/**
 * ext4_is_encryption_context_consistent_with_policy() - Checks whether the policy is consistent with the encryption context for the inode
 * @inode:  ...
 * @policy: ...
 *
 * Return ...
 */
static int ext4_is_encryption_context_consistent_with_policy(
	struct inode *inode, const struct ext4_encryption_policy *policy)
{
	struct ext4_encryption_context ctx;
	int res = ext4_xattr_get(inode, EXT4_XATTR_INDEX_ENCRYPTION,
				 EXT4_XATTR_NAME_ENCRYPTION_CONTEXT, &ctx,
				 sizeof(ctx));
	if (res != sizeof(ctx))
		return 0;
	return (memcmp(ctx.master_key_descriptor, policy->master_key_descriptor,
			EXT4_KEY_DESCRIPTOR_SIZE) == 0 &&
		(ctx.contents_encryption_mode ==
		 policy->contents_encryption_mode) &&
		(ctx.filenames_encryption_mode ==
		 policy->filenames_encryption_mode));
}

static int ext4_create_encryption_context_from_policy(
	struct inode *inode, const struct ext4_encryption_policy *policy)
{
	struct ext4_encryption_context ctx;
	int res = 0;

	ctx.format = EXT4_ENCRYPTION_CONTEXT_FORMAT_V0;
	memcpy(ctx.master_key_descriptor, policy->master_key_descriptor,
	       EXT4_KEY_DESCRIPTOR_SIZE);
	ctx.contents_encryption_mode = ext4_validate_encryption_mode(
		policy->contents_encryption_mode);
	if (ctx.contents_encryption_mode == EXT4_ENCRYPTION_MODE_INVALID) {
		printk(KERN_WARNING
		       "%s: Invalid contents encryption mode %d\n", __func__,
			policy->contents_encryption_mode);
		res = -EINVAL;
		goto out;
	}
	ctx.filenames_encryption_mode = ext4_validate_encryption_mode(
		policy->filenames_encryption_mode);
	if (ctx.filenames_encryption_mode == EXT4_ENCRYPTION_MODE_INVALID) {
		printk(KERN_WARNING
		       "%s: Invalid filenames encryption mode %d\n", __func__,
			policy->filenames_encryption_mode);
		res = -EINVAL;
		goto out;
	}
	BUILD_BUG_ON(sizeof(ctx.nonce) != EXT4_KEY_DERIVATION_NONCE_SIZE);
	get_random_bytes(ctx.nonce, EXT4_KEY_DERIVATION_NONCE_SIZE);

	res = ext4_xattr_set(inode, EXT4_XATTR_INDEX_ENCRYPTION,
			     EXT4_XATTR_NAME_ENCRYPTION_CONTEXT, &ctx,
			     sizeof(ctx), 0);
out:
	if (!res)
		ext4_set_inode_flag(inode, EXT4_INODE_ENCRYPT);
	return res;
}

int ext4_process_policy(const struct ext4_encryption_policy *policy,
			struct inode *inode)
{
	if (policy->version != 0)
		return -EINVAL;

	if (!ext4_inode_has_encryption_context(inode)) {
		if (!ext4_empty_dir(inode))
			return -ENOTEMPTY;
		return ext4_create_encryption_context_from_policy(inode,
								  policy);
	}

	if (ext4_is_encryption_context_consistent_with_policy(inode, policy))
		return 0;

	printk(KERN_WARNING "%s: Policy inconsistent with encryption context\n",
	       __func__);
	return -EINVAL;
}

int ext4_get_policy(struct inode *inode, struct ext4_encryption_policy *policy)
{
	struct ext4_encryption_context ctx;

	int res = ext4_xattr_get(inode, EXT4_XATTR_INDEX_ENCRYPTION,
				 EXT4_XATTR_NAME_ENCRYPTION_CONTEXT,
				 &ctx, sizeof(ctx));
	if (res != sizeof(ctx))
		return -ENOENT;
	if (ctx.format != EXT4_ENCRYPTION_CONTEXT_FORMAT_V0)
		return -EINVAL;
	policy->version = 0;
	policy->contents_encryption_mode = ctx.contents_encryption_mode;
	policy->filenames_encryption_mode = ctx.filenames_encryption_mode;
	memcpy(&policy->master_key_descriptor, ctx.master_key_descriptor,
	       EXT4_KEY_DESCRIPTOR_SIZE);
	return 0;
}

int ext4_is_child_context_consistent_with_parent(struct inode *parent,
						 struct inode *child)
{
	struct ext4_encryption_context parent_ctx, child_ctx;
	int res = ext4_xattr_get(parent, EXT4_XATTR_INDEX_ENCRYPTION,
				 EXT4_XATTR_NAME_ENCRYPTION_CONTEXT,
				 &parent_ctx, sizeof(parent_ctx));

	if (res != sizeof(parent_ctx))
		return 0;
	res = ext4_xattr_get(parent, EXT4_XATTR_INDEX_ENCRYPTION,
			     EXT4_XATTR_NAME_ENCRYPTION_CONTEXT,
			     &child_ctx, sizeof(child_ctx));
	if (res != sizeof(child_ctx))
		return 0;
	return (memcmp(parent_ctx.master_key_descriptor,
		       child_ctx.master_key_descriptor,
		       EXT4_KEY_DESCRIPTOR_SIZE) == 0 &&
		(parent_ctx.contents_encryption_mode ==
		 child_ctx.contents_encryption_mode) &&
		(parent_ctx.filenames_encryption_mode ==
		 child_ctx.filenames_encryption_mode));
}

/**
 * ext4_inherit_context() - Sets a child context from its parent
 * @parent: Parent inode from which the context is inherited.
 * @child:  Child inode that inherits the context from @parent.
 *
 * Return: Zero on success, non-zero otherwise
 */
int ext4_inherit_context(struct inode *parent, struct inode *child)
{
	struct ext4_encryption_context ctx;
	struct ext4_sb_info *sbi = EXT4_SB(parent->i_sb);
	int res = ext4_xattr_get(parent, EXT4_XATTR_INDEX_ENCRYPTION,
				 EXT4_XATTR_NAME_ENCRYPTION_CONTEXT,
				 &ctx, sizeof(ctx));

	if (res != sizeof(ctx)) {
		if (unlikely(sbi->s_mount_flags &
			     EXT4_MF_TEST_DUMMY_ENCRYPTION)) {
			ctx.format = EXT4_ENCRYPTION_CONTEXT_FORMAT_V0;
			ctx.contents_encryption_mode =
				EXT4_ENCRYPTION_MODE_AES_256_XTS;
			ctx.filenames_encryption_mode =
				EXT4_ENCRYPTION_MODE_AES_256_CTS;
			memset(ctx.master_key_descriptor, 0x42,
			       EXT4_KEY_DESCRIPTOR_SIZE);
			res = 0;
		} else {
			goto out;
		}
	}
	get_random_bytes(ctx.nonce, EXT4_KEY_DERIVATION_NONCE_SIZE);
	res = ext4_xattr_set(child, EXT4_XATTR_INDEX_ENCRYPTION,
			     EXT4_XATTR_NAME_ENCRYPTION_CONTEXT, &ctx,
			     sizeof(ctx), 0);
out:
	if (!res)
		ext4_set_inode_flag(child, EXT4_INODE_ENCRYPT);
	return res;
}

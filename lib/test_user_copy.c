/*
 * Kernel module for testing copy_to/from_user infrastructure.
 *
 * Copyright 2013 Google Inc. All Rights Reserved
 *
 * Authors:
 *      Kees Cook       <keescook@chromium.org>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mman.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <net/checksum.h>

#define test(condition, msg)		\
({					\
	int cond = (condition);		\
	if (cond)			\
		pr_warn("%s\n", msg);	\
	cond;				\
})

static int __init test_user_copy_init(void)
{
	int ret = 0;
	char *kmem;
	char __user *usermem;
	char *bad_usermem;
	unsigned long user_addr;
	unsigned long value = 0x5A;
	int err;
	mm_segment_t fs = get_fs();

	kmem = kmalloc(PAGE_SIZE * 2, GFP_KERNEL);
	if (!kmem)
		return -ENOMEM;

	user_addr = vm_mmap(NULL, 0, PAGE_SIZE * 2,
			    PROT_READ | PROT_WRITE | PROT_EXEC,
			    MAP_ANONYMOUS | MAP_PRIVATE, 0);
	if (user_addr >= (unsigned long)(TASK_SIZE)) {
		pr_warn("Failed to allocate user memory\n");
		kfree(kmem);
		return -ENOMEM;
	}

	usermem = (char __user *)user_addr;
	bad_usermem = (char *)user_addr;

	/* Legitimate usage: none of these should fail. */
	ret |= test(copy_from_user(kmem, usermem, PAGE_SIZE),
		    "legitimate copy_from_user failed");
	ret |= test(copy_to_user(usermem, kmem, PAGE_SIZE),
		    "legitimate copy_to_user failed");
	ret |= test(copy_in_user(usermem, usermem + PAGE_SIZE, PAGE_SIZE),
		    "legitimate copy_in_user failed");
	ret |= test(get_user(value, (unsigned long __user *)usermem),
		    "legitimate get_user failed");
	ret |= test(put_user(value, (unsigned long __user *)usermem),
		    "legitimate put_user failed");
	ret |= test(clear_user(usermem, PAGE_SIZE) != 0,
		    "legitimate clear_user passed");
	ret |= test(strncpy_from_user(kmem, usermem, PAGE_SIZE) < 0,
		    "legitimate strncpy_from_user failed");
	ret |= test(strnlen_user(usermem, PAGE_SIZE) == 0,
		    "legitimate strnlen_user failed");
	ret |= test(strlen_user(usermem) == 0,
		    "legitimate strlen_user failed");
	err = 0;
	csum_and_copy_from_user(usermem, kmem, PAGE_SIZE, 0, &err);
	ret |= test(err, "legitimate csum_and_copy_from_user failed");
	err = 0;
	csum_and_copy_to_user(kmem, usermem, PAGE_SIZE, 0, &err);
	ret |= test(err, "legitimate csum_and_copy_to_user failed");

	ret |= test(!access_ok(VERIFY_READ, usermem, PAGE_SIZE * 2),
		    "legitimate access_ok VERIFY_READ failed");
	ret |= test(!access_ok(VERIFY_WRITE, usermem, PAGE_SIZE * 2),
		    "legitimate access_ok VERIFY_WRITE failed");
	ret |= test(__copy_from_user(kmem, usermem, PAGE_SIZE),
		    "legitimate __copy_from_user failed");
	ret |= test(__copy_from_user_inatomic(kmem, usermem, PAGE_SIZE),
		    "legitimate __copy_from_user_inatomic failed");
	ret |= test(__copy_to_user(usermem, kmem, PAGE_SIZE),
		    "legitimate __copy_to_user failed");
	ret |= test(__copy_to_user_inatomic(usermem, kmem, PAGE_SIZE),
		    "legitimate __copy_to_user_inatomic failed");
	ret |= test(__copy_in_user(usermem, usermem + PAGE_SIZE, PAGE_SIZE),
		    "legitimate __copy_in_user failed");
	ret |= test(__get_user(value, (unsigned long __user *)usermem),
		    "legitimate __get_user failed");
	ret |= test(__put_user(value, (unsigned long __user *)usermem),
		    "legitimate __put_user failed");
	ret |= test(__clear_user(usermem, PAGE_SIZE) != 0,
		    "legitimate __clear_user passed");
	err = 0;
	csum_partial_copy_from_user(usermem, kmem, PAGE_SIZE, 0, &err);
	ret |= test(err, "legitimate csum_partial_copy_from_user failed");

	/* Invalid usage: none of these should succeed. */
	ret |= test(!copy_from_user(kmem, (char __user *)(kmem + PAGE_SIZE),
				    PAGE_SIZE),
		    "illegal all-kernel copy_from_user passed");
	ret |= test(!copy_from_user(bad_usermem, (char __user *)kmem,
				    PAGE_SIZE),
		    "illegal reversed copy_from_user passed");
	ret |= test(!copy_to_user((char __user *)kmem, kmem + PAGE_SIZE,
				  PAGE_SIZE),
		    "illegal all-kernel copy_to_user passed");
	ret |= test(!copy_to_user((char __user *)kmem, bad_usermem,
				  PAGE_SIZE),
		    "illegal reversed copy_to_user passed");
	ret |= test(!copy_in_user((char __user *)kmem,
				  (char __user *)(kmem + PAGE_SIZE), PAGE_SIZE),
		    "illegal all-kernel copy_in_user passed");
	ret |= test(!copy_in_user((char __user *)kmem, usermem,
				  PAGE_SIZE),
		    "illegal copy_in_user to kernel passed");
	ret |= test(!copy_in_user(usermem, (char __user *)kmem,
				  PAGE_SIZE),
		    "illegal copy_in_user from kernel passed");
	ret |= test(!get_user(value, (unsigned long __user *)kmem),
		    "illegal get_user passed");
	ret |= test(!put_user(value, (unsigned long __user *)kmem),
		    "illegal put_user passed");
	ret |= test(clear_user((char __user *)kmem, PAGE_SIZE) != PAGE_SIZE,
		    "illegal kernel clear_user passed");
	ret |= test(strncpy_from_user(kmem, (char __user *)(kmem + PAGE_SIZE),
				      PAGE_SIZE) >= 0,
		    "illegal all-kernel strncpy_from_user passed");
	ret |= test(strncpy_from_user(bad_usermem, (char __user *)kmem,
				      PAGE_SIZE) >= 0,
		    "illegal reversed strncpy_from_user passed");
	ret |= test(strnlen_user((char __user *)kmem, PAGE_SIZE) != 0,
		    "illegal strnlen_user passed");
	ret |= test(strlen_user((char __user *)kmem) != 0,
		    "illegal strlen_user passed");
	err = 0;
	csum_and_copy_from_user((char __user *)(kmem + PAGE_SIZE), kmem,
				PAGE_SIZE, 0, &err);
	ret |= test(!err, "illegal all-kernel csum_and_copy_from_user passed");
	err = 0;
	csum_and_copy_from_user((char __user *)kmem, bad_usermem,
				PAGE_SIZE, 0, &err);
	ret |= test(!err, "illegal reversed csum_and_copy_from_user passed");
	err = 0;
	csum_and_copy_to_user(kmem, (char __user *)(kmem + PAGE_SIZE),
			      PAGE_SIZE, 0, &err);
	ret |= test(!err, "illegal all-kernel csum_and_copy_to_user passed");
	err = 0;
	csum_and_copy_to_user(bad_usermem, (char __user *)kmem, PAGE_SIZE, 0,
			      &err);
	ret |= test(!err, "illegal reversed csum_and_copy_to_user passed");

	/*
	 * If unchecked user accesses (__*) on this architecture cannot access
	 * kernel mode (i.e. access_ok() is redundant), and usually faults when
	 * attempted, check this behaviour.
	 *
	 * These tests are enabled for:
	 * - MIPS with Enhanced Virtual Addressing (EVA): user accesses use EVA
	 *   instructions which can only access user mode accessible memory. It
	 *   is assumed to be unlikely that user address space mappings will
	 *   intersect the kernel buffer address.
	 */
#if defined(CONFIG_MIPS) && defined(CONFIG_EVA)
	ret |= test(!__copy_from_user(kmem, (char __user *)(kmem + PAGE_SIZE),
				      PAGE_SIZE),
		    "illegal all-kernel __copy_from_user passed");
	ret |= test(!__copy_from_user(bad_usermem, (char __user *)kmem,
				      PAGE_SIZE),
		    "illegal reversed __copy_from_user passed");
	ret |= test(!__copy_from_user_inatomic(kmem,
					(char __user *)(kmem + PAGE_SIZE),
					PAGE_SIZE),
		    "illegal all-kernel __copy_from_user_inatomic passed");
	ret |= test(!__copy_from_user_inatomic(bad_usermem, (char __user *)kmem,
					       PAGE_SIZE),
		    "illegal reversed __copy_from_user_inatomic passed");
	ret |= test(!__copy_to_user((char __user *)kmem, kmem + PAGE_SIZE,
				    PAGE_SIZE),
		    "illegal all-kernel __copy_to_user passed");
	ret |= test(!__copy_to_user((char __user *)kmem, bad_usermem,
				    PAGE_SIZE),
		    "illegal reversed __copy_to_user passed");
	ret |= test(!__copy_to_user_inatomic((char __user *)kmem,
					     kmem + PAGE_SIZE, PAGE_SIZE),
		    "illegal all-kernel __copy_to_user_inatomic passed");
	ret |= test(!__copy_to_user_inatomic((char __user *)kmem, bad_usermem,
					     PAGE_SIZE),
		    "illegal reversed __copy_to_user_inatomic passed");
	ret |= test(!__copy_in_user((char __user *)kmem,
				    (char __user *)(kmem + PAGE_SIZE),
				    PAGE_SIZE),
		    "illegal all-kernel __copy_in_user passed");
	ret |= test(!__copy_in_user((char __user *)kmem, usermem,
				    PAGE_SIZE),
		    "illegal __copy_in_user to kernel passed");
	ret |= test(!__copy_in_user(usermem, (char __user *)kmem,
				    PAGE_SIZE),
		    "illegal __copy_in_user from kernel passed");
	ret |= test(!__get_user(value, (unsigned long __user *)kmem),
		    "illegal __get_user passed");
	ret |= test(!__put_user(value, (unsigned long __user *)kmem),
		    "illegal __put_user passed");
	ret |= test(__clear_user((char __user *)kmem, PAGE_SIZE) != PAGE_SIZE,
		    "illegal kernel __clear_user passed");
	err = 0;
	csum_partial_copy_from_user((char __user *)(kmem + PAGE_SIZE), kmem,
				    PAGE_SIZE, 0, &err);
	ret |= test(!err,
		    "illegal all-kernel csum_partial_copy_from_user passed");
	err = 0;
	csum_partial_copy_from_user((char __user *)kmem, bad_usermem, PAGE_SIZE,
				    0, &err);
	ret |= test(!err,
		    "illegal reversed csum_partial_copy_from_user passed");
#endif

	/*
	 * Test access to kernel memory by adjusting address limit.
	 * This is used by the kernel to invoke system calls with kernel
	 * pointers.
	 */
	set_fs(get_ds());

	/* Legitimate usage: none of these should fail. */
	ret |= test(copy_from_user(kmem, (char __user *)(kmem + PAGE_SIZE),
				   PAGE_SIZE),
		    "legitimate all-kernel copy_from_user failed");
	ret |= test(copy_to_user((char __user *)kmem, kmem + PAGE_SIZE,
				 PAGE_SIZE),
		    "legitimate all-kernel copy_to_user failed");
	ret |= test(copy_in_user((char __user *)kmem,
				 (char __user *)(kmem + PAGE_SIZE), PAGE_SIZE),
		    "legitimate all-kernel copy_in_user failed");
	ret |= test(get_user(value, (unsigned long __user *)kmem),
		    "legitimate kernel get_user failed");
	ret |= test(put_user(value, (unsigned long __user *)kmem),
		    "legitimate kernel put_user failed");
	ret |= test(clear_user((char __user *)kmem, PAGE_SIZE) != 0,
		    "legitimate kernel clear_user failed");
	ret |= test(strncpy_from_user(kmem, (char __user *)(kmem + PAGE_SIZE),
				      PAGE_SIZE) < 0,
		    "legitimate all-kernel strncpy_from_user failed");
	ret |= test(strnlen_user((char __user *)kmem, PAGE_SIZE) == 0,
		    "legitimate kernel strnlen_user failed");
	ret |= test(strlen_user((char __user *)kmem) == 0,
		    "legitimate kernel strlen_user failed");
	err = 0;
	csum_and_copy_from_user((char __user *)(kmem + PAGE_SIZE), kmem,
				PAGE_SIZE, 0, &err);
	ret |= test(err, "legitimate kernel csum_and_copy_from_user failed");
	err = 0;
	csum_and_copy_to_user(kmem, (char __user *)(kmem + PAGE_SIZE),
			      PAGE_SIZE, 0, &err);
	ret |= test(err, "legitimate kernel csum_and_copy_to_user failed");

	ret |= test(!access_ok(VERIFY_READ, (char __user *)kmem, PAGE_SIZE * 2),
		    "legitimate kernel access_ok VERIFY_READ failed");
	ret |= test(!access_ok(VERIFY_WRITE, (char __user *)kmem,
			       PAGE_SIZE * 2),
		    "legitimate kernel access_ok VERIFY_WRITE failed");
	ret |= test(__copy_from_user(kmem, (char __user *)(kmem + PAGE_SIZE),
				     PAGE_SIZE),
		    "legitimate all-kernel __copy_from_user failed");
	ret |= test(__copy_from_user_inatomic(kmem,
					      (char __user *)(kmem + PAGE_SIZE),
					      PAGE_SIZE),
		    "legitimate all-kernel __copy_from_user_inatomic failed");
	ret |= test(__copy_to_user((char __user *)kmem, kmem + PAGE_SIZE,
				   PAGE_SIZE),
		    "legitimate all-kernel __copy_to_user failed");
	ret |= test(__copy_to_user_inatomic((char __user *)kmem,
					    kmem + PAGE_SIZE, PAGE_SIZE),
		    "legitimate all-kernel __copy_to_user_inatomic failed");
	ret |= test(__copy_in_user((char __user *)kmem,
				   (char __user *)(kmem + PAGE_SIZE),
				   PAGE_SIZE),
		    "legitimate all-kernel __copy_in_user failed");
	ret |= test(__get_user(value, (unsigned long __user *)kmem),
		    "legitimate kernel __get_user failed");
	ret |= test(__put_user(value, (unsigned long __user *)kmem),
		    "legitimate kernel __put_user failed");
	ret |= test(__clear_user((char __user *)kmem, PAGE_SIZE) != 0,
		    "legitimate kernel __clear_user failed");
	err = 0;
	csum_partial_copy_from_user((char __user *)(kmem + PAGE_SIZE), kmem,
				    PAGE_SIZE, 0, &err);
	ret |= test(err,
		    "legitimate kernel csum_partial_copy_from_user failed");

	/* Restore previous address limit. */
	set_fs(fs);

	vm_munmap(user_addr, PAGE_SIZE * 2);
	kfree(kmem);

	if (ret == 0) {
		pr_info("tests passed.\n");
		return 0;
	}

	return -EINVAL;
}

module_init(test_user_copy_init);

static void __exit test_user_copy_exit(void)
{
	pr_info("unloaded.\n");
}

module_exit(test_user_copy_exit);

MODULE_AUTHOR("Kees Cook <keescook@chromium.org>");
MODULE_LICENSE("GPL");

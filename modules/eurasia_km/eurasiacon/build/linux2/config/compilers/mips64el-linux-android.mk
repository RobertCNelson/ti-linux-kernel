# 64-bit MIPS R6 little-endian compiler
IS_KERNEL_32 := 0
ifneq ($(KERNELDIR),)
  IS_KERNEL_32 = ($(shell grep -q "CONFIG_MIPS=y" $(KERNELDIR)/.config && echo 1 || echo 0))
 ifneq ($(ARCH),mips)
  ifeq ($(IS_KERNEL_32),1)
   $(warning ******************************************************)
   $(warning Your kernel appears to be configured for 32-bit MIPS,)
   $(warning but CROSS_COMPILE (or KERNEL_CROSS_COMPILE) points)
   $(warning to a 64-bit compiler.)
   $(warning If you want a 32-bit build, either set CROSS_COMPILE)
   $(warning to point to a 32-bit compiler, or build with ARCH=mips)
   $(warning to force 32-bit mode with your existing compiler.)
   $(warning ******************************************************)
   $(error Invalid CROSS_COMPILE / kernel architecture combination)
  endif # CONFIG_X86_32
 endif # ARCH=mips
endif # KERNELDIR

# If ARCH=mips is set, force a build for 32-bit only, even though we're
# using a 64-bit compiler.
ifeq ($(ARCH),mips)
 TARGET_PRIMARY_ARCH := target_mips32r6el
 ifeq ($(IS_KERNEL_32),0)
	USE_64BIT_COMPAT := 1
 endif
else
 $(error MIPS64 build is not supported)
endif

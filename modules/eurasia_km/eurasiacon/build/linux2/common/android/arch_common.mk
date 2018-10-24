########################################################################### ###
#@Copyright     Copyright (c) Imagination Technologies Ltd. All Rights Reserved
#@License       Dual MIT/GPLv2
# 
# The contents of this file are subject to the MIT license as set out below.
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# Alternatively, the contents of this file may be used under the terms of
# the GNU General Public License Version 2 ("GPL") in which case the provisions
# of GPL are applicable instead of those above.
# 
# If you wish to allow use of your version of this file only under the terms of
# GPL, and not to allow others to use your version of this file under the terms
# of the MIT license, indicate your decision by deleting the provisions above
# and replace them with the notice and other provisions required by GPL as set
# out in the file called "GPL-COPYING" included in this distribution. If you do
# not delete the provisions above, a recipient may use your version of this file
# under the terms of either the MIT license or GPL.
# 
# This License is also included in this distribution in the file called
# "MIT-COPYING".
# 
# EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
# PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
# BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
# PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
### ###########################################################################

SYS_CFLAGS := \
 -fno-short-enums \
 -funwind-tables \
 -D__linux__

SYS_INCLUDES :=

ifneq ($(TARGET_PLATFORM),)

 # Support for building with the Android NDK >= r15b.
 # The NDK provides only the most basic includes and libraries.

 SYS_INCLUDES += \
  -isystem $(NDK_PLATFORMS_ROOT)/$(TARGET_PLATFORM)/arch-$(TARGET_ARCH)/usr/include \
  -isystem $(NDK_SYSROOT)/usr/include/drm \
  -isystem $(NDK_SYSROOT)/usr/include

else # !TARGET_PLATFORM

 # These libraries are not coming from the NDK now, so we need to include them
 # from the ANDROID_ROOT source tree.

 SYS_INCLUDES += \
  -isystem $(ANDROID_ROOT)/bionic/libc/include \
  -isystem $(ANDROID_ROOT)/bionic/libc/kernel/android/uapi \
  -isystem $(ANDROID_ROOT)/bionic/libc/kernel/uapi \
  -isystem $(ANDROID_ROOT)/bionic/libm/include \
  -isystem $(ANDROID_ROOT)/external/libdrm/include/drm \
  -isystem $(ANDROID_ROOT)/external/zlib/src \
  -isystem $(ANDROID_ROOT)/frameworks/native/include

 ifeq ($(is_future_version),1)
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/libnativehelper/include_jni
 else ifeq ($(is_aosp_master),1)
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/libnativehelper/include_jni
 else
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/libnativehelper/include/nativehelper
 endif

endif # !TARGET_PLATFORM

 # These components aren't in the NDK. They *are* in the VNDK. If this is an
 # NDK or non-NDK build, but not a VNDK build, include the needed bits from
 # the ANDROID_ROOT source tree. We put libsync first because the NDK copy
 # of the sync headers have been stripped in an unsupported way.

 SYS_INCLUDES := \
  -isystem $(ANDROID_ROOT)/system/core/libsync/include \
  $(SYS_INCLUDES) \
  -isystem $(ANDROID_ROOT)/external/libdrm \
  -isystem $(ANDROID_ROOT)/external/libpng \
  -isystem $(ANDROID_ROOT)/external/libunwind/include \
  -isystem $(ANDROID_ROOT)/hardware/libhardware/include \
  -isystem $(ANDROID_ROOT)/system/media/camera/include

 # boringssl replaced openssl from Marshmallow
 ifeq ($(is_at_least_marshmallow),1)
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/external/boringssl/src/include
 else
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/external/openssl/include
 endif

 # libjpeg-turbo replaced libjpeg from Nougat
 ifeq ($(is_at_least_nougat),1)
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/external/libjpeg-turbo
 else
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/external/jpeg
 endif

 # Handle upstream includes refactoring
 ifeq ($(is_at_least_oreo),1)
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/frameworks/native/libs/nativewindow/include \
   -isystem $(ANDROID_ROOT)/system/core/libbacktrace/include \
   -isystem $(ANDROID_ROOT)/system/core/libsystem/include \
   -isystem $(ANDROID_ROOT)/system/core/libutils/include
  ifeq ($(NDK_ROOT),)
   SYS_INCLUDES += \
    -isystem $(ANDROID_ROOT)/frameworks/native/libs/arect/include \
    -isystem $(ANDROID_ROOT)/system/core/liblog/include
  endif
 else
  SYS_INCLUDES += \
   -isystem $(ANDROID_ROOT)/frameworks/base/include \
   -isystem $(ANDROID_ROOT)/system/core/include
 endif

# This is comparing PVR_BUILD_DIR to see if it is omap and adding 
# includes required for it's HWC.
ifeq ($(notdir $(abspath .)),omap_android)
SYS_INCLUDES += \
 -isystem $(ANDROID_ROOT)/hardware/ti/omap4xxx/kernel-headers \
 -isystem $(ANDROID_ROOT)/hardware/ti/omap4xxx/ion
endif

# Always include the NDK compatibility directory, because it allows us to
# compile in inline versions of simple functions to eliminate dependencies,
# and we can also constrain the available APIs. Do this last, so we can
# make sure it is always first on the include list.

SYS_INCLUDES := -isystem eurasiacon/android/ndk $(SYS_INCLUDES)

# Android enables build-id sections to allow mapping binaries to debug
# information for symbol resolution
SYS_LDFLAGS += -Wl,--build-id=md5

SYS_EXE_LDFLAGS_CXX := -lstdc++

SYS_LIB_LDFLAGS_CXX := $(SYS_EXE_LDFLAGS_CXX)

OPTIM := -O2

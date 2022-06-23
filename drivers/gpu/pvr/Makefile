ccflags-y += \
	-D__linux__ \
	-include $(srctree)/drivers/gpu/pvr/config_kernel.h \
	-Idrivers/gpu/pvr/include4 \
	-Idrivers/gpu/pvr/services4/include \
	-Idrivers/gpu/pvr/services4/system/omap \
	-Idrivers/gpu/pvr/services4/system/include \
	-Idrivers/gpu/pvr/services4/srvkm/bridged \
	-Idrivers/gpu/pvr/services4/srvkm/bridged/sgx \
	-Idrivers/gpu/pvr/services4/srvkm/common \
	-Idrivers/gpu/pvr/services4/srvkm/devices/sgx \
	-Idrivers/gpu/pvr/services4/srvkm/env/linux \
	-Idrivers/gpu/pvr/services4/srvkm/include \
	-Idrivers/gpu/pvr/services4/srvkm/hwdefs

pvrsrvkm-y += \
	services4/srvkm/bridged/bridged_pvr_bridge.o \
	services4/srvkm/bridged/bridged_support.o \
	services4/srvkm/bridged/sgx/bridged_sgx_bridge.o \
	services4/srvkm/common/buffer_manager.o \
	services4/srvkm/common/devicemem.o \
	services4/srvkm/common/handle.o \
	services4/srvkm/common/hash.o \
	services4/srvkm/common/lists.o \
	services4/srvkm/common/mem.o \
	services4/srvkm/common/mem_debug.o \
	services4/srvkm/common/metrics.o \
	services4/srvkm/common/osfunc_common.o \
	services4/srvkm/common/pdump_common.o \
	services4/srvkm/common/perproc.o \
	services4/srvkm/common/power.o \
	services4/srvkm/common/pvrsrv.o \
	services4/srvkm/common/ra.o \
	services4/srvkm/common/refcount.o \
	services4/srvkm/common/resman.o \
	services4/srvkm/devices/sgx/mmu.o \
	services4/srvkm/devices/sgx/pb.o \
	services4/srvkm/devices/sgx/sgxinit.o \
	services4/srvkm/devices/sgx/sgxkick.o \
	services4/srvkm/devices/sgx/sgxpower.o \
	services4/srvkm/devices/sgx/sgxreset.o \
	services4/srvkm/devices/sgx/sgxtransfer.o \
	services4/srvkm/devices/sgx/sgxutils.o \
	services4/srvkm/env/linux/dmabuf.o \
	services4/srvkm/env/linux/dma_fence_sync_native_server.o \
	services4/srvkm/env/linux/event.o \
	services4/srvkm/env/linux/mmap.o \
	services4/srvkm/env/linux/mm.o \
	services4/srvkm/env/linux/module.o \
	services4/srvkm/env/linux/mutex.o \
	services4/srvkm/env/linux/mutils.o \
	services4/srvkm/env/linux/osfunc.o \
	services4/srvkm/env/linux/osperproc.o \
	services4/srvkm/env/linux/pdump.o \
	services4/srvkm/env/linux/proc.o \
	services4/srvkm/env/linux/pvr_bridge_k.o \
	services4/srvkm/env/linux/pvr_counting_timeline.o \
	services4/srvkm/env/linux/pvr_debug.o \
	services4/srvkm/env/linux/pvr_fence.o \
	services4/srvkm/env/linux/pvr_linux_fence.o \
	services4/srvkm/env/linux/pvr_sw_fence.o \
	services4/srvkm/env/linux/pvr_sync_common.o \
	services4/system/omap/sysconfig.o \
	services4/system/omap/sysutils.o \

obj-$(CONFIG_PVR_SGX544) += pvrsrvkm.o

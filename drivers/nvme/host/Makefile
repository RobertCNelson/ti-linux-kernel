
obj-$(CONFIG_BLK_DEV_NVME)     += nvme.o

lightnvm-$(CONFIG_NVM)	:= lightnvm.o
nvme-y		+= core.o pci.o scsi.o $(lightnvm-y)

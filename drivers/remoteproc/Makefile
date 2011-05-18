#
# Generic framework for controlling remote processors
#

obj-$(CONFIG_REMOTE_PROC)		+= remoteproc.o
obj-$(CONFIG_OMAP_REMOTE_PROC)		+= omap_remoteproc.o
rpres-$(CONFIG_OMAP_RPRES)              := rpres_dev.o
obj-y					+= $(rpres-m) $(rpres-y)
obj-$(CONFIG_OMAP_RPRES)		+= rpres.o

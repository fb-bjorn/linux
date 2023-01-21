obj-$(CONFIG_FBACCEL) = fbaccel.o

fbaccel-y := fba_admin.o
fbaccel-y += fba_alloc.o
fbaccel-y += fba_channel.o
fbaccel-y += fba_ctx.o
fbaccel-y += fba_device.o
fbaccel-y += fba_dma.o
fbaccel-y += fba_dmabuf.o
fbaccel-y += fba_freya.o
fbaccel-y += fba_io.o
fbaccel-y += fba_job.o
fbaccel-y += fba_mon.o
fbaccel-y += fba_poller.o
fbaccel-y += fba_resource.o
fbaccel-y += fba_shared.o
fbaccel-y += fba_tenjin.o
fbaccel-y += fba_trace.o
fbaccel-y += fba_uart.o
fbaccel-y += fba_util.o

# define_trace.h needs to know how to find our header
CFLAGS_fba_trace.o     := -I$(src)

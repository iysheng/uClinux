ccflags-$(CONFIG_ARCH_MULTIPLATFORM) := -I$(srctree)/$(src)/include \
	-I$(srctree)/arch/arm/plat-orion/include

AFLAGS_coherency_ll.o		:= -Wa,-march=armv7-a
CFLAGS_pmsu.o			:= -march=armv7-a

obj-$(CONFIG_MACH_MVEBU_ANY)	 += system-controller.o mvebu-soc-id.o

ifeq ($(CONFIG_MACH_MVEBU_V7),y)
obj-y				 += cpu-reset.o board-v7.o coherency.o coherency_ll.o pmsu.o pmsu_ll.o pm.o pm-board.o
obj-$(CONFIG_SMP)		 += platsmp.o headsmp.o platsmp-a9.o headsmp-a9.o
endif

obj-$(CONFIG_MACH_DOVE)		 += dove.o
obj-$(CONFIG_MACH_KIRKWOOD)	 += kirkwood.o kirkwood-pm.o
obj-$(CONFIG_MACH_8300)          += 8300.o
obj-$(CONFIG_MACH_5400_RM_DT)    += board-accelerated.o
obj-$(CONFIG_MACH_6300_EX_DT)    += board-accelerated.o
obj-$(CONFIG_MACH_6300CX)        += 6300.o
obj-$(CONFIG_MACH_6300LX)        += 6300.o board-accelerated.o
obj-$(CONFIG_MACH_6350SR)        += 6300.o board-accelerated.o

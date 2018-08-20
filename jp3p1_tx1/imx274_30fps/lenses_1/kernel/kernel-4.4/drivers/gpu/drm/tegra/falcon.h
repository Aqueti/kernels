/*
 * Copyright (C) 2015-2016 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _FALCON_H_
#define _FALCON_H_

#include <linux/types.h>

#define FALCON_UCLASS_METHOD_OFFSET		0x00000040

#define FALCON_UCLASS_METHOD_DATA		0x00000044

#define FALCON_IRQMSET				0x00001010
#define FALCON_IRQMSET_WDTMR			(1 << 1)
#define FALCON_IRQMSET_HALT			(1 << 4)
#define FALCON_IRQMSET_EXTERR			(1 << 5)
#define FALCON_IRQMSET_SWGEN0			(1 << 6)
#define FALCON_IRQMSET_SWGEN1			(1 << 7)
#define FALCON_IRQMSET_EXT(v)			(((v) & 0xff) << 8)

#define FALCON_IRQDEST				0x0000101c
#define FALCON_IRQDEST_HALT			(1 << 4)
#define FALCON_IRQDEST_EXTERR			(1 << 5)
#define FALCON_IRQDEST_SWGEN0			(1 << 6)
#define FALCON_IRQDEST_SWGEN1			(1 << 7)
#define FALCON_IRQDEST_EXT(v)			(((v) & 0xff) << 8)

#define FALCON_ITFEN				0x00001048
#define FALCON_ITFEN_CTXEN			(1 << 0)
#define FALCON_ITFEN_MTHDEN			(1 << 1)

#define FALCON_IDLESTATE			0x0000104c

#define FALCON_CPUCTL				0x00001100
#define FALCON_CPUCTL_STARTCPU			(1 << 1)

#define FALCON_BOOTVEC				0x00001104

#define FALCON_DMACTL				0x0000110c
#define FALCON_DMACTL_DMEM_SCRUBBING		(1 << 1)
#define FALCON_DMACTL_IMEM_SCRUBBING		(1 << 2)

#define FALCON_DMATRFBASE			0x00001110

#define FALCON_DMATRFMOFFS			0x00001114

#define FALCON_DMATRFCMD			0x00001118
#define FALCON_DMATRFCMD_IDLE			(1 << 1)
#define FALCON_DMATRFCMD_IMEM			(1 << 4)
#define FALCON_DMATRFCMD_SIZE_256B		(6 << 8)

#define FALCON_DMATRFFBOFFS			0x0000111c

struct falcon_firmware_bin_header_v1 {
	u32 bin_magic;		/* 0x10de */
	u32 bin_ver;		/* cya, versioning of bin format (1) */
	u32 bin_size;		/* entire image size including this header */
	u32 os_bin_header_offset;
	u32 os_bin_data_offset;
	u32 os_bin_size;
};

struct falcon_firmware_os_app_v1 {
	u32 offset;
	u32 size;
};

struct falcon_firmware_os_header_v1 {
	u32 os_code_offset;
	u32 os_code_size;
	u32 os_data_offset;
	u32 os_data_size;
	u32 num_apps;
	struct falcon_firmware_os_app_v1 *app_code;
	struct falcon_firmware_os_app_v1 *app_data;
	u32 *os_ovl_offset;
	u32 *os_ovl_size;
};

struct falcon;

struct falcon_ops {
	void *(*alloc)(struct falcon *falcon, size_t size,
		       dma_addr_t *paddr);
	void (*free)(struct falcon *falcon, size_t size,
		     dma_addr_t paddr, void *vaddr);
};

struct falcon_firmware_section {
	unsigned long offset;
	size_t size;
};

struct falcon_firmware {
	/* Raw firmware data */
	dma_addr_t paddr;
	void *vaddr;
	size_t size;

	/* Parsed firmware information */
	struct falcon_firmware_section bin_data;
	struct falcon_firmware_section data;
	struct falcon_firmware_section code;

	unsigned int os_start_offset;

	bool valid;
};

struct falcon {
	/* Set by falcon client */
	struct device *dev;
	void __iomem *regs;
	const struct falcon_ops *ops;
	void *data;

	struct falcon_firmware firmware;
};

int falcon_init(struct falcon *falcon);
void falcon_exit(struct falcon *falcon);
int falcon_read_firmware(struct falcon *falcon, const char *firmware_name);
int falcon_boot(struct falcon *falcon);
void falcon_execute_method(struct falcon *falcon, u32 method, u32 data);
int falcon_wait_idle(struct falcon *falcon);

#endif /* _FALCON_H_ */

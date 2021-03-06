/*
 * Copyright (C) 2014-2017 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/highmem.h>

#include <soc/tegra/common.h>
#include <linux/memblock.h>

/* Before T18x architecture */
static const struct of_device_id tegra210_le_machine_match[] = {
	{ .compatible = "nvidia,tegra20", },
	{ .compatible = "nvidia,tegra30", },
	{ .compatible = "nvidia,tegra114", },
	{ .compatible = "nvidia,tegra124", },
	{ .compatible = "nvidia,tegra132", },
	{ .compatible = "nvidia,tegra210", },
	{ .compatible = "nvidia,tegra210b01", },
	{ }
};

/* T186 and later architecture */
static const struct of_device_id tegra186_ge_machine_match[] = {
	{ .compatible = "nvidia,tegra186", },
	{ .compatible = "nvidia,tegra194", },
	{ }
};

phys_addr_t tegra_bootloader_fb_start;
phys_addr_t tegra_bootloader_fb_size;
phys_addr_t tegra_bootloader_fb2_start;
phys_addr_t tegra_bootloader_fb2_size;
phys_addr_t tegra_bootloader_fb3_start;
phys_addr_t tegra_bootloader_fb3_size;
phys_addr_t tegra_bootloader_lut_start;
phys_addr_t tegra_bootloader_lut_size;
phys_addr_t tegra_fb_start;
phys_addr_t tegra_fb_size;
phys_addr_t tegra_fb2_start;
phys_addr_t tegra_fb2_size;
phys_addr_t tegra_fb3_start;
phys_addr_t tegra_fb3_size;
phys_addr_t tegra_lut_start;
phys_addr_t tegra_lut_size;

static int usb_port_owner_info;
static int panel_id;
static struct board_info display_board_info;

bool soc_is_tegra210_n_before(void)
{
	struct device_node *root;

	root = of_find_node_by_path("/");
	if (!root)
		return false;

	return of_match_node(tegra210_le_machine_match, root) != NULL;
}

bool soc_is_tegra186_n_later(void)
{
	struct device_node *root;

	root = of_find_node_by_path("/");
	if (!root)
		return false;

	return of_match_node(tegra186_ge_machine_match, root) != NULL;
}

static int __init tegra_bootloader_fb_arg(char *options)
{
	char *p = options;

	tegra_bootloader_fb_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_fb_start = memparse(p+1, &p);

	pr_info("Found tegra_fbmem: %08llx@%08llx\n",
		(u64)tegra_bootloader_fb_size, (u64)tegra_bootloader_fb_start);

	if (tegra_bootloader_fb_size) {
		tegra_bootloader_fb_size = PAGE_ALIGN(tegra_bootloader_fb_size);

		if (memblock_reserve(tegra_bootloader_fb_start,
				tegra_bootloader_fb_size)) {
			pr_err("Failed to reserve bootloader fb %08llx@%08llx\n",
				(u64)tegra_bootloader_fb_size,
				(u64)tegra_bootloader_fb_start);
			tegra_bootloader_fb_start = 0;
			tegra_bootloader_fb_size = 0;
		}
	}


	return 0;
}
early_param("tegra_fbmem", tegra_bootloader_fb_arg);

static int __init tegra_bootloader_fb2_arg(char *options)
{
	char *p = options;

	tegra_bootloader_fb2_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_fb2_start = memparse(p+1, &p);

	pr_info("Found tegra_fbmem2: %08llx@%08llx\n",
		(u64)tegra_bootloader_fb2_size,
		(u64)tegra_bootloader_fb2_start);

	if (tegra_bootloader_fb2_size) {
		tegra_bootloader_fb2_size =
				PAGE_ALIGN(tegra_bootloader_fb2_size);
		if (memblock_reserve(tegra_bootloader_fb2_start,
				tegra_bootloader_fb2_size)) {
			pr_err("Failed to reserve bootloader fb2 %08llx@%08llx\n",
				(u64)tegra_bootloader_fb2_size,
				(u64)tegra_bootloader_fb2_start);
			tegra_bootloader_fb2_start = 0;
			tegra_bootloader_fb2_size = 0;
		}
	}

	return 0;
}
early_param("tegra_fbmem2", tegra_bootloader_fb2_arg);

static int __init tegra_bootloader_fb3_arg(char *options)
{
	char *p = options;

	tegra_bootloader_fb3_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_fb3_start = memparse(p+1, &p);

	pr_info("Found tegra_fbmem3: %08llx@%08llx\n",
		(u64)tegra_bootloader_fb3_size,
		(u64)tegra_bootloader_fb3_start);

	if (tegra_bootloader_fb3_size) {
		tegra_bootloader_fb3_size =
				PAGE_ALIGN(tegra_bootloader_fb3_size);
		if (memblock_reserve(tegra_bootloader_fb3_start,
				tegra_bootloader_fb3_size)) {
			pr_err("Failed to reserve bootloader fb3 %08llx@%08llx\n",
				(u64)tegra_bootloader_fb3_size,
				(u64)tegra_bootloader_fb3_start);
			tegra_bootloader_fb3_start = 0;
			tegra_bootloader_fb3_size = 0;
		}
	}

	return 0;
}
early_param("tegra_fbmem3", tegra_bootloader_fb3_arg);

static int __init tegra_bootloader_lut_arg(char *options)
{
	char *p = options;

	tegra_bootloader_lut_size = memparse(p, &p);
	if (*p == '@')
		tegra_bootloader_lut_start = memparse(p+1, &p);

	pr_info("Found lut_mem: %08llx@%08llx\n",
		(u64)tegra_bootloader_lut_size,
		(u64)tegra_bootloader_lut_start);

	if (tegra_bootloader_lut_size) {
		tegra_bootloader_lut_size =
				PAGE_ALIGN(tegra_bootloader_lut_size);
		if (memblock_reserve(tegra_bootloader_lut_start,
				tegra_bootloader_lut_size)) {
			pr_err("Failed to reserve bootloader lut_mem %08llx@%08llx\n",
				(u64)tegra_bootloader_lut_size,
				(u64)tegra_bootloader_lut_start);
			tegra_bootloader_lut_start = 0;
			tegra_bootloader_lut_size = 0;
		}
	}

	return 0;
}
early_param("lut_mem", tegra_bootloader_lut_arg);

/* returns true if bl initialized the display */
bool tegra_is_bl_display_initialized(int instance)
{
	/* display initialized implies non-zero
	 * fb size is passed from bl to kernel
	 */
	switch (instance) {
	case 0:
		return tegra_bootloader_fb_start && tegra_bootloader_fb_size;
	case 1:
		return tegra_bootloader_fb2_start && tegra_bootloader_fb2_size;
	case 2:
		return tegra_bootloader_fb3_start && tegra_bootloader_fb3_size;
	default:
		pr_err("Could not find DC instance %d\n", instance);
		return false;
	}
}

void tegra_get_fb_resource(struct resource *fb_res, int instance)
{
	if (!tegra_is_bl_display_initialized(instance)) {
		fb_res->start = 0;
		fb_res->end = 0;
	} else {
		switch (instance) {
		case 0:
			fb_res->start =
				(resource_size_t) tegra_bootloader_fb_start;
			fb_res->end = fb_res->start +
				(resource_size_t) tegra_bootloader_fb_size - 1;
			break;
		case 1:
			fb_res->start =
				(resource_size_t) tegra_bootloader_fb2_start;
			fb_res->end = fb_res->start +
				(resource_size_t) tegra_bootloader_fb2_size - 1;
			break;
		case 2:
			fb_res->start =
				(resource_size_t) tegra_bootloader_fb3_start;
			fb_res->end = fb_res->start +
				(resource_size_t) tegra_bootloader_fb3_size - 1;
			break;
		default:
			pr_err("Could not find DC instance %d\n", instance);
			break;
		}
	}

}

static int __init tegra_usb_port_owner_info(char *id)
{
	char *p = id;

	usb_port_owner_info = memparse(p, &p);
	return 1;
}

int tegra_get_usb_port_owner_info(void)
{
	return usb_port_owner_info;
}
EXPORT_SYMBOL(tegra_get_usb_port_owner_info);

__setup("usb_port_owner_info=", tegra_usb_port_owner_info);

int tegra_get_board_panel_id(void)
{
	return panel_id;
}
static int __init tegra_board_panel_id(char *options)
{
	char *p = options;
	panel_id = memparse(p, &p);
	return panel_id;
}
__setup("display_panel=", tegra_board_panel_id);



static int tegra_get_board_info_properties(struct board_info *bi,
		const char *property_name)
{
	struct device_node *board_info;
	char board_info_path[50] = {0};
	u32 prop_val;
	int err;


	strcpy(board_info_path, "/chosen/");
	strcat(board_info_path, property_name);

	board_info = of_find_node_by_path(board_info_path);
	memset(bi, 0, sizeof(*bi));

	if (board_info) {
		err = of_property_read_u32(board_info, "id", &prop_val);
		if (err < 0) {
			pr_err("failed to read %s/id\n", board_info_path);
			goto out;
		}
		bi->board_id = prop_val;

		err = of_property_read_u32(board_info, "sku", &prop_val);
		if (err < 0) {
			pr_err("failed to read %s/sku\n", board_info_path);
			goto out;
		}
		bi->sku = prop_val;

		err = of_property_read_u32(board_info, "fab", &prop_val);
		if (err < 0) {
			pr_err("failed to read %s/fab\n", board_info_path);
			goto out;
		}
		bi->fab = prop_val;

		err = of_property_read_u32(board_info,
					"major_revision", &prop_val);
		if (err < 0) {
			pr_err("failed to read %s/major_revision\n",
					board_info_path);
			goto out;
		}
		bi->major_revision = prop_val;

		err = of_property_read_u32(board_info,
					"minor_revision", &prop_val);
		if (err < 0) {
			pr_err("failed to read %s/minor_revision\n",
					board_info_path);
			goto out;
		}
		bi->minor_revision = prop_val;
		return 0;
	}

	pr_err("Node path %s not found\n", board_info_path);
out:
	return -1;
}

void tegra_get_display_board_info(struct board_info *bi)
{
	static bool parsed = 0;

	if (!parsed) {
		int ret;
		parsed = 1;
		ret = tegra_get_board_info_properties(bi, "display-board");
		if (!ret)
			memcpy(&display_board_info, bi,
					sizeof(struct board_info));
	}
	memcpy(bi, &display_board_info, sizeof(struct board_info));
}

/*
 * Due to conflicting restrictions on the placement of the framebuffer,
 * the bootloader is likely to leave the framebuffer pointed at a location
 * in memory that is outside the grhost aperture.  This function will move
 * the framebuffer contents from a physical address that is anywhere (lowmem,
 * highmem, or outside the memory map) to a physical address that is outside
 * the memory map.
 */
void __tegra_move_framebuffer(struct platform_device *pdev,
	phys_addr_t to, phys_addr_t from,
	size_t size)
{
	struct page *page;
	void __iomem *to_io;
	void *from_virt;
	unsigned long i;

	BUG_ON(PAGE_ALIGN((unsigned long)to) != (unsigned long)to);
	BUG_ON(PAGE_ALIGN(from) != from);
	BUG_ON(PAGE_ALIGN(size) != size);

	to_io = ioremap_wc(to, size);
	if (!to_io) {
		pr_err("%s: Failed to map target framebuffer\n", __func__);
		return;
	}

	if (from && pfn_valid(page_to_pfn(phys_to_page(from)))) {
		for (i = 0 ; i < size; i += PAGE_SIZE) {
			page = phys_to_page(from + i);
			from_virt = kmap(page);
			memcpy(to_io + i, from_virt, PAGE_SIZE);
			kunmap(page);
		}
	} else if (from) {
		void __iomem *from_io = ioremap_wc(from, size);
		if (!from_io) {
			pr_err("%s: Failed to map source framebuffer\n",
				__func__);
			goto out;
		}

		for (i = 0; i < size; i += 4)
			writel_relaxed(readl_relaxed(from_io + i), to_io + i);
		dmb(ishld);

		iounmap(from_io);
	}

out:
	iounmap(to_io);
}

void __tegra_clear_framebuffer(struct platform_device *pdev,
			       unsigned long to, unsigned long size)
{
	void __iomem *to_io;
	unsigned long i;

	BUG_ON(PAGE_ALIGN((unsigned long)to) != (unsigned long)to);
	BUG_ON(PAGE_ALIGN(size) != size);

	to_io = ioremap_wc(to, size);
	if (!to_io) {
		pr_err("%s: Failed to map target framebuffer\n", __func__);
		return;
	}

	if (pfn_valid(page_to_pfn(phys_to_page(to)))) {
		for (i = 0 ; i < size; i += PAGE_SIZE)
			memset(to_io + i, 0, PAGE_SIZE);
	} else {
		for (i = 0; i < size; i += 4)
			writel_relaxed(0, to_io + i);
		dmb(ishld);
	}

	iounmap(to_io);
}

static int __init display_tegra_dts_info(void)
{
	int ret_d;
	int ret_t;
	unsigned long dt_root;
	const char *dts_fname;
	const char *dtb_bdate;
	const char *dtb_btime;

	dt_root = of_get_flat_dt_root();

	dts_fname = of_get_flat_dt_prop(dt_root, "nvidia,dtsfilename", NULL);
	if (dts_fname)
		pr_info("DTS File Name: %s\n", dts_fname);
	else
		pr_info("DTS File Name: <unknown>\n");

	ret_d = of_property_read_string_index(of_find_node_by_path("/"),
			"nvidia,dtbbuildtime", 0, &dtb_bdate);
	ret_t = of_property_read_string_index(of_find_node_by_path("/"),
			"nvidia,dtbbuildtime", 1, &dtb_btime);
	if (!ret_d && !ret_t)
		pr_info("DTB Build time: %s %s\n", dtb_bdate, dtb_btime);
	else
		pr_info("DTB Build time: <unknown>\n");

	return 0;
}
early_initcall(display_tegra_dts_info);

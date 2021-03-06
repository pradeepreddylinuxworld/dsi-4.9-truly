/*
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Eddie Dong <eddie.dong@intel.com>
 *    Jike Song <jike.song@intel.com>
 *
 * Contributors:
 *    Zhi Wang <zhi.a.wang@intel.com>
 *    Min He <min.he@intel.com>
 *    Bing Niu <bing.niu@intel.com>
 *
 */

#include "i915_drv.h"
#include "gvt.h"

enum {
	INTEL_GVT_PCI_BAR_GTTMMIO = 0,
	INTEL_GVT_PCI_BAR_APERTURE,
	INTEL_GVT_PCI_BAR_PIO,
	INTEL_GVT_PCI_BAR_MAX,
};

/**
 * intel_vgpu_emulate_cfg_read - emulate vGPU configuration space read
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_emulate_cfg_read(void *__vgpu, unsigned int offset,
	void *p_data, unsigned int bytes)
{
	struct intel_vgpu *vgpu = __vgpu;

	if (WARN_ON(bytes > 4))
		return -EINVAL;

	if (WARN_ON(offset + bytes > INTEL_GVT_MAX_CFG_SPACE_SZ))
		return -EINVAL;

	memcpy(p_data, vgpu_cfg_space(vgpu) + offset, bytes);
	return 0;
}

static int map_aperture(struct intel_vgpu *vgpu, bool map)
{
	u64 first_gfn, first_mfn;
	u64 val;
	int ret;

	if (map == vgpu->cfg_space.bar[INTEL_GVT_PCI_BAR_APERTURE].tracked)
		return 0;

	val = vgpu_cfg_space(vgpu)[PCI_BASE_ADDRESS_2];
	if (val & PCI_BASE_ADDRESS_MEM_TYPE_64)
		val = *(u64 *)(vgpu_cfg_space(vgpu) + PCI_BASE_ADDRESS_2);
	else
		val = *(u32 *)(vgpu_cfg_space(vgpu) + PCI_BASE_ADDRESS_2);

	first_gfn = (val + vgpu_aperture_offset(vgpu)) >> PAGE_SHIFT;
	first_mfn = vgpu_aperture_pa_base(vgpu) >> PAGE_SHIFT;

	ret = intel_gvt_hypervisor_map_gfn_to_mfn(vgpu, first_gfn,
						  first_mfn,
						  vgpu_aperture_sz(vgpu)
						  >> PAGE_SHIFT, map,
						  GVT_MAP_APERTURE);
	if (ret)
		return ret;

	vgpu->cfg_space.bar[INTEL_GVT_PCI_BAR_APERTURE].tracked = map;
	return 0;
}

static int trap_gttmmio(struct intel_vgpu *vgpu, bool trap)
{
	u64 start, end;
	u64 val;
	int ret;

	if (trap == vgpu->cfg_space.bar[INTEL_GVT_PCI_BAR_GTTMMIO].tracked)
		return 0;

	val = vgpu_cfg_space(vgpu)[PCI_BASE_ADDRESS_0];
	if (val & PCI_BASE_ADDRESS_MEM_TYPE_64)
		start = *(u64 *)(vgpu_cfg_space(vgpu) + PCI_BASE_ADDRESS_0);
	else
		start = *(u32 *)(vgpu_cfg_space(vgpu) + PCI_BASE_ADDRESS_0);

	start &= ~GENMASK(3, 0);
	end = start + vgpu->cfg_space.bar[INTEL_GVT_PCI_BAR_GTTMMIO].size - 1;

	ret = intel_gvt_hypervisor_set_trap_area(vgpu, start, end, trap);
	if (ret)
		return ret;

	vgpu->cfg_space.bar[INTEL_GVT_PCI_BAR_GTTMMIO].tracked = trap;
	return 0;
}

static int emulate_pci_command_write(struct intel_vgpu *vgpu,
	unsigned int offset, void *p_data, unsigned int bytes)
{
	u8 old = vgpu_cfg_space(vgpu)[offset];
	u8 new = *(u8 *)p_data;
	u8 changed = old ^ new;
	int ret;

	if (!(changed & PCI_COMMAND_MEMORY))
		return 0;

	if (old & PCI_COMMAND_MEMORY) {
		ret = trap_gttmmio(vgpu, false);
		if (ret)
			return ret;
		ret = map_aperture(vgpu, false);
		if (ret)
			return ret;
	} else {
		ret = trap_gttmmio(vgpu, true);
		if (ret)
			return ret;
		ret = map_aperture(vgpu, true);
		if (ret)
			return ret;
	}

	memcpy(vgpu_cfg_space(vgpu) + offset, p_data, bytes);
	return 0;
}

static int emulate_pci_bar_write(struct intel_vgpu *vgpu, unsigned int offset,
	void *p_data, unsigned int bytes)
{
	unsigned int bar_index =
		(rounddown(offset, 8) % PCI_BASE_ADDRESS_0) / 8;
	u32 new = *(u32 *)(p_data);
	bool lo = IS_ALIGNED(offset, 8);
	u64 size;
	int ret = 0;
	bool mmio_enabled =
		vgpu_cfg_space(vgpu)[PCI_COMMAND] & PCI_COMMAND_MEMORY;

	if (WARN_ON(bar_index >= INTEL_GVT_PCI_BAR_MAX))
		return -EINVAL;

	if (new == 0xffffffff) {
		/*
		 * Power-up software can determine how much address
		 * space the device requires by writing a value of
		 * all 1's to the register and then reading the value
		 * back. The device will return 0's in all don't-care
		 * address bits.
		 */
		size = vgpu->cfg_space.bar[bar_index].size;
		if (lo) {
			new = rounddown(new, size);
		} else {
			u32 val = vgpu_cfg_space(vgpu)[rounddown(offset, 8)];
			/* for 32bit mode bar it returns all-0 in upper 32
			 * bit, for 64bit mode bar it will calculate the
			 * size with lower 32bit and return the corresponding
			 * value
			 */
			if (val & PCI_BASE_ADDRESS_MEM_TYPE_64)
				new &= (~(size-1)) >> 32;
			else
				new = 0;
		}
		/*
		 * Unmapp & untrap the BAR, since guest hasn't configured a
		 * valid GPA
		 */
		switch (bar_index) {
		case INTEL_GVT_PCI_BAR_GTTMMIO:
			ret = trap_gttmmio(vgpu, false);
			break;
		case INTEL_GVT_PCI_BAR_APERTURE:
			ret = map_aperture(vgpu, false);
			break;
		}
		intel_vgpu_write_pci_bar(vgpu, offset, new, lo);
	} else {
		/*
		 * Unmapp & untrap the old BAR first, since guest has
		 * re-configured the BAR
		 */
		switch (bar_index) {
		case INTEL_GVT_PCI_BAR_GTTMMIO:
			ret = trap_gttmmio(vgpu, false);
			break;
		case INTEL_GVT_PCI_BAR_APERTURE:
			ret = map_aperture(vgpu, false);
			break;
		}
		intel_vgpu_write_pci_bar(vgpu, offset, new, lo);
		/* Track the new BAR */
		if (mmio_enabled) {
			switch (bar_index) {
			case INTEL_GVT_PCI_BAR_GTTMMIO:
				ret = trap_gttmmio(vgpu, true);
				break;
			case INTEL_GVT_PCI_BAR_APERTURE:
				ret = map_aperture(vgpu, true);
				break;
			}
		}
	}
	return ret;
}

/**
 * intel_vgpu_emulate_cfg_read - emulate vGPU configuration space write
 *
 * Returns:
 * Zero on success, negative error code if failed.
 */
int intel_vgpu_emulate_cfg_write(void *__vgpu, unsigned int offset,
	void *p_data, unsigned int bytes)
{
	struct intel_vgpu *vgpu = __vgpu;
	int ret;

	if (WARN_ON(bytes > 4))
		return -EINVAL;

	if (WARN_ON(offset + bytes >= INTEL_GVT_MAX_CFG_SPACE_SZ))
		return -EINVAL;

	/* First check if it's PCI_COMMAND */
	if (IS_ALIGNED(offset, 2) && offset == PCI_COMMAND) {
		if (WARN_ON(bytes > 2))
			return -EINVAL;
		return emulate_pci_command_write(vgpu, offset, p_data, bytes);
	}

	switch (rounddown(offset, 4)) {
	case PCI_BASE_ADDRESS_0:
	case PCI_BASE_ADDRESS_1:
	case PCI_BASE_ADDRESS_2:
	case PCI_BASE_ADDRESS_3:
		if (WARN_ON(!IS_ALIGNED(offset, 4)))
			return -EINVAL;
		return emulate_pci_bar_write(vgpu, offset, p_data, bytes);

	case INTEL_GVT_PCI_SWSCI:
		if (WARN_ON(!IS_ALIGNED(offset, 4)))
			return -EINVAL;
		ret = intel_vgpu_emulate_opregion_request(vgpu, *(u32 *)p_data);
		if (ret)
			return ret;
		break;

	case INTEL_GVT_PCI_OPREGION:
		if (WARN_ON(!IS_ALIGNED(offset, 4)))
			return -EINVAL;
		ret = intel_vgpu_init_opregion(vgpu, *(u32 *)p_data);
		if (ret)
			return ret;

		memcpy(vgpu_cfg_space(vgpu) + offset, p_data, bytes);
		break;
	default:
		memcpy(vgpu_cfg_space(vgpu) + offset, p_data, bytes);
		break;
	}
	return 0;
}

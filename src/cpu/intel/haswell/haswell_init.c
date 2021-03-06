/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2007-2009 coresystems GmbH
 * Copyright (C) 2011 The ChromiumOS Authors.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of
 * the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <console/console.h>
#include <device/device.h>
#include <string.h>
#include <arch/acpi.h>
#include <cpu/cpu.h>
#include <cpu/x86/mtrr.h>
#include <cpu/x86/msr.h>
#include <cpu/x86/mp.h>
#include <cpu/x86/lapic.h>
#include <cpu/intel/microcode.h>
#include <cpu/intel/speedstep.h>
#include <cpu/intel/turbo.h>
#include <cpu/x86/cache.h>
#include <cpu/x86/name.h>
#include <cpu/x86/smm.h>
#include <delay.h>
#include <pc80/mc146818rtc.h>
#include <northbridge/intel/haswell/haswell.h>
#include <southbridge/intel/lynxpoint/pch.h>
#include "haswell.h"
#include "chip.h"

/* Intel suggested latency times in units of 1024ns. */
#define C_STATE_LATENCY_CONTROL_0_LIMIT 0x42
#define C_STATE_LATENCY_CONTROL_1_LIMIT 0x73
#define C_STATE_LATENCY_CONTROL_2_LIMIT 0x91
#define C_STATE_LATENCY_CONTROL_3_LIMIT 0xe4
#define C_STATE_LATENCY_CONTROL_4_LIMIT 0x145
#define C_STATE_LATENCY_CONTROL_5_LIMIT 0x1ef

#define C_STATE_LATENCY_MICRO_SECONDS(limit, base) \
	(((1 << ((base)*5)) * (limit)) / 1000)
#define C_STATE_LATENCY_FROM_LAT_REG(reg) \
	C_STATE_LATENCY_MICRO_SECONDS(C_STATE_LATENCY_CONTROL_ ##reg## _LIMIT, \
	                              (IRTL_1024_NS >> 10))

/*
 * List of supported C-states in this processor. Only the ULT parts support C8,
 * C9, and C10.
 */
enum {
	C_STATE_C0,             /* 0 */
	C_STATE_C1,             /* 1 */
	C_STATE_C1E,            /* 2 */
	C_STATE_C3,             /* 3 */
	C_STATE_C6_SHORT_LAT,   /* 4 */
	C_STATE_C6_LONG_LAT,    /* 5 */
	C_STATE_C7_SHORT_LAT,   /* 6 */
	C_STATE_C7_LONG_LAT,    /* 7 */
	C_STATE_C7S_SHORT_LAT,  /* 8 */
	C_STATE_C7S_LONG_LAT,   /* 9 */
	C_STATE_C8,             /* 10 */
	C_STATE_C9,             /* 11 */
	C_STATE_C10,            /* 12 */
	NUM_C_STATES
};

#define MWAIT_RES(state, sub_state)                         \
	{                                                   \
		.addrl = (((state) << 4) | (sub_state)),    \
		.space_id = ACPI_ADDRESS_SPACE_FIXED,       \
		.bit_width = ACPI_FFIXEDHW_VENDOR_INTEL,    \
		.bit_offset = ACPI_FFIXEDHW_CLASS_MWAIT,    \
		.access_size = ACPI_FFIXEDHW_FLAG_HW_COORD, \
	}

static acpi_cstate_t cstate_map[NUM_C_STATES] = {
	[C_STATE_C0] = { },
	[C_STATE_C1] = {
		.latency = 0,
		.power = 1000,
		.resource = MWAIT_RES(0,0),
	},
	[C_STATE_C1E] = {
		.latency = 0,
		.power = 1000,
		.resource = MWAIT_RES(0,1),
	},
	[C_STATE_C3] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(0),
		.power = 900,
		.resource = MWAIT_RES(1, 0),
	},
	[C_STATE_C6_SHORT_LAT] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(1),
		.power = 800,
		.resource = MWAIT_RES(2, 0),
	},
	[C_STATE_C6_LONG_LAT] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(2),
		.power = 800,
		.resource = MWAIT_RES(2, 1),
	},
	[C_STATE_C7_SHORT_LAT] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(1),
		.power = 700,
		.resource = MWAIT_RES(3, 0),
	},
	[C_STATE_C7_LONG_LAT] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(2),
		.power = 700,
		.resource = MWAIT_RES(3, 1),
	},
	[C_STATE_C7S_SHORT_LAT] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(1),
		.power = 700,
		.resource = MWAIT_RES(3, 2),
	},
	[C_STATE_C7S_LONG_LAT] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(2),
		.power = 700,
		.resource = MWAIT_RES(3, 3),
	},
	[C_STATE_C8] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(3),
		.power = 600,
		.resource = MWAIT_RES(4, 0),
	},
	[C_STATE_C9] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(4),
		.power = 500,
		.resource = MWAIT_RES(5, 0),
	},
	[C_STATE_C10] = {
		.latency = C_STATE_LATENCY_FROM_LAT_REG(5),
		.power = 400,
		.resource = MWAIT_RES(6, 0),
	},
};

static void enable_vmx(void)
{
	struct cpuid_result regs;
	msr_t msr;
	int enable = IS_ENABLED(CONFIG_ENABLE_VMX);

	regs = cpuid(1);
	/* Check that the VMX is supported before reading or writing the MSR. */
	if (!((regs.ecx & CPUID_VMX) || (regs.ecx & CPUID_SMX)))
		return;

	msr = rdmsr(IA32_FEATURE_CONTROL);

	if (msr.lo & (1 << 0)) {
		printk(BIOS_ERR, "VMX is locked, so %s will do nothing\n", __func__);
		/* VMX locked. If we set it again we get an illegal
		 * instruction
		 */
		return;
	}

	/* The IA32_FEATURE_CONTROL MSR may initialize with random values.
	 * It must be cleared regardless of VMX config setting.
	 */
	msr.hi = msr.lo = 0;

	printk(BIOS_DEBUG, "%s VMX\n", enable ? "Enabling" : "Disabling");

	if (enable) {
		msr.lo |= (1 << 2);
		if (regs.ecx & CPUID_SMX)
			msr.lo |= (1 << 1);
	}

	wrmsr(IA32_FEATURE_CONTROL, msr);

	msr.lo |= (1 << 0); /* Set lock bit */

	wrmsr(IA32_FEATURE_CONTROL, msr);
}

/* Convert time in seconds to POWER_LIMIT_1_TIME MSR value */
static const u8 power_limit_time_sec_to_msr[] = {
	[0]   = 0x00,
	[1]   = 0x0a,
	[2]   = 0x0b,
	[3]   = 0x4b,
	[4]   = 0x0c,
	[5]   = 0x2c,
	[6]   = 0x4c,
	[7]   = 0x6c,
	[8]   = 0x0d,
	[10]  = 0x2d,
	[12]  = 0x4d,
	[14]  = 0x6d,
	[16]  = 0x0e,
	[20]  = 0x2e,
	[24]  = 0x4e,
	[28]  = 0x6e,
	[32]  = 0x0f,
	[40]  = 0x2f,
	[48]  = 0x4f,
	[56]  = 0x6f,
	[64]  = 0x10,
	[80]  = 0x30,
	[96]  = 0x50,
	[112] = 0x70,
	[128] = 0x11,
};

/* Convert POWER_LIMIT_1_TIME MSR value to seconds */
static const u8 power_limit_time_msr_to_sec[] = {
	[0x00] = 0,
	[0x0a] = 1,
	[0x0b] = 2,
	[0x4b] = 3,
	[0x0c] = 4,
	[0x2c] = 5,
	[0x4c] = 6,
	[0x6c] = 7,
	[0x0d] = 8,
	[0x2d] = 10,
	[0x4d] = 12,
	[0x6d] = 14,
	[0x0e] = 16,
	[0x2e] = 20,
	[0x4e] = 24,
	[0x6e] = 28,
	[0x0f] = 32,
	[0x2f] = 40,
	[0x4f] = 48,
	[0x6f] = 56,
	[0x10] = 64,
	[0x30] = 80,
	[0x50] = 96,
	[0x70] = 112,
	[0x11] = 128,
};

int haswell_family_model(void)
{
	return cpuid_eax(1) & 0x0fff0ff0;
}

int haswell_stepping(void)
{
	return cpuid_eax(1) & 0xf;
}

/* Dynamically determine if the part is ULT. */
int haswell_is_ult(void)
{
	static int ult = -1;

	if (ult < 0)
		ult = !!(haswell_family_model() == HASWELL_FAMILY_ULT);

	return ult;
}

/* The core 100MHz BLCK is disabled in deeper c-states. One needs to calibrate
 * the 100MHz BCLCK against the 24MHz BLCK to restore the clocks properly
 * when a core is woken up. */
static int pcode_ready(void)
{
	int wait_count;
	const int delay_step = 10;

	wait_count = 0;
	do {
		if (!(MCHBAR32(BIOS_MAILBOX_INTERFACE) & MAILBOX_RUN_BUSY))
			return 0;
		wait_count += delay_step;
		udelay(delay_step);
	} while (wait_count < 1000);

	return -1;
}

static void calibrate_24mhz_bclk(void)
{
	int err_code;

	if (pcode_ready() < 0) {
		printk(BIOS_ERR, "PCODE: mailbox timeout on wait ready.\n");
		return;
	}

	/* A non-zero value initiates the PCODE calibration. */
	MCHBAR32(BIOS_MAILBOX_DATA) = ~0;
	MCHBAR32(BIOS_MAILBOX_INTERFACE) =
		MAILBOX_RUN_BUSY | MAILBOX_BIOS_CMD_FSM_MEASURE_INTVL;

	if (pcode_ready() < 0) {
		printk(BIOS_ERR, "PCODE: mailbox timeout on completion.\n");
		return;
	}

	err_code = MCHBAR32(BIOS_MAILBOX_INTERFACE) & 0xff;

	printk(BIOS_DEBUG, "PCODE: 24MHz BLCK calibration response: %d\n",
	       err_code);

	/* Read the calibrated value. */
	MCHBAR32(BIOS_MAILBOX_INTERFACE) =
		MAILBOX_RUN_BUSY | MAILBOX_BIOS_CMD_READ_CALIBRATION;

	if (pcode_ready() < 0) {
		printk(BIOS_ERR, "PCODE: mailbox timeout on read.\n");
		return;
	}

	printk(BIOS_DEBUG, "PCODE: 24MHz BLCK calibration value: 0x%08x\n",
	       MCHBAR32(BIOS_MAILBOX_DATA));
}

static u32 pcode_mailbox_read(u32 command)
{
	if (pcode_ready() < 0) {
		printk(BIOS_ERR, "PCODE: mailbox timeout on wait ready.\n");
		return 0;
	}

	/* Send command and start transaction */
	MCHBAR32(BIOS_MAILBOX_INTERFACE) = command | MAILBOX_RUN_BUSY;

	if (pcode_ready() < 0) {
		printk(BIOS_ERR, "PCODE: mailbox timeout on completion.\n");
		return 0;
	}

	/* Read mailbox */
	return MCHBAR32(BIOS_MAILBOX_DATA);
}

static void initialize_vr_config(void)
{
	msr_t msr;

	printk(BIOS_DEBUG, "Initializing VR config.\n");

	/*  Configure VR_CURRENT_CONFIG. */
	msr = rdmsr(MSR_VR_CURRENT_CONFIG);
	/* Preserve bits 63 and 62. Bit 62 is PSI4 enable, but it is only valid
	 * on ULT systems. */
	msr.hi &= 0xc0000000;
	msr.hi |= (0x01 << (52 - 32)); /* PSI3 threshold -  1A. */
	msr.hi |= (0x05 << (42 - 32)); /* PSI2 threshold -  5A. */
	msr.hi |= (0x0f << (32 - 32)); /* PSI1 threshold - 15A. */

	if (haswell_is_ult())
		msr.hi |= (1 <<  (62 - 32)); /* Enable PSI4 */
	/* Leave the max instantaneous current limit (12:0) to default. */
	wrmsr(MSR_VR_CURRENT_CONFIG, msr);

	/*  Configure VR_MISC_CONFIG MSR. */
	msr = rdmsr(MSR_VR_MISC_CONFIG);
	/* Set the IOUT_SLOPE scalar applied to dIout in U10.1.9 format. */
	msr.hi &= ~(0x3ff << (40 - 32));
	msr.hi |= (0x200 << (40 - 32)); /* 1.0 */
	/* Set IOUT_OFFSET to 0. */
	msr.hi &= ~0xff;
	/* Set exit ramp rate to fast. */
	msr.hi |= (1 << (50 - 32));
	/* Set entry ramp rate to slow. */
	msr.hi &= ~(1 << (51 - 32));
	/* Enable decay mode on C-state entry. */
	msr.hi |= (1 << (52 - 32));
	/* Set the slow ramp rate to be fast ramp rate / 4 */
	msr.hi &= ~(0x3 << (53 - 32));
	msr.hi |= (0x01 << (53 - 32));
	/* Set MIN_VID (31:24) to allow CPU to have full control. */
	msr.lo &= ~0xff000000;
	wrmsr(MSR_VR_MISC_CONFIG, msr);

	/*  Configure VR_MISC_CONFIG2 MSR. */
	if (haswell_is_ult()) {
		msr = rdmsr(MSR_VR_MISC_CONFIG2);
		msr.lo &= ~0xffff;
		/* Allow CPU to control minimum voltage completely (15:8) and
		 * set the fast ramp voltage to 1110mV (0x6f in 10mV steps). */
		msr.lo |= 0x006f;
		wrmsr(MSR_VR_MISC_CONFIG2, msr);
	}
}

static void configure_pch_power_sharing(void)
{
	u32 pch_power, pch_power_ext, pmsync, pmsync2;
	int i;

	/* Read PCH Power levels from PCODE */
	pch_power = pcode_mailbox_read(MAILBOX_BIOS_CMD_READ_PCH_POWER);
	pch_power_ext = pcode_mailbox_read(MAILBOX_BIOS_CMD_READ_PCH_POWER_EXT);

	printk(BIOS_INFO, "PCH Power: PCODE Levels 0x%08x 0x%08x\n",
               pch_power, pch_power_ext);

	pmsync = RCBA32(PMSYNC_CONFIG);
	pmsync2 = RCBA32(PMSYNC_CONFIG2);

	/* Program PMSYNC_TPR_CONFIG PCH power limit values
	 *  pmsync[0:4]   = mailbox[0:5]
	 *  pmsync[8:12]  = mailbox[6:11]
	 *  pmsync[16:20] = mailbox[12:17]
	 */
	for (i = 0; i < 3; i++) {
		u32 level = pch_power & 0x3f;
		pch_power >>= 6;
		pmsync &= ~(0x1f << (i * 8));
		pmsync |= (level & 0x1f) << (i * 8);
	}
	RCBA32(PMSYNC_CONFIG) = pmsync;

	/* Program PMSYNC_TPR_CONFIG2 Extended PCH power limit values
	 *  pmsync2[0:4]   = mailbox[23:18]
	 *  pmsync2[8:12]  = mailbox_ext[6:11]
	 *  pmsync2[16:20] = mailbox_ext[12:17]
	 *  pmsync2[24:28] = mailbox_ext[18:22]
	 */
	pmsync2 &= ~0x1f;
	pmsync2 |= pch_power & 0x1f;

	for (i = 1; i < 4; i++) {
		u32 level = pch_power_ext & 0x3f;
		pch_power_ext >>= 6;
		pmsync2 &= ~(0x1f << (i * 8));
		pmsync2 |= (level & 0x1f) << (i * 8);
	}
	RCBA32(PMSYNC_CONFIG2) = pmsync2;
}

int cpu_config_tdp_levels(void)
{
	msr_t platform_info;

	/* Bits 34:33 indicate how many levels supported */
	platform_info = rdmsr(MSR_PLATFORM_INFO);
	return (platform_info.hi >> 1) & 3;
}

/*
 * Configure processor power limits if possible
 * This must be done AFTER set of BIOS_RESET_CPL
 */
void set_power_limits(u8 power_limit_1_time)
{
	msr_t msr = rdmsr(MSR_PLATFORM_INFO);
	msr_t limit;
	unsigned power_unit;
	unsigned tdp, min_power, max_power, max_time;
	u8 power_limit_1_val;

	if (power_limit_1_time > ARRAY_SIZE(power_limit_time_sec_to_msr))
		power_limit_1_time = 28;

	if (!(msr.lo & PLATFORM_INFO_SET_TDP))
		return;

	/* Get units */
	msr = rdmsr(MSR_PKG_POWER_SKU_UNIT);
	power_unit = 2 << ((msr.lo & 0xf) - 1);

	/* Get power defaults for this SKU */
	msr = rdmsr(MSR_PKG_POWER_SKU);
	tdp = msr.lo & 0x7fff;
	min_power = (msr.lo >> 16) & 0x7fff;
	max_power = msr.hi & 0x7fff;
	max_time = (msr.hi >> 16) & 0x7f;

	printk(BIOS_DEBUG, "CPU TDP: %u Watts\n", tdp / power_unit);

	if (power_limit_time_msr_to_sec[max_time] > power_limit_1_time)
		power_limit_1_time = power_limit_time_msr_to_sec[max_time];

	if (min_power > 0 && tdp < min_power)
		tdp = min_power;

	if (max_power > 0 && tdp > max_power)
		tdp = max_power;

	power_limit_1_val = power_limit_time_sec_to_msr[power_limit_1_time];

	/* Set long term power limit to TDP */
	limit.lo = 0;
	limit.lo |= tdp & PKG_POWER_LIMIT_MASK;
	limit.lo |= PKG_POWER_LIMIT_EN;
	limit.lo |= (power_limit_1_val & PKG_POWER_LIMIT_TIME_MASK) <<
		PKG_POWER_LIMIT_TIME_SHIFT;

	/* Set short term power limit to 1.25 * TDP */
	limit.hi = 0;
	limit.hi |= ((tdp * 125) / 100) & PKG_POWER_LIMIT_MASK;
	limit.hi |= PKG_POWER_LIMIT_EN;
	/* Power limit 2 time is only programmable on server SKU */

	wrmsr(MSR_PKG_POWER_LIMIT, limit);

	/* Set power limit values in MCHBAR as well */
	MCHBAR32(MCH_PKG_POWER_LIMIT_LO) = limit.lo;
	MCHBAR32(MCH_PKG_POWER_LIMIT_HI) = limit.hi;

	/* Set DDR RAPL power limit by copying from MMIO to MSR */
	msr.lo = MCHBAR32(MCH_DDR_POWER_LIMIT_LO);
	msr.hi = MCHBAR32(MCH_DDR_POWER_LIMIT_HI);
	wrmsr(MSR_DDR_RAPL_LIMIT, msr);

	/* Use nominal TDP values for CPUs with configurable TDP */
	if (cpu_config_tdp_levels()) {
		msr = rdmsr(MSR_CONFIG_TDP_NOMINAL);
		limit.hi = 0;
		limit.lo = msr.lo & 0xff;
		wrmsr(MSR_TURBO_ACTIVATION_RATIO, limit);
	}
}

static void configure_c_states(void)
{
	msr_t msr;

	msr = rdmsr(MSR_PMG_CST_CONFIG_CONTROL);
	msr.lo |= (1 << 30);	// Package c-state Undemotion Enable
	msr.lo |= (1 << 29);	// Package c-state Demotion Enable
	msr.lo |= (1 << 28);	// C1 Auto Undemotion Enable
	msr.lo |= (1 << 27);	// C3 Auto Undemotion Enable
	msr.lo |= (1 << 26);	// C1 Auto Demotion Enable
	msr.lo |= (1 << 25);	// C3 Auto Demotion Enable
	msr.lo &= ~(1 << 10);	// Disable IO MWAIT redirection
	/* The deepest package c-state defaults to factory-configured value. */
	wrmsr(MSR_PMG_CST_CONFIG_CONTROL, msr);

	msr = rdmsr(MSR_PMG_IO_CAPTURE_BASE);
	msr.lo &= ~0xffff;
	msr.lo |= (get_pmbase() + 0x14);	// LVL_2 base address
	/* The deepest package c-state defaults to factory-configured value. */
	wrmsr(MSR_PMG_IO_CAPTURE_BASE, msr);

	msr = rdmsr(MSR_MISC_PWR_MGMT);
	msr.lo &= ~(1 << 0);	// Enable P-state HW_ALL coordination
	wrmsr(MSR_MISC_PWR_MGMT, msr);

	msr = rdmsr(MSR_POWER_CTL);
	msr.lo |= (1 << 18);	// Enable Energy Perf Bias MSR 0x1b0
	msr.lo |= (1 << 1);	// C1E Enable
	msr.lo |= (1 << 0);	// Bi-directional PROCHOT#
	wrmsr(MSR_POWER_CTL, msr);

	/* C-state Interrupt Response Latency Control 0 - package C3 latency */
	msr.hi = 0;
	msr.lo = IRTL_VALID | IRTL_1024_NS | C_STATE_LATENCY_CONTROL_0_LIMIT;
	wrmsr(MSR_C_STATE_LATENCY_CONTROL_0, msr);

	/* C-state Interrupt Response Latency Control 1 */
	msr.hi = 0;
	msr.lo = IRTL_VALID | IRTL_1024_NS | C_STATE_LATENCY_CONTROL_1_LIMIT;
	wrmsr(MSR_C_STATE_LATENCY_CONTROL_1, msr);

	/* C-state Interrupt Response Latency Control 2 - package C6/C7 short */
	msr.hi = 0;
	msr.lo = IRTL_VALID | IRTL_1024_NS | C_STATE_LATENCY_CONTROL_2_LIMIT;
	wrmsr(MSR_C_STATE_LATENCY_CONTROL_2, msr);

	/* Haswell ULT only supoprts the 3-5 latency response registers.*/
	if (haswell_is_ult()) {
		/* C-state Interrupt Response Latency Control 3 - package C8 */
		msr.hi = 0;
		msr.lo = IRTL_VALID | IRTL_1024_NS |
		         C_STATE_LATENCY_CONTROL_3_LIMIT;
		wrmsr(MSR_C_STATE_LATENCY_CONTROL_3, msr);

		/* C-state Interrupt Response Latency Control 4 - package C9 */
		msr.hi = 0;
		msr.lo = IRTL_VALID | IRTL_1024_NS |
		         C_STATE_LATENCY_CONTROL_4_LIMIT;
		wrmsr(MSR_C_STATE_LATENCY_CONTROL_4, msr);

		/* C-state Interrupt Response Latency Control 5 - package C10 */
		msr.hi = 0;
		msr.lo = IRTL_VALID | IRTL_1024_NS |
		         C_STATE_LATENCY_CONTROL_5_LIMIT;
		wrmsr(MSR_C_STATE_LATENCY_CONTROL_5, msr);
	}
}

static void configure_thermal_target(void)
{
	struct cpu_intel_haswell_config *conf;
	device_t lapic;
	msr_t msr;

	/* Find pointer to CPU configuration */
	lapic = dev_find_lapic(SPEEDSTEP_APIC_MAGIC);
	if (!lapic || !lapic->chip_info)
		return;
	conf = lapic->chip_info;

	/* Set TCC activation offset if supported */
	msr = rdmsr(MSR_PLATFORM_INFO);
	if ((msr.lo & (1 << 30)) && conf->tcc_offset) {
		msr = rdmsr(MSR_TEMPERATURE_TARGET);
		msr.lo &= ~(0xf << 24); /* Bits 27:24 */
		msr.lo |= (conf->tcc_offset & 0xf) << 24;
		wrmsr(MSR_TEMPERATURE_TARGET, msr);
	}
}

static void configure_misc(void)
{
	msr_t msr;

	msr = rdmsr(IA32_MISC_ENABLE);
	msr.lo |= (1 << 0);	  /* Fast String enable */
	msr.lo |= (1 << 3); 	  /* TM1/TM2/EMTTM enable */
	msr.lo |= (1 << 16);	  /* Enhanced SpeedStep Enable */
	wrmsr(IA32_MISC_ENABLE, msr);

	/* Disable Thermal interrupts */
	msr.lo = 0;
	msr.hi = 0;
	wrmsr(IA32_THERM_INTERRUPT, msr);

	/* Enable package critical interrupt only */
	msr.lo = 1 << 4;
	msr.hi = 0;
	wrmsr(IA32_PACKAGE_THERM_INTERRUPT, msr);
}

static void enable_lapic_tpr(void)
{
	msr_t msr;

	msr = rdmsr(MSR_PIC_MSG_CONTROL);
	msr.lo &= ~(1 << 10);	/* Enable APIC TPR updates */
	wrmsr(MSR_PIC_MSG_CONTROL, msr);
}

static void configure_dca_cap(void)
{
	struct cpuid_result cpuid_regs;
	msr_t msr;

	/* Check feature flag in CPUID.(EAX=1):ECX[18]==1 */
	cpuid_regs = cpuid(1);
	if (cpuid_regs.ecx & (1 << 18)) {
		msr = rdmsr(IA32_PLATFORM_DCA_CAP);
		msr.lo |= 1;
		wrmsr(IA32_PLATFORM_DCA_CAP, msr);
	}
}

static void set_max_ratio(void)
{
	msr_t msr, perf_ctl;

	perf_ctl.hi = 0;

	/* Check for configurable TDP option */
	if (cpu_config_tdp_levels()) {
		/* Set to nominal TDP ratio */
		msr = rdmsr(MSR_CONFIG_TDP_NOMINAL);
		perf_ctl.lo = (msr.lo & 0xff) << 8;
	} else {
		/* Platform Info bits 15:8 give max ratio */
		msr = rdmsr(MSR_PLATFORM_INFO);
		perf_ctl.lo = msr.lo & 0xff00;
	}
	wrmsr(IA32_PERF_CTL, perf_ctl);

	printk(BIOS_DEBUG, "haswell: frequency set to %d\n",
	       ((perf_ctl.lo >> 8) & 0xff) * HASWELL_BCLK);
}

static void set_energy_perf_bias(u8 policy)
{
	msr_t msr;
	int ecx;

	/* Determine if energy efficient policy is supported. */
	ecx = cpuid_ecx(0x6);
	if (!(ecx & (1 << 3)))
		return;

	/* Energy Policy is bits 3:0 */
	msr = rdmsr(IA32_ENERGY_PERFORMANCE_BIAS);
	msr.lo &= ~0xf;
	msr.lo |= policy & 0xf;
	wrmsr(IA32_ENERGY_PERFORMANCE_BIAS, msr);

	printk(BIOS_DEBUG, "haswell: energy policy set to %u\n",
	       policy);
}

static void configure_mca(void)
{
	msr_t msr;
	const unsigned int mcg_cap_msr = 0x179;
	int i;
	int num_banks;

	msr = rdmsr(mcg_cap_msr);
	num_banks = msr.lo & 0xff;
	msr.lo = msr.hi = 0;
	/* TODO(adurbin): This should only be done on a cold boot. Also, some
	 * of these banks are core vs package scope. For now every CPU clears
	 * every bank. */
	for (i = 0; i < num_banks; i++)
		wrmsr(IA32_MC0_STATUS + (i * 4), msr);
}

static void bsp_init_before_ap_bringup(struct bus *cpu_bus)
{
	/* Setup MTRRs based on physical address size. */
	x86_setup_fixed_mtrrs();
	x86_setup_var_mtrrs(cpuid_eax(0x80000008) & 0xff, 2);
	x86_mtrr_check();

	initialize_vr_config();

	if (haswell_is_ult()) {
		calibrate_24mhz_bclk();
		configure_pch_power_sharing();
	}
}

/* All CPUs including BSP will run the following function. */
static void haswell_init(device_t cpu)
{
	/* Clear out pending MCEs */
	configure_mca();

	/* Enable the local cpu apics */
	enable_lapic_tpr();
	setup_lapic();

	/* Enable virtualization if Kconfig option is set */
	enable_vmx();

	/* Configure C States */
	configure_c_states();

	/* Configure Enhanced SpeedStep and Thermal Sensors */
	configure_misc();

	/* Thermal throttle activation offset */
	configure_thermal_target();

	/* Enable Direct Cache Access */
	configure_dca_cap();

	/* Set energy policy */
	set_energy_perf_bias(ENERGY_POLICY_NORMAL);

	/* Set Max Ratio */
	set_max_ratio();

	/* Enable Turbo */
	enable_turbo();
}

/* MP initialization support. */
static const void *microcode_patch;
int ht_disabled;

static int adjust_apic_id_ht_disabled(int index, int apic_id)
{
	return 2 * index;
}

static void relocate_and_load_microcode(void *unused)
{
	/* Relocate the SMM handler. */
	smm_relocate();

	/* After SMM relocation a 2nd microcode load is required. */
	intel_microcode_load_unlocked(microcode_patch);
}

static void enable_smis(void *unused)
{
	/* Now that all APs have been relocated as well as the BSP let SMIs
	 * start flowing. */
	southbridge_smm_enable_smi();

	/* Lock down the SMRAM space. */
	smm_lock();
}

static struct mp_flight_record mp_steps[] = {
	MP_FR_NOBLOCK_APS(relocate_and_load_microcode, NULL,
	                  relocate_and_load_microcode, NULL),
	MP_FR_BLOCK_APS(mp_initialize_cpu, NULL, mp_initialize_cpu, NULL),
	/* Wait for APs to finish initialization before proceeding. */
	MP_FR_BLOCK_APS(NULL, NULL, enable_smis, NULL),
};

void bsp_init_and_start_aps(struct bus *cpu_bus)
{
	void *smm_save_area;
	int num_threads;
	int num_cores;
	msr_t msr;
	struct mp_params mp_params;

	msr = rdmsr(CORE_THREAD_COUNT_MSR);
	num_threads = (msr.lo >> 0) & 0xffff;
	num_cores = (msr.lo >> 16) & 0xffff;
	printk(BIOS_DEBUG, "CPU has %u cores, %u threads enabled.\n",
	       num_cores, num_threads);

	ht_disabled = num_threads == num_cores;

	/* Perform any necessary BSP initialization before APs are brought up.
	 * This call also allows the BSP to prepare for any secondary effects
	 * from calling cpu_initialize() such as smm_init(). */
	bsp_init_before_ap_bringup(cpu_bus);

	microcode_patch = intel_microcode_find();

	/* Save default SMM area before relocation occurs. */
	smm_save_area = backup_default_smm_area();

	mp_params.num_cpus = num_threads;
	mp_params.parallel_microcode_load = 1;
	if (ht_disabled)
		mp_params.adjust_apic_id = adjust_apic_id_ht_disabled;
	else
		mp_params.adjust_apic_id = NULL;
	mp_params.flight_plan = &mp_steps[0];
	mp_params.num_records = ARRAY_SIZE(mp_steps);
	mp_params.microcode_pointer = microcode_patch;

	/* Load relocation and permeanent handlers. Then initiate relocation. */
	if (smm_initialize())
		printk(BIOS_CRIT, "SMM Initialiazation failed...\n");

	if (mp_init(cpu_bus, &mp_params)) {
		printk(BIOS_ERR, "MP initialization failure.\n");
	}

	/* Restore the default SMM region. */
	restore_default_smm_area(smm_save_area);
}

static struct device_operations cpu_dev_ops = {
	.init     = haswell_init,
};

static struct cpu_device_id cpu_table[] = {
	{ X86_VENDOR_INTEL, 0x306c1 }, /* Intel Haswell 4+2 A0 */
	{ X86_VENDOR_INTEL, 0x306c2 }, /* Intel Haswell 4+2 B0 */
	{ X86_VENDOR_INTEL, 0x40650 }, /* Intel Haswell ULT B0 */
	{ X86_VENDOR_INTEL, 0x40651 }, /* Intel Haswell ULT B1 */
	{ 0, 0 },
};

static const struct cpu_driver driver __cpu_driver = {
	.ops      = &cpu_dev_ops,
	.id_table = cpu_table,
	.cstates  = cstate_map,
};

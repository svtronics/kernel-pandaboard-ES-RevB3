/*
 * stm_arm.c
 *
 * System Trace Module (STM) Framework Driver Implementation
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
 * Copyright (C) 2012 Linaro - http://www.linaro.org
 * Copyright (c) 2011 ARM
 *
 * I have combined TI and ARM's STM drivers.  Where I commented out code
 * from each vendor, I have marked them thus:
 *
 * for TI changes:
 *     //RMH_TI
 * for ARM changes:
 *     //RMH_ARM
 *
 * All such comments will be removed in the "final" driver.
 *
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/ioport.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/err.h>

#ifdef CONFIG_ARM_AMBA
//RMH_ARM - includes from the ARM driver
#include <linux/amba/bus.h>
#endif // CONFIG_ARM_AMBA

#include "stm_fw.h"
#include "stm_msg.h"


/*****************************************************************************
 * ARM STM Register Offsets
 *****************************************************************************/
#define STMPPER_OFFSET         0xE00 // RW	Stimulus Port Enable Register, STMSPER
#define STMPSPTER_OFFSET       0xE20 // RW	Stimulus Port Trigger Enable Register, STMSPTER
#define STMPRIVMASKR_OFFSET    0xE40 // RW	Trace Privilege Register, STMPRIVMASKR
#define STMSPSCR_OFFSET        0xE60 // RW	Stimulus Port Select Configuration Register, STMSPSCR
#define STMSPMSCR_OFFSET       0xE64 // RW	Stimulus Port Master Select Configuration Register, STMSPMSCR
#define STMSPOVERRIDER_OFFSET  0xE68 // RW	Stimulus Port Override Register, STMSPOVERRIDER
#define STMSPMOVERRIDER_OFFSET 0xE6C // RW	Stimulus Port Master Override Register, STMSPMOVERRIDER
#define STMSPTRIGCSR_OFFSET    0xE70 // RW	Stimulus Port Trigger Control and Status Register, STMSPTRIGCSR

// 0xE80-0xE9C	Primary Control and Status Registers
#define STMTCSR_OFFSET         0xE80 // RW	Trace Control and Status Register, STMTCSR
#define STMTSSTIMR_OFFSET      0xE84 // WO	Timestamp Stimulus Register, STMTSSTIMR
#define STMTSFREQR_OFFSET      0xE8C // RW	Timestamp Frequency Register, STMTSFREQR
#define STMSYNCR_OFFSET        0xE90 // RW	Synchronization Control Register, STMSYNCR
#define STMAUXCR_OFFSET        0xE94 // RW	Auxiliary Control Register, STMAUXCR

// 0xEA0-0xEAC	Identification Registers
#define STMFEAT1R_OFFSET       0xEA0 // RO	Features 1 Register, STMFEAT1R
#define STMFEAT2R_OFFSET       0xEA4 // RO	Features 2 Register, STMFEAT2R
#define STMFEAT3R_OFFSET       0xEA8 // RO	Features 3 Register, STMFEAT3R

// 0xF00-0xFFC	CoreSight Management Registers
#define STMITCTRL_OFFSET       0xF00 // RW	Integration Mode Control Register, STMITCTRL
#define STMCLAIMSET_OFFSET     0xFA0 // RW	Claim Tag Set Register, STMCLAIMSET
#define STMCLAIMCLR_OFFSET     0xFA4 // RW	Claim Tag Clear Register, STMCLAIMCLR
#define STMLAR_OFFSET          0xFB0 // WO	Lock Access Register, STMLAR
#define STMLSR_OFFSET          0xFB4 // RO	Lock Status Register, STMLSR
#define STMAUTHSTATUS_OFFSET   0xFB8 // RO	Authentication Status Register, STMAUTHSTATUS
#define STMDEVID_OFFSET        0xFC8 // RO  Device Configuration Register, STMDEVID
#define STMDEVTYPE_OFFSET      0xFCC // RO  Device Type	Device Type Register, STMDEVTYPE

// 0xFD0-0xFEC	Peripheral ID	RO  Peripheral ID Registers, STMPIDR0-7
#define STMPIDR0_OFFSET        0xFD0
#define STMPIDR1_OFFSET        0xFD4
#define STMPIDR2_OFFSET        0xFD8
#define STMPIDR3_OFFSET        0xFDC
#define STMPIDR4_OFFSET        0xFE0
#define STMPIDR5_OFFSET        0xFE4
#define STMPIDR6_OFFSET        0xFE8
#define STMPIDR7_OFFSET        0xFEC

// 0xFF0-0xFFC	Component ID	RO  Component ID Registers, STMCIDR0-3
#define STMCIDR0_OFFSET        0xFF0
#define STMCIDR1_OFFSET        0xFF4
#define STMCIDR2_OFFSET        0xFF8
#define STMCIDR3_OFFSET        0xFFC

#define STMCIDR0_VALUE         0x0D
#define STMCIDR1_VALUE         0x90
#define STMCIDR2_VALUE         0x05
#define STMCIDR3_VALUE         0xB1

/*****************************************************************************
 * Support function prototypes
 *****************************************************************************/
static int parse_ulongs_from_string(int num_longs, int max_ulong, long *data,
						   char *kbuf, char *delims);

static void build_ost_header(uint32_t protocol_id, uint32_t size,
			     uint32_t *ret_buf, int32_t *buf_size);

static int32_t stm_put_msg(void *phdr_buf, int32_t hdr_size, void *pmsg_buf,
			   uint32_t msg_size, int32_t chn, bool timestamp);

static struct chn_alloc *find_chn_object(int chn);

static bool stm_app_ownership(void);

static bool stm_test_enable(void);

static bool stm_test_clock(void);

/*****************************************************************************
 * Local definitions and statics variables
 *****************************************************************************/
#define STM_CHANNEL_TOTAL     256    /* Total number of channels. */
#define STM_CHANNEL_RESERVED   32    /* Number of reserved channels. */
#define STM_START_ALLOC		0    /* Start allocating channels with chn 0. */
#define STM_MAX_SW_MASTERS      4    /* Maximum number of sw masters active at
				      * any one time (not including masking) */
#define STM_MAX_HW_MASTERS      4    /* Maximum number of sw masters active at
				      * any one time */
#define HW_MASTER_MASK       0x80    /* All HW masters have bit 7 set */

#define STM_CONTROL_BASE

#define ATTRIBUTE_BUF_SIZE    256
static char attribute_buf[ATTRIBUTE_BUF_SIZE];

static const uint32_t stm_base_address = 0x5415a000; //RMH_TI - L4_EMU_44XX_BASE;
static const uint32_t stm_size = 0x1000; // RMH - STM device memory region size from TI TRM
static const uint32_t stm_resolution = 0x1000;
static uint32_t stm_iobase; // RMH - I'm currently maintaining both stm_iobase and stm_state.regs to make it easier to port the ARM code into the TI fw
#define STM_ADDRESS_SPACE (stm_resolution * STM_CHANNEL_TOTAL)

/* These are offsets from stm_control_base */
#define STM_SWMCTRL0_OFFSET     0x024
#define STM_SWMCTRL1_OFFSET     0x028
#define STM_SWMCTRL2_OFFSET     0x02c
#define STM_SWMCTRL3_OFFSET     0x030
#define STM_SWMCTRL4_OFFSET     0x034
#define STM_HWMCTRL_OFFSET      0x038
#define STM_LOCK_OFFSET		0xFB0
#define STM_LOCK_STATUS_OFFSET  0xFB4

/* STM_SWMCTRL0 bit definitions */
#define STM_OWNERSHIP_MASK      0xC0000000
#define STM_OWNERSHIP_AVAILABLE 0x00000000
#define STM_OWNERSHIP_CLAIMED   0x40000000
#define STM_OWNERSHIP_ENABLED   0x80000000
#define STM_OWNERSHIP_OVERRIDE  0x20000000
#define STM_CURRENT_OWNER_MASK  0x10000000
#define STM_CURRENT_OWNER_APP   0x10000000
#define STM_MODULE_ENABLE       0x80010000
#define STM_MODULE_DISABLE      0x00000000
/* STM Lock Access Register value */
#define STM_MODULE_UNLOCK       0xC5ACCE55
/* STM Lock Status bits */
#define STM_LOCK_STATUS_MASK    0x2
#define STM_LOCK_STATUS_LOCKED  0x2
#define STM_LOCK_IMPLEMENTED    0x1

/* STM element size */
enum {
	STM_BYTE_SIZE =  BIT(0), /* Byte element size */
	STM_SHORT_SIZE = BIT(1), /* Short element size */
	STM_WORD_SIZE =  BIT(2)	 /* Word element size */
};

enum {
	STM_BYTE1_ALIGN = 1,
	STM_SHORT_ALIGN = 2,
	STM_BYTE3_ALIGN = 3,
	STM_WORD_ALIGN  = 4
};

/* STM address types */
enum {
	STM_REGULAR = 1,
	STM_TIMESTAMP = 2
};

struct stm_device {
	struct mutex lock;
	struct list_head chn_list_head;
	int user_allocated_chn_cnt;
	DECLARE_BITMAP(chn_map, STM_CHANNEL_TOTAL);
	DECLARE_BITMAP(sw_master_map, STM_MAX_SW_MASTERS);
	DECLARE_BITMAP(hw_master_map, STM_MAX_HW_MASTERS);
};

static struct stm_device dev;

struct chn_alloc {
	u8 chn;
	int xmit_byte_count;
	enum msg_formats chn_msg_format;
	struct mutex chn_lock;
	struct list_head chn_list_link;
};

#define STM_CHNL_SHIFT       8
#define STM_CHNL_SIZE        (1u << STM_CHNL_SHIFT)

struct stm_state {
	/* Device information */
	u32 num_channels; /* Number of channels */
	//RMH_ARM u32 num_blocks; /* Number of blocks must be ^2 */
	//RMH_ARM struct block_pool_state bp_obj;
	//RMH_ARM struct chnl_pool_state cp_obj;
	void *base_addr;
	//RMH_ARM struct dentry *stm_root;
	//RMH_ARM struct list_head debugfs_nodes;
	//RMH_ARM spinlock_t debugfs_lock;
	void *regs;
} stm_state;


/*****************************************************************************
 * Public Functions registered with the stm_fw driver
 *****************************************************************************/

/* Provide information on what is supported by the driver,
 * what is not supported is optional.
 */
int get_global_info(struct stm_global_info *info)
{
	int count;
	int ret = 0;

	/* Using dev.lock to protect attribute_buf and
	 * dev.user_allocated_chn_cnt
	 */
	if (mutex_lock_interruptible(&dev.lock))
		return -ERESTARTSYS;

	info->total_chn_cnt = STM_CHANNEL_TOTAL;
	info->reserved_chn_cnt = STM_CHANNEL_RESERVED;
	info->user_allocated_chn_cnt = dev.user_allocated_chn_cnt;

	/* If enable can be read then the module is powered and clocked */
	info->stm_module_enabled = stm_test_enable();
	if (info->stm_module_enabled)
		info->stm_module_clock_on = 1;
	else
		info->stm_module_clock_on = stm_test_clock();

	info->attribute_list = attribute_buf;

	count = snprintf(attribute_buf, ATTRIBUTE_BUF_SIZE,
					    "printk_logging not supported\n,");
	count += snprintf(attribute_buf + count, ATTRIBUTE_BUF_SIZE - count,
				"traceppoint_ftrace logging not supported\n,");
	count += snprintf(attribute_buf + count, ATTRIBUTE_BUF_SIZE - count,
				    "lttng_kernel transport not supported\n,");
	count += snprintf(attribute_buf + count, ATTRIBUTE_BUF_SIZE - count,
					       "Port routing not supported\n");

	if (count > ATTRIBUTE_BUF_SIZE)
		ret = -ENOBUFS;

	mutex_unlock(&dev.lock);

	return ret;
}

/* Write buf to STM port */
int stm_write(int chn, char *buf, int cnt, bool uspace, bool timestamp)
{

	/*
	 * TODO:: If cnt > STM_MAX_XFER (4096) then only transfer a partial
	 * message without a timestamp. Only time stamp message when count is
	 * completly transfered. May need to change stm_write API to include
	 * position to determine first (OST), middle or last(timestamp).
	 */
	int ret = 0;
	const char *local_buf = NULL;
	struct chn_alloc *chn_obj = find_chn_object(chn);
	int hdr_size = 0;
	uint32_t ost_header[2] = { 0 };

	if (chn_obj < 0)
		return (int)chn_obj;

	if (!buf || cnt < 1 || !chn_obj) {
		ret = -EINVAL;
		goto stm_write_exit;
	}

	/*
	 * Provide protection for chn_obj accesses and keeps all
	 * writes to the same STM channel in order regardless of mode.
	 */
	if (mutex_lock_interruptible(&chn_obj->chn_lock))
		return -ERESTARTSYS;

	switch (chn_obj->chn_msg_format) {

	case VENDOR:
		if (uspace) {
			if (cnt > STM_MAX_XFER_BYTES) {
				cnt = STM_MAX_XFER_BYTES;
				timestamp = false;
			}
			local_buf = kmalloc(cnt, GFP_KERNEL);
			if (!local_buf) {
				ret = -ENOMEM;
				goto stm_write_exit;
			}

			if (copy_from_user((void *)local_buf, buf, cnt)) {
				ret = -EFAULT;
				goto stm_write_exit;
			}
		}

		/* Send message over STM channel */
		build_ost_header((uint32_t)OST_PROTOCOL_BYTESTREAM, cnt,
					    (uint32_t *)ost_header, &hdr_size);
		ret = stm_put_msg((void *)ost_header, hdr_size,
				       (void *)local_buf, cnt, chn, timestamp);
		if (!ret)
			ret = cnt;

		break;

	case USER:
		/*
		 * No need to worry about exceeding STM_MAX_XFER_BYTES because
		 * stm_fw driver takes care of this check.
		 */
		ret = stm_put_msg(NULL, 0, (void *)buf, cnt, chn, timestamp);
		if (!ret)
			ret = cnt;

		break;

	default:
		ret = -EINVAL;
		goto stm_write_exit;
	}

	if (ret > 0)
		chn_obj->xmit_byte_count += ret;

stm_write_exit:

	mutex_unlock(&chn_obj->chn_lock);

	kfree(local_buf);

	return ret;
}

enum usecase_id {
	USECASE_USER,
	USECASE_PRINTK,
	USECASE_LAST
};

static char *usecase_cmds[] = {
	[USECASE_USER] = "user",
	[USECASE_PRINTK] = "printk"
};

int stm_allocate_channel(char *usecase)
{
	int chn;
	int ret = 0;
	int chn_offset;
	int chn_num;
	int chn_last;
	enum usecase_id uc_id;
	struct chn_alloc *chn_obj;

	if (mutex_lock_interruptible(&dev.lock))
		return -ERESTARTSYS;

	/* Search for the supported usecase id */
	for (uc_id = USECASE_USER; uc_id < USECASE_LAST; uc_id++)
		if (!strncmp(usecase, usecase_cmds[uc_id],
					strlen(usecase_cmds[uc_id])))
			break;

	switch (uc_id) {

	case USECASE_USER:
		chn_num = STM_CHANNEL_TOTAL - STM_CHANNEL_RESERVED;
		chn_offset = STM_START_ALLOC;
		chn_last = chn_num;
		break;

	case USECASE_PRINTK:
		chn_num = STM_CHANNEL_RESERVED;
		chn_offset = STM_CHANNEL_TOTAL - STM_CHANNEL_RESERVED;
		chn_last = STM_CHANNEL_TOTAL;
		break;

	default:
		ret = -EINVAL;
		goto stm_allocate_channel_exit;
	}

	/* Look for a free channel from channel map - note thread safe test */
	do {
		chn = find_next_zero_bit(dev.chn_map, chn_num, chn_offset);
	} while ((chn < chn_last) && test_and_set_bit(chn, dev.chn_map));

	if (chn > chn_last) {
		ret = -EPERM;
		goto stm_allocate_channel_exit;
	}

	chn_obj = kmalloc(sizeof(struct chn_alloc), GFP_KERNEL);
	if (!chn_obj) {
		ret = -ENOMEM;
		goto stm_allocate_channel_exit;
	}

	mutex_init(&chn_obj->chn_lock);
	chn_obj->chn = chn;
	chn_obj->chn_msg_format = VENDOR;
	chn_obj->xmit_byte_count = 0;

	list_add(&chn_obj->chn_list_link, &dev.chn_list_head);
	dev.user_allocated_chn_cnt++;
	ret = chn;

stm_allocate_channel_exit:

	mutex_unlock(&dev.lock);
	return ret;

}

int stm_free_channel(int chn)
{
	struct list_head *ptr;
	struct chn_alloc *chn_obj = NULL;

	if (mutex_lock_interruptible(&dev.lock))
		return -ERESTARTSYS;

	list_for_each(ptr, &dev.chn_list_head) {
		chn_obj = list_entry(ptr, struct chn_alloc, chn_list_link);
		if (!chn_obj || chn != chn_obj->chn)
			continue;

		/* Make sure nobody else is using the channel. */
		if (mutex_lock_interruptible(&chn_obj->chn_lock)) {
			mutex_unlock(&dev.lock);
			return -ERESTARTSYS;
		}
		/* Remove the channel from the link list. */
		list_del(ptr);
		/* Safe to unlock. */
		mutex_unlock(&chn_obj->chn_lock);

		clear_bit(chn, dev.chn_map);
		dev.user_allocated_chn_cnt--;
		kfree(chn_obj);
		break;
	}

	mutex_unlock(&dev.lock);

	return 0;
}

enum config_id {
	CFGID_FMT_USER,
	CFGID_FMT_VENDOR,
	CFGID_FMT_RAW,
	CFGID_ROUTE_TO_PTI,
	CFGID_ROUTE_TO_ETB,
	CFGID_SET_MASTER,
	CFGID_CLR_MASTER,
	CFGID_STM_ENABLE,
	CFGID_STM_DISABLE,
	CFGID_GUARANTEED_TIMING,
	CFGID_LAST
};

static char *config_cmds[] = {
	[CFGID_FMT_USER] = "fmt_user",
	[CFGID_FMT_VENDOR] = "fmt_vendor",
	[CFGID_FMT_RAW] = "fmt_raw",
	[CFGID_ROUTE_TO_PTI] = "route_stm_to_pti",
	[CFGID_ROUTE_TO_ETB] = "route_stm_to_etb",
	[CFGID_SET_MASTER] = "set_master",
	[CFGID_CLR_MASTER] = "clr_master",
	[CFGID_STM_ENABLE] = "stm_enable",
	[CFGID_STM_DISABLE] = "stm_disable",
	[CFGID_GUARANTEED_TIMING] = "guaranteed"
};


int stm_config_write(int chn, char *config_buf, int count)
{
	int ret = 0;
	struct chn_alloc *chn_obj;
	enum config_id cfg_id;
	void *map;
	int max;
	uint32_t master_select;
	uint32_t master_mask;
	unsigned int i;
	const int num_ulongs = 2;
	const int max_ulong = 255;
	long data[num_ulongs];
	char delims[] = " ";
	uint32_t control;

	chn_obj = (chn > -1) ? find_chn_object(chn) : NULL;
	if (chn_obj < 0)
		return (int)chn_obj;

	if (!config_buf || count < 1 || (!chn_obj && chn > -1))
		return  -EINVAL;

	if (chn_obj)
		if (mutex_lock_interruptible(&chn_obj->chn_lock))
			return -ERESTARTSYS;

	/* check buf against list of supported features */
	for (cfg_id = CFGID_FMT_USER; cfg_id < CFGID_LAST; cfg_id++)
		if (!strncmp(config_buf, config_cmds[cfg_id],
						strlen(config_cmds[cfg_id])))
			break;

	/* All cmd_id's less than CFGID_ROUTE_TO_PTI require a vaild chn_obj */
	if (!chn_obj && cfg_id < CFGID_ROUTE_TO_PTI) {
		ret = -EINVAL;
		goto stm_config_write_exit;
	}

	switch (cfg_id) {

	case CFGID_FMT_USER:
		chn_obj->chn_msg_format = USER;
		break;

	case CFGID_FMT_VENDOR:
		chn_obj->chn_msg_format = VENDOR;
		break;

	case CFGID_FMT_RAW:
		chn_obj->chn_msg_format = RAW;
		ret = -EINVAL;
		goto stm_config_write_exit;

	case CFGID_ROUTE_TO_ETB:
		/* Note: call stm_app_ownership first*/
		/* Note: need to set rate at which masters are generated
		 * for ETB.
		 */
		break;

	case CFGID_ROUTE_TO_PTI:
		/* Note: In TI case CCS ssets up the PTI port, so this is
		 * really just a  command to disable the ETB routing.
		 * TODO: Think about changing to CFGID_ROUTE_TO_NULL
		 */
		break;

	case CFGID_SET_MASTER:
		ret = parse_ulongs_from_string(num_ulongs, max_ulong,
						     data, config_buf, delims);
		if (ret < 0)
			goto stm_config_write_exit;

		/* if avaiable set ownership to application (verses debugger) */
		if (!stm_app_ownership()) {
			ret = -EACCES;
			goto stm_config_write_exit;
		}

		/* Test for an avaiable master select */
		if (data[0] & HW_MASTER_MASK) {
			map = dev.hw_master_map;
			max = STM_MAX_HW_MASTERS;
		} else {
			map = dev.sw_master_map;
			max = STM_MAX_SW_MASTERS;
		}

		do {
			i = find_next_zero_bit(map, max, 0);
		} while ((i < max) && test_and_set_bit(i, map));
		if (i == STM_MAX_SW_MASTERS) {
			ret = -EINVAL;
			goto stm_config_write_exit;
		}

		if (data[0] & HW_MASTER_MASK) {
			/* set hw master select value */
//RMH_TI			master_select = ioread32((void *)(stm_cntl_iobase +
//RMH_TI							STM_HWMCTRL_OFFSET));
			master_select = 0; //RMH_TI
			master_select |= data[0] << (i * 8);
//RMH_TI			iowrite32(master_select, (void *)(stm_cntl_iobase +
//RMH_TI							STM_HWMCTRL_OFFSET));
		} else {
			/* set sw master select value */
//RMH_TI			master_select = ioread32((void *)(stm_cntl_iobase +
//RMH_TI						       STM_SWMCTRL1_OFFSET));
			master_select = 0; //RMH_TI
			master_select |= data[0] << (i * 8);
//RMH_TI			iowrite32(master_select, (void *)(stm_cntl_iobase +
//RMH_TI						       STM_SWMCTRL1_OFFSET));
			/* set master mask value */
//RMH_TI			master_mask = ioread32((void *)stm_cntl_iobase +
//RMH_TI							STM_SWMCTRL2_OFFSET);
			master_mask = 0; //RMH_TI
			master_mask |= data[1] << (i * 8);
//RMH_TI			iowrite32(master_mask, (void *)(stm_cntl_iobase +
//RMH_TI							STM_SWMCTRL2_OFFSET));
/* TODO:: Add fixed values for other domain master selection registers. */
/*	These may not actually need to be set.			*/

		}
		break;

	case CFGID_STM_ENABLE:
		/*
		 * if avaiable set ownership to application (verses host
		 * debugger)
		 */
		if (!stm_app_ownership()) {
			ret = -EACCES;
			goto stm_config_write_exit;
		}

//RMH_TI		iowrite32(STM_MODULE_ENABLE, (void *)(stm_cntl_iobase
//RMH_TI						       + STM_SWMCTRL0_OFFSET));

//RMH_TI		control = ioread32((void *)(stm_cntl_iobase
//RMH_TI						       + STM_SWMCTRL0_OFFSET));
		control = 0; //RMH_TI

		if ((control & STM_OWNERSHIP_MASK) != STM_OWNERSHIP_ENABLED)
			return -EFAULT;

		break;

	case CFGID_STM_DISABLE:

		/*
		 * if avaiable set ownership to application (verses host
		 * debugger)
		 */
		if (!stm_app_ownership()) {
			ret = -EACCES;
			goto stm_config_write_exit;
		}

//RMH_TI		iowrite32(STM_MODULE_DISABLE, (void *)(stm_cntl_iobase
//RMH_TI						       + STM_SWMCTRL0_OFFSET));

//RMH_TI		control = ioread32((void *)(stm_cntl_iobase
//RMH_TI						       + STM_SWMCTRL0_OFFSET));
		control = 0; //RMH_TI

		if ((control & STM_OWNERSHIP_MASK) != STM_OWNERSHIP_AVAILABLE)
			return -EFAULT;

		break;

	case CFGID_GUARANTEED_TIMING:
		/* Only support to prevent error */
		break;
	default:
		/* If the config request not supported */
		 count = -EINVAL;
	}

	ret = count;

stm_config_write_exit:

	if (chn_obj)
		mutex_unlock(&chn_obj->chn_lock);

	return ret;

}

/* Provide comma seperated list of configuration and channel state info. */
int stm_config_read(int chn, char *config_string, int count)
{

	struct chn_alloc *chn_obj = find_chn_object(chn);
	int buf_cnt = 0;
	char *chn_format = NULL;

	if (IS_ERR(chn_obj))
		return PTR_ERR(chn_obj);

	if (!config_string || count < 1 || !chn_obj)
		return -EINVAL;

	if (mutex_lock_interruptible(&chn_obj->chn_lock))
		return -ERESTARTSYS;

	buf_cnt = snprintf(config_string, count, "STM channel %d\n,",
							chn_obj->chn);

	switch (chn_obj->chn_msg_format) {
	case VENDOR:
		chn_format = config_cmds[CFGID_FMT_VENDOR];
		break;
	case USER:
		chn_format = config_cmds[CFGID_FMT_USER];
		break;
	case RAW:
		chn_format = config_cmds[CFGID_FMT_RAW];
		break;
	}
	if (chn_format)
		buf_cnt += snprintf(config_string+buf_cnt, count-buf_cnt,
					   "Channel format %s\n,", chn_format);

	/* Note: If any other strings are added, this string must be terminated
	 *       with a comma.
	 */
	buf_cnt += snprintf(config_string+buf_cnt, count-buf_cnt,
			 "Transmit Byte Count %d\n", chn_obj->xmit_byte_count);

	mutex_unlock(&chn_obj->chn_lock);

	return buf_cnt;
}


static struct stm_operations stm_arm_ops = {
	.name = "stm_arm",
	.get_global_info = get_global_info,
	.write = stm_write,
	.allocate_channel = stm_allocate_channel,
	.free_channel = stm_free_channel,
	.config_write = stm_config_write,
	.config_read = stm_config_read
};


void stm_arm_clean(void)
{
	struct list_head *ptr;
	struct chn_alloc *chn_obj = NULL;

	list_for_each(ptr, &dev.chn_list_head) {
		chn_obj = list_entry(ptr, struct chn_alloc, chn_list_link);
		kfree(chn_obj);
	}

 	release_mem_region(stm_base_address, STM_ADDRESS_SPACE);
//RMH_TI	release_mem_region(stm_control_base, STM_ADDRESS_SPACE);

 	if (!stm_iobase) {
 		iounmap((void *)stm_iobase);
 		stm_iobase = 0;
	}

//RMH_TI	if (!stm_cntl_iobase) {
//RMH_TI		iounmap((void *)stm_cntl_iobase);
//RMH_TI		stm_cntl_iobase = 0;
//RMH_TI	}
}


static int stm_device_setup(uint32_t base_address, uint32_t size)
{
	int ret = 0;
	uint32_t regs[8];
	printk(KERN_INFO"RMH: STM: %d: stm_device_setup\n", __LINE__);

	stm_state.base_addr = (void *) base_address;
	stm_state.num_channels = size / STM_CHNL_SIZE;
	stm_state.regs = ioremap_nocache(base_address, size);
	stm_iobase = (uint32_t) stm_state.regs; // RMH - I'm currently maintaining both stm_iobase and stm_state.regs to make it easier to port the ARM code into the TI fw

	printk(KERN_INFO"RMH: STM: %d: stm_probe: base_addr=0x%x regs=0x%x\n", __LINE__, (uint32_t)stm_state.base_addr, (uint32_t)stm_state.regs);

	if (!stm_state.regs) {
		ret = -ENOMEM;
		printk(KERN_INFO"RMH: STM: %d: stm_probe: ioremap_nocache returned %d\n", __LINE__, ret);
	}
	else {
		// try to probe the device to see if it really is an ARM STM chip
		printk(KERN_INFO"RMH: STM: %d: stm_device_setup: probing device\n", __LINE__);

		regs[0] = ioread32((void *)stm_state.regs + STMDEVTYPE_OFFSET); // RO  Device Type	Device Type Register, STMDEVTYPE
		printk(KERN_INFO"RMH: STM: %d: DEVTYPE = 0x%x\n", __LINE__, regs[0]);

		regs[0] = ioread32((void *)stm_state.regs + STMPIDR0_OFFSET);
		regs[1] = ioread32((void *)stm_state.regs + STMPIDR1_OFFSET);
		regs[2] = ioread32((void *)stm_state.regs + STMPIDR2_OFFSET);
		regs[3] = ioread32((void *)stm_state.regs + STMPIDR3_OFFSET);
		regs[4] = ioread32((void *)stm_state.regs + STMPIDR4_OFFSET);
		regs[5] = ioread32((void *)stm_state.regs + STMPIDR5_OFFSET);
		regs[6] = ioread32((void *)stm_state.regs + STMPIDR6_OFFSET);
		regs[7] = ioread32((void *)stm_state.regs + STMPIDR7_OFFSET);
		printk(KERN_INFO"RMH: STM: %d: PID=0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n", __LINE__, regs[0],regs[1],regs[2],regs[3], regs[4],regs[5],regs[6],regs[7]);

		regs[0] = ioread32((void *)stm_state.regs + STMCIDR0_OFFSET);
		regs[1] = ioread32((void *)stm_state.regs + STMCIDR1_OFFSET);
		regs[2] = ioread32((void *)stm_state.regs + STMCIDR2_OFFSET);
		regs[3] = ioread32((void *)stm_state.regs + STMCIDR3_OFFSET);
		
		printk(KERN_INFO"RMH: STM: %d: CID=0x%x 0x%x 0x%x 0x%x\n", __LINE__, regs[0],regs[1],regs[2],regs[3]);
	}

	return ret;
}

#ifdef CONFIG_ARM_AMBA
static int stm_probe(struct amba_device *dev, struct amba_id *id)
{
	int ret = 0;
	printk(KERN_INFO"RMH: STM: %d: stm_probe\n", __LINE__);	

	ret = amba_request_regions(dev, NULL);
	if (ret){
		printk(KERN_INFO"RMH: STM: %d: stm_probe: amba_request_regions returned %d\n", __LINE__, ret);
		goto out;
	}

	stm_device_setup(dev->res.start, resource_size(dev->res.size));

#if 0 //RMH_ARM - use the framework for managing channels
	ret = stm_block_pool_create(stm_state.regs, resource_size(&dev->res));
	if (ret < 0) {
		iounmap(stm_state.regs);
		goto out;
	}

	ret = stm_channel_pool_create();
	if (ret < 0) {
		stm_block_pool_destroy();
		iounmap(stm_state.regs);
		goto out;
	}

	ret = stm_create_debugfs();
	if (ret < 0) {
		stm_channel_pool_destroy();
		stm_block_pool_destroy();
		iounmap(stm_state.regs);
		goto out;
	}
#endif // 0 - RMH

	amba_set_drvdata(dev, &stm_state);
	printk(KERN_INFO"RMH: STM: %d: stm_probe: returning zero\n", __LINE__);

	return 0;

out:
	return ret;
}


static int stm_remove(struct amba_device *dev)
{
	printk(KERN_INFO"RMH: STM: %d: stm_remove\n", __LINE__);	
	amba_set_drvdata(dev, NULL);
	iounmap(stm_state.regs);
	//RMH_ARM stm_channel_pool_destroy();
	//RMH_ARM stm_block_pool_destroy();
	//RMH_ARM stm_remove_debugfs();
	return 0;
}

static struct amba_id stm_ids[] = {
	{
		.id 	= 0x000BB962,
		.mask 	= 0x000fffff,
	},
	{ 0, 0},
};

static struct amba_driver stm_driver = {
	.drv = {
		.name = "stm",
	},
	.id_table = stm_ids,
	.probe = stm_probe,
	.remove = stm_remove,
};
#endif // CONFIG_ARM_AMBA

int __init stm_arm_init(void)
{

	int ret = 0;
	char *mod_name = "STM ARM Module";

 	if (!request_mem_region(stm_base_address, STM_ADDRESS_SPACE,
								mod_name)) {
		ret = -ENODEV;
		goto init_err;
	}

//RMH_TI 	stm_iobase = (unsigned long)ioremap_nocache(stm_base_address,
//RMH_TI							    STM_ADDRESS_SPACE);
//RMH_TI 	if (!stm_iobase) {
//RMH_TI 		ret = -ENODEV;
//RMH_TI 		goto init_err;
//RMH_TI 	}

//RMH_TI	if (!request_mem_region(stm_control_base, STM_ADDRESS_SPACE,
//RMH_TI								mod_name)) {
//RMH_TI		ret = -ENODEV;
//RMH_TI		goto init_err;
//RMH_TI	}

//RMH_TI	stm_cntl_iobase = (unsigned long)ioremap_nocache(stm_control_base,
//RMH_TI							     stm_control_size);
//RMH_TI	if (!stm_cntl_iobase) {
//RMH_TI		ret = -ENODEV;
//RMH_TI		goto init_err;
//RMH_TI	}
	printk(KERN_INFO"RMH: STM: %d: stm_arm_init\n", __LINE__);	

#ifdef CONFIG_ARM_AMBA
	ret = amba_driver_register(&stm_driver);
	if (ret){
		printk(KERN_INFO"RMH: STM device failed amba registeration\n");
		return -1;
	}
	printk(KERN_INFO"RMH: STM AMBA device registered\n");
#else
	printk(KERN_INFO"RMH: STM device init\n");
	stm_device_setup(stm_base_address, stm_size);
	printk(KERN_INFO"RMH: STM device init'd\n");
#endif // CONFIG_ARM_AMBA

	/* Register with the STM Framework Drvier */
	ret = stm_register(&stm_arm_ops);
	if (ret < 0)
		goto init_err;

	mutex_init(&dev.lock);
	INIT_LIST_HEAD(&dev.chn_list_head);
	dev.user_allocated_chn_cnt = 0;

	return ret;

init_err:
	stm_arm_clean();
	pr_err("stm_arm: driver registration error %d\n", ret);
	return ret;

}

void __exit stm_arm_exit(void)
{
	pr_info("stm_arm: driver exit\n");
	stm_arm_clean();
}

module_init(stm_arm_init);
module_exit(stm_arm_exit);
MODULE_AUTHOR("Linaro");
MODULE_DESCRIPTION("STM ARM module");
MODULE_LICENSE("Dual BSD/GPL");

/*****************************************************************************
 * Support functions
 *****************************************************************************/
static struct chn_alloc *find_chn_object(int chn)
{
	struct list_head *ptr;
	struct chn_alloc *chn_obj = NULL;
	struct chn_alloc *chn_obj_ret = NULL;

	if (mutex_lock_interruptible(&dev.lock))
		return (struct chn_alloc *)-ERESTARTSYS;

	list_for_each(ptr, &dev.chn_list_head) {
		chn_obj = list_entry(ptr, struct chn_alloc, chn_list_link);
		if (chn == chn_obj->chn) {
			chn_obj_ret = chn_obj;
			break;
		}
	}

	mutex_unlock(&dev.lock);
	return chn_obj_ret;
}

static int parse_ulongs_from_string(int num_longs, int max_ulong, long *data,
						    char *kbuf, char *delims)
{
	/* Parse the set header command */
	int i;
	int ret;
	char **item = &kbuf;
	/* Get command - throw it away */
	char *param = strsep(item, delims);

	/* Get next two parameter values */
	for (i = 0; i < num_longs; i++) {
		param = strsep(item, delims);
		if (!param)
			return -EINVAL;

		ret = kstrtol(param, 10, &data[i]);
		if (ret)
			return ret;

		if (data[i] < 0 || data[i] > max_ulong)
			return -EINVAL;
	}

	return 0;
}

static inline uint32_t compose_address(uint32_t addr_type, int32_t stm_chn)
{
	uint32_t offset;

	offset = (addr_type == STM_TIMESTAMP) ? stm_resolution / 2 : 0;

	return (uint32_t)(/*RMH stm_iobase + */
			 (stm_resolution * (uint32_t)stm_chn) +
			  offset);
}

static void build_ost_header(uint32_t protocol_id, uint32_t size,
			     uint32_t *ret_buf, int32_t *buf_size)
{
	if (size >= OST_SHORT_HEADER_LENGTH_LIMIT) {
		*ret_buf++ = (OST_VERSION | OST_ENTITY_ID | protocol_id |
			      OST_SHORT_HEADER_LENGTH_LIMIT);
		*ret_buf = size;
		*buf_size = 8;  /* 8 bytes for the extended length header */
	} else {
		*ret_buf = (OST_VERSION | OST_ENTITY_ID | protocol_id | size);
		*buf_size = 4;      /* 4 bytes for the normal header */
	}
}


static int32_t stm_put_msg(void *phdr_buf, int32_t hdr_size, void *pmsg_buf,
			   uint32_t msg_size, int32_t stm_chn, bool timestamp)
{
	return 0;
}

static bool stm_app_ownership(void)
{
	uint32_t control;
	uint32_t lock_status;
	int retry = 100;
	bool ret = true;

	control = 0; //RMH_TI ioread32((void *)stm_cntl_iobase + STM_SWMCTRL0_OFFSET);

	/* If driver already owns the STM module then exit*/
	if ((control &  STM_CURRENT_OWNER_MASK) == STM_CURRENT_OWNER_APP) {
		ret = true;
		goto stm_app_ownership_exit;
	}

	/* If drvier does not own, and it's not avaiable then exit */
	if ((control &  STM_OWNERSHIP_MASK) != STM_OWNERSHIP_AVAILABLE) {
		ret = false;
		goto stm_app_ownership_exit;
	}

	/* Since it's avaiable can attempt to claim - so unlock the module*/
//RMH_TI	iowrite32(STM_MODULE_UNLOCK, (void *)stm_cntl_iobase + STM_LOCK_OFFSET);

	/* Test that it unlocked properly */
//RMH_TI	lock_status = ioread32((void *)stm_cntl_iobase +
//RMH_TI						STM_LOCK_STATUS_OFFSET);
	lock_status = 0;

	/*
	 * If locked set or the implemented bit is not readable (in cases where
	 * the clock is not enabled) then exit, no chance at claiming STM
	 */
	if ((lock_status & STM_LOCK_IMPLEMENTED) != STM_LOCK_IMPLEMENTED ||
	    (lock_status & STM_LOCK_STATUS_MASK) == STM_LOCK_STATUS_LOCKED) {
		ret = false;
		goto stm_app_ownership_exit;
	}

	/* claim */
	control = STM_OWNERSHIP_CLAIMED;
//RMH_TI	iowrite32(control, (void *)stm_cntl_iobase + STM_SWMCTRL0_OFFSET);

	/* Test for claim and app owns */
	do {
		const uint32_t mask = STM_OWNERSHIP_MASK |
					STM_CURRENT_OWNER_MASK;
		const uint32_t claimed = STM_OWNERSHIP_CLAIMED |
					STM_CURRENT_OWNER_APP;
		const uint32_t enabled = STM_OWNERSHIP_ENABLED |
					STM_CURRENT_OWNER_APP;

//RMH_TI		control = ioread32((void *)stm_cntl_iobase +
//RMH_TI							STM_SWMCTRL0_OFFSET);
		control = 0; //RMH_TI
		if ((control & mask) == claimed || (control & mask) == enabled)
			break;

	} while (retry--);

	if (retry == -1)
		ret = false;

stm_app_ownership_exit:
	return ret;
}

static bool stm_test_enable()
{
	uint32_t control;

//RMH_TI	control = ioread32((void *)stm_cntl_iobase + STM_SWMCTRL0_OFFSET);
	control = 0; // RMH

	if ((control & STM_MODULE_ENABLE) == STM_MODULE_ENABLE)
		return true;

	return false;
}

static bool stm_test_clock()
{
	bool app_owns = false;
	uint32_t control;

//RMH_TI	control = ioread32((void *)stm_cntl_iobase + STM_SWMCTRL0_OFFSET);
	control = 0; //RMH_TI

	/* If driver already owns the STM module then it miust be
	 * powered up and clock enabled.
	 */
	if ((control &  STM_CURRENT_OWNER_MASK) == STM_CURRENT_OWNER_APP)
		return true;

	/* If drvier does not own, and ownership state is not available
	 * (claimed or enabled) then must be powered up and clock enabled.
	 */
	if ((control &  STM_OWNERSHIP_MASK) != STM_OWNERSHIP_AVAILABLE)
		return true;

	/* If ownership is avaialbe test driver (app) can own*/
	app_owns = stm_app_ownership();
	if (app_owns) {
//RMH_TI		iowrite32(STM_MODULE_DISABLE, (void *)(stm_cntl_iobase
//RMH_TI						       + STM_SWMCTRL0_OFFSET));
		return true;
	}

	return false;
}


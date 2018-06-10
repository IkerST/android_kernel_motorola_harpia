/* Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/msm_ion.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#ifdef CONFIG_MSM_TZ_LOG_WDOG_DUMP
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/ctype.h>
#include <linux/dma-contiguous.h>
#include <linux/dma-mapping.h>
#include <linux/pstore_ram.h>
#include <soc/qcom/memory_dump.h>
#include <soc/qcom/bootinfo.h>
#endif /* CONFIG_MSM_TZ_LOG_WDOG_DUMP */
#include <soc/qcom/scm.h>
#include <soc/qcom/qseecomi.h>

#define DEBUG_MAX_RW_BUF 4096

/* QSEE_LOG_BUF_SIZE = 32K */
#define QSEE_LOG_BUF_SIZE 0x8000


/* TZ Diagnostic Area legacy version number */
#define TZBSP_DIAG_MAJOR_VERSION_LEGACY	2
/*
 * Preprocessor Definitions and Constants
 */
#define TZBSP_CPU_COUNT 0x02
/*
 * Number of VMID Tables
 */
#define TZBSP_DIAG_NUM_OF_VMID 16
/*
 * VMID Description length
 */
#define TZBSP_DIAG_VMID_DESC_LEN 7
/*
 * Number of Interrupts
 */
#define TZBSP_DIAG_INT_NUM  32
/*
 * Length of descriptive name associated with Interrupt
 */
#define TZBSP_MAX_INT_DESC 16
/*
 * VMID Table
 */
struct tzdbg_vmid_t {
	uint8_t vmid; /* Virtual Machine Identifier */
	uint8_t desc[TZBSP_DIAG_VMID_DESC_LEN];	/* ASCII Text */
};
/*
 * Boot Info Table
 */
struct tzdbg_boot_info_t {
	uint32_t wb_entry_cnt;	/* Warmboot entry CPU Counter */
	uint32_t wb_exit_cnt;	/* Warmboot exit CPU Counter */
	uint32_t pc_entry_cnt;	/* Power Collapse entry CPU Counter */
	uint32_t pc_exit_cnt;	/* Power Collapse exit CPU counter */
	uint32_t warm_jmp_addr;	/* Last Warmboot Jump Address */
	uint32_t spare;	/* Reserved for future use. */
};

struct tzdbg_boot_info2_t {
	uint32_t wb_entry_cnt;	/* Warmboot entry CPU Counter */
	uint32_t wb_exit_cnt;	/* Warmboot exit CPU Counter */
	uint32_t pc_entry_cnt;	/* Power Collapse entry CPU Counter */
	uint32_t pc_exit_cnt;	/* Power Collapse exit CPU counter */
	uint64_t warm_jmp_addr;	/* Last Warmboot Jump Address */
	uint32_t warm_jmp_instr;/* Last Warmboot Jump Address Instruction */
};
/*
 * Reset Info Table
 */
struct tzdbg_reset_info_t {
	uint32_t reset_type;	/* Reset Reason */
	uint32_t reset_cnt;	/* Number of resets occured/CPU */
};
/*
 * Interrupt Info Table
 */
struct tzdbg_int_t {
	/*
	 * Type of Interrupt/exception
	 */
	uint16_t int_info;
	/*
	 * Availability of the slot
	 */
	uint8_t avail;
	/*
	 * Reserved for future use
	 */
	uint8_t spare;
	/*
	 * Interrupt # for IRQ and FIQ
	 */
	uint32_t int_num;
	/*
	 * ASCII text describing type of interrupt e.g:
	 * Secure Timer, EBI XPU. This string is always null terminated,
	 * supporting at most TZBSP_MAX_INT_DESC characters.
	 * Any additional characters are truncated.
	 */
	uint8_t int_desc[TZBSP_MAX_INT_DESC];
	uint64_t int_count[TZBSP_CPU_COUNT]; /* # of times seen per CPU */
};

/*
 * Log ring buffer position
 */
struct tzdbg_log_pos_t {
	uint16_t wrap;
	uint16_t offset;
};

 /*
 * Log ring buffer
 */
struct tzdbg_log_t {
	struct tzdbg_log_pos_t	log_pos;
	/* open ended array to the end of the 4K IMEM buffer */
	uint8_t					log_buf[];
};

/*
 * Diagnostic Table
 */
struct tzdbg_t {
	uint32_t magic_num;
	uint32_t version;
	/*
	 * Number of CPU's
	 */
	uint32_t cpu_count;
	/*
	 * Offset of VMID Table
	 */
	uint32_t vmid_info_off;
	/*
	 * Offset of Boot Table
	 */
	uint32_t boot_info_off;
	/*
	 * Offset of Reset info Table
	 */
	uint32_t reset_info_off;
	/*
	 * Offset of Interrupt info Table
	 */
	uint32_t int_info_off;
	/*
	 * Ring Buffer Offset
	 */
	uint32_t ring_off;
	/*
	 * Ring Buffer Length
	 */
	uint32_t ring_len;
	/*
	 * VMID to EE Mapping
	 */
	struct tzdbg_vmid_t vmid_info[TZBSP_DIAG_NUM_OF_VMID];
	/*
	 * Boot Info
	 */
	struct tzdbg_boot_info_t  boot_info[TZBSP_CPU_COUNT];
	/*
	 * Reset Info
	 */
	struct tzdbg_reset_info_t reset_info[TZBSP_CPU_COUNT];
	uint32_t num_interrupts;
	struct tzdbg_int_t  int_info[TZBSP_DIAG_INT_NUM];
	/*
	 * We need at least 2K for the ring buffer
	 */
	struct tzdbg_log_t ring_buffer;	/* TZ Ring Buffer */
};

/*
 * Enumeration order for VMID's
 */
enum tzdbg_stats_type {
	TZDBG_BOOT = 0,
	TZDBG_RESET,
	TZDBG_INTERRUPT,
	TZDBG_VMID,
	TZDBG_GENERAL,
	TZDBG_LOG,
	TZDBG_QSEE_LOG,
	TZDBG_STATS_MAX
};

struct tzdbg_stat {
	char *name;
	char *data;
};

struct tzdbg {
	void __iomem *virt_iobase;
	struct tzdbg_t *diag_buf;
	size_t diag_size;
	char *disp_buf;
	int debug_tz[TZDBG_STATS_MAX];
	struct tzdbg_stat stat[TZDBG_STATS_MAX];
};

static struct tzdbg tzdbg = {

	.stat[TZDBG_BOOT].name = "boot",
	.stat[TZDBG_RESET].name = "reset",
	.stat[TZDBG_INTERRUPT].name = "interrupt",
	.stat[TZDBG_VMID].name = "vmid",
	.stat[TZDBG_GENERAL].name = "general",
	.stat[TZDBG_LOG].name = "log",
	.stat[TZDBG_QSEE_LOG].name = "qsee_log",
};

static struct tzdbg_log_t *g_qsee_log;

/*
 * Debugfs data structure and functions
 */

static int _disp_tz_general_stats(void)
{
	int len = 0;

	len += snprintf(tzdbg.disp_buf + len, tzdbg.diag_size - 1,
			"   Version        : 0x%x\n"
			"   Magic Number   : 0x%x\n"
			"   Number of CPU  : %d\n",
			tzdbg.diag_buf->version,
			tzdbg.diag_buf->magic_num,
			tzdbg.diag_buf->cpu_count);
	tzdbg.stat[TZDBG_GENERAL].data = tzdbg.disp_buf;
	return len;
}

static int _disp_tz_vmid_stats(void)
{
	int i, num_vmid;
	int len = 0;
	struct tzdbg_vmid_t *ptr;

	ptr = (struct tzdbg_vmid_t *)((unsigned char *)tzdbg.diag_buf +
					tzdbg.diag_buf->vmid_info_off);
	num_vmid = ((tzdbg.diag_buf->boot_info_off -
				tzdbg.diag_buf->vmid_info_off)/
					(sizeof(struct tzdbg_vmid_t)));

	for (i = 0; i < num_vmid; i++) {
		if (ptr->vmid < 0xFF) {
			len += snprintf(tzdbg.disp_buf + len,
				(tzdbg.diag_size - 1) - len,
				"   0x%x        %s\n",
				(uint32_t)ptr->vmid, (uint8_t *)ptr->desc);
		}
		if (len > (tzdbg.diag_size - 1)) {
			pr_warn("%s: Cannot fit all info into the buffer\n",
								__func__);
			break;
		}
		ptr++;
	}

	tzdbg.stat[TZDBG_VMID].data = tzdbg.disp_buf;
	return len;
}

static int _disp_tz_boot_stats(void)
{
	int i;
	int len = 0;
	struct tzdbg_boot_info_t *ptr;
	struct tzdbg_boot_info2_t *ptr2;
	int v2 = 0;

	if ((tzdbg.diag_buf->reset_info_off - tzdbg.diag_buf->boot_info_off) ==
			(sizeof(*ptr2) * tzdbg.diag_buf->cpu_count))
		v2 = 1;
	ptr = (struct tzdbg_boot_info_t *)((unsigned char *)tzdbg.diag_buf +
					tzdbg.diag_buf->boot_info_off);
	ptr2 = (struct tzdbg_boot_info2_t *)ptr;

	for (i = 0; i < tzdbg.diag_buf->cpu_count; i++) {
		len += snprintf(tzdbg.disp_buf + len,
				(tzdbg.diag_size - 1) - len,
				"  CPU #: %d\n"
				"     Warmboot jump address     : 0x%lx\n"
				"     Warmboot entry CPU counter: 0x%x\n"
				"     Warmboot exit CPU counter : 0x%x\n"
				"     Power Collapse entry CPU counter: 0x%x\n"
				"     Power Collapse exit CPU counter : 0x%x\n",
				i, (unsigned long)(v2 ? ptr2->warm_jmp_addr :
						ptr->warm_jmp_addr),
				(v2 ? ptr2->wb_entry_cnt : ptr->wb_entry_cnt),
				(v2 ? ptr2->wb_exit_cnt : ptr->wb_exit_cnt),
				(v2 ? ptr2->pc_entry_cnt : ptr->pc_entry_cnt),
				(v2 ? ptr2->pc_exit_cnt : ptr->pc_exit_cnt));

		if (len > (tzdbg.diag_size - 1)) {
			pr_warn("%s: Cannot fit all info into the buffer\n",
								__func__);
			break;
		}
		ptr++;
		ptr2++;
	}
	tzdbg.stat[TZDBG_BOOT].data = tzdbg.disp_buf;
	return len;
}

static int _disp_tz_reset_stats(void)
{
	int i;
	int len = 0;
	struct tzdbg_reset_info_t *ptr;

	ptr = (struct tzdbg_reset_info_t *)((unsigned char *)tzdbg.diag_buf +
					tzdbg.diag_buf->reset_info_off);

	for (i = 0; i < tzdbg.diag_buf->cpu_count; i++) {
		len += snprintf(tzdbg.disp_buf + len,
				(tzdbg.diag_size - 1) - len,
				"  CPU #: %d\n"
				"     Reset Type (reason)       : 0x%x\n"
				"     Reset counter             : 0x%x\n",
				i, ptr->reset_type, ptr->reset_cnt);

		if (len > (tzdbg.diag_size - 1)) {
			pr_warn("%s: Cannot fit all info into the buffer\n",
								__func__);
			break;
		}

		ptr++;
	}
	tzdbg.stat[TZDBG_RESET].data = tzdbg.disp_buf;
	return len;
}

static int _disp_tz_interrupt_stats(void)
{
	int i, j, int_info_size;
	int len = 0;
	int *num_int;
	unsigned char *ptr;
	struct tzdbg_int_t *tzdbg_ptr;

	num_int = (uint32_t *)((unsigned char *)tzdbg.diag_buf +
			(tzdbg.diag_buf->int_info_off - sizeof(uint32_t)));
	ptr = ((unsigned char *)tzdbg.diag_buf +
					tzdbg.diag_buf->int_info_off);
	if (*num_int)
		int_info_size = ((tzdbg.diag_buf->ring_off -
				tzdbg.diag_buf->int_info_off)/(*num_int));

	for (i = 0; i < (*num_int); i++) {
		tzdbg_ptr = (struct tzdbg_int_t *)ptr;
		len += snprintf(tzdbg.disp_buf + len,
				(tzdbg.diag_size - 1) - len,
				"     Interrupt Number          : 0x%x\n"
				"     Type of Interrupt         : 0x%x\n"
				"     Description of interrupt  : %s\n",
				tzdbg_ptr->int_num,
				(uint32_t)tzdbg_ptr->int_info,
				(uint8_t *)tzdbg_ptr->int_desc);
		for (j = 0; j < tzdbg.diag_buf->cpu_count; j++) {
			len += snprintf(tzdbg.disp_buf + len,
				(tzdbg.diag_size - 1) - len,
				"     int_count on CPU # %d      : %u\n",
				(uint32_t)j,
				(uint32_t)tzdbg_ptr->int_count[j]);
		}
		len += snprintf(tzdbg.disp_buf + len,
				(tzdbg.diag_size - 1) - len, "\n");

		if (len > (tzdbg.diag_size - 1)) {
			pr_warn("%s: Cannot fit all info into the buffer\n",
								__func__);
			break;
		}

		ptr += int_info_size;
	}
	tzdbg.stat[TZDBG_INTERRUPT].data = tzdbg.disp_buf;
	return len;
}

static int _disp_tz_log_stats_legacy(void)
{
	int len = 0;
	unsigned char *ptr;

	ptr = (unsigned char *)tzdbg.diag_buf +
					tzdbg.diag_buf->ring_off;
	len += snprintf(tzdbg.disp_buf, (tzdbg.diag_size - 1) - len,
							"%s\n", ptr);

	tzdbg.stat[TZDBG_LOG].data = tzdbg.disp_buf;
	return len;
}

static int _disp_log_stats(struct tzdbg_log_t *log,
			struct tzdbg_log_pos_t *log_start, uint32_t log_len,
			size_t count, uint32_t buf_idx)
{
	uint32_t wrap_start;
	uint32_t wrap_end;
	uint32_t wrap_cnt;
	int max_len;
	int len = 0;
	int i = 0;

	wrap_start = log_start->wrap;
	wrap_end = log->log_pos.wrap;

	/* Calculate difference in # of buffer wrap-arounds */
	if (wrap_end >= wrap_start) {
		wrap_cnt = wrap_end - wrap_start;
	} else {
		/* wrap counter has wrapped around, invalidate start position */
		wrap_cnt = 2;
	}

	if (wrap_cnt > 1) {
		/* end position has wrapped around more than once, */
		/* current start no longer valid                   */
		log_start->wrap = log->log_pos.wrap - 1;
		log_start->offset = (log->log_pos.offset + 1) % log_len;
	} else if ((wrap_cnt == 1) &&
		(log->log_pos.offset > log_start->offset)) {
		/* end position has overwritten start */
		log_start->offset = (log->log_pos.offset + 1) % log_len;
	}

	while (log_start->offset == log->log_pos.offset) {
		/*
		 * No data in ring buffer,
		 * so we'll hang around until something happens
		 */
		unsigned long t = msleep_interruptible(50);
		if (t != 0) {
			/* Some event woke us up, so let's quit */
			return 0;
		}

		if (buf_idx == TZDBG_LOG)
			memcpy_fromio((void *)tzdbg.diag_buf, tzdbg.virt_iobase,
						tzdbg.diag_size);

	}

	max_len = (count > tzdbg.diag_size) ? tzdbg.diag_size : count;

	/*
	 *  Read from ring buff while there is data and space in return buff
	 */
	while ((log_start->offset != log->log_pos.offset) && (len < max_len)) {
		tzdbg.disp_buf[i++] = log->log_buf[log_start->offset];
		log_start->offset = (log_start->offset + 1) % log_len;
		if (0 == log_start->offset)
			++log_start->wrap;
		++len;
	}

	/*
	 * return buffer to caller
	 */
	tzdbg.stat[buf_idx].data = tzdbg.disp_buf;
	return len;
}

static int _disp_tz_log_stats(size_t count)
{
	static struct tzdbg_log_pos_t log_start = {0};
	struct tzdbg_log_t *log_ptr;
	log_ptr = (struct tzdbg_log_t *)((unsigned char *)tzdbg.diag_buf +
				tzdbg.diag_buf->ring_off -
				offsetof(struct tzdbg_log_t, log_buf));

	return _disp_log_stats(log_ptr, &log_start,
				tzdbg.diag_buf->ring_len, count, TZDBG_LOG);
}

static int _disp_qsee_log_stats(size_t count)
{
	static struct tzdbg_log_pos_t log_start = {0};

	return _disp_log_stats(g_qsee_log, &log_start,
			QSEE_LOG_BUF_SIZE - sizeof(struct tzdbg_log_pos_t),
			count, TZDBG_QSEE_LOG);
}

static ssize_t tzdbgfs_read(struct file *file, char __user *buf,
	size_t count, loff_t *offp)
{
	int len = 0;
	int *tz_id =  file->private_data;

	memcpy_fromio((void *)tzdbg.diag_buf, tzdbg.virt_iobase,
						tzdbg.diag_size);
	switch (*tz_id) {
	case TZDBG_BOOT:
		len = _disp_tz_boot_stats();
		break;
	case TZDBG_RESET:
		len = _disp_tz_reset_stats();
		break;
	case TZDBG_INTERRUPT:
		len = _disp_tz_interrupt_stats();
		break;
	case TZDBG_GENERAL:
		len = _disp_tz_general_stats();
		break;
	case TZDBG_VMID:
		len = _disp_tz_vmid_stats();
		break;
	case TZDBG_LOG:
		if (TZBSP_DIAG_MAJOR_VERSION_LEGACY <
				(tzdbg.diag_buf->version >> 16)) {
			len = _disp_tz_log_stats(count);
			*offp = 0;
		} else {
			len = _disp_tz_log_stats_legacy();
		}
		break;
	case TZDBG_QSEE_LOG:
		len = _disp_qsee_log_stats(count);
		*offp = 0;
		break;
	default:
		break;
	}

	if (len > count)
		len = count;

	return simple_read_from_buffer(buf, len, offp,
				tzdbg.stat[(*tz_id)].data, len);
}

static int tzdbgfs_open(struct inode *inode, struct file *pfile)
{
	pfile->private_data = inode->i_private;
	return 0;
}

const struct file_operations tzdbg_fops = {
	.owner   = THIS_MODULE,
	.read    = tzdbgfs_read,
	.open    = tzdbgfs_open,
};

static struct ion_client  *g_ion_clnt;
static struct ion_handle *g_ihandle;

/*
 * Allocates log buffer from ION, registers the buffer at TZ
 */
static void tzdbg_register_qsee_log_buf(void)
{
	/* register log buffer scm request */
	struct qseecom_reg_log_buf_ireq req;

	/* scm response */
	struct qseecom_command_scm_resp resp = {};
	ion_phys_addr_t pa = 0;
	size_t len;
	int ret = 0;

	/* Create ION msm client */
	g_ion_clnt = msm_ion_client_create("qsee_log");
	if (g_ion_clnt == NULL) {
		pr_err("%s: Ion client cannot be created\n", __func__);
		return;
	}

	g_ihandle = ion_alloc(g_ion_clnt, QSEE_LOG_BUF_SIZE,
			4096, ION_HEAP(ION_QSECOM_HEAP_ID), 0);
	if (IS_ERR_OR_NULL(g_ihandle)) {
		pr_err("%s: Ion client could not retrieve the handle\n",
			__func__);
		goto err1;
	}

	ret = ion_phys(g_ion_clnt, g_ihandle, &pa, &len);
	if (ret) {
		pr_err("%s: Ion conversion to physical address failed\n",
			__func__);
		goto err2;
	}

	req.qsee_cmd_id = QSEOS_REGISTER_LOG_BUF_COMMAND;
	req.phy_addr = (uint32_t)pa;
	req.len = len;

	if (!is_scm_armv8()) {
		/*  SCM_CALL  to register the log buffer */
		ret = scm_call(SCM_SVC_TZSCHEDULER, 1,  &req, sizeof(req),
			&resp, sizeof(resp));
	} else {
		struct scm_desc desc = {0};
		desc.args[0] = (uint32_t)pa;
		desc.args[1] = len;
		desc.arginfo = 0x22;
		ret = scm_call2(SCM_QSEEOS_FNID(1, 6), &desc);
		resp.result = desc.ret[0];
	}

	if (ret) {
		pr_err("%s: scm_call to register log buffer failed\n",
			__func__);
		goto err2;
	}

	if (resp.result != QSEOS_RESULT_SUCCESS) {
		pr_err(
		"%s: scm_call to register log buf failed, resp result =%d\n",
		__func__, resp.result);
		goto err2;
	}

	g_qsee_log =
		(struct tzdbg_log_t *)ion_map_kernel(g_ion_clnt, g_ihandle);

	if (IS_ERR(g_qsee_log)) {
		pr_err("%s: Couldn't map ion buffer to kernel\n",
			__func__);
		goto err2;
	}

	g_qsee_log->log_pos.wrap = g_qsee_log->log_pos.offset = 0;
	return;

err2:
	ion_free(g_ion_clnt, g_ihandle);
	g_ihandle = NULL;
err1:
	ion_client_destroy(g_ion_clnt);
	g_ion_clnt = NULL;
}

static int  tzdbgfs_init(struct platform_device *pdev)
{
	int rc = 0;
	int i;
	struct dentry           *dent_dir;
	struct dentry           *dent;

	dent_dir = debugfs_create_dir("tzdbg", NULL);
	if (dent_dir == NULL) {
		dev_err(&pdev->dev, "tzdbg debugfs_create_dir failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < TZDBG_STATS_MAX; i++) {
		tzdbg.debug_tz[i] = i;
		dent = debugfs_create_file(tzdbg.stat[i].name,
				S_IRUGO, dent_dir,
				&tzdbg.debug_tz[i], &tzdbg_fops);
		if (dent == NULL) {
			dev_err(&pdev->dev, "TZ debugfs_create_file failed\n");
			rc = -ENOMEM;
			goto err;
		}
	}
	tzdbg.disp_buf = kzalloc(tzdbg.diag_size, GFP_KERNEL);
	if (tzdbg.disp_buf == NULL) {
		pr_err("%s: Can't Allocate memory for tzdbg.disp_buf\n",
			__func__);

		goto err;
	}
	platform_set_drvdata(pdev, dent_dir);
	return 0;
err:
	debugfs_remove_recursive(dent_dir);

	return rc;
}

static void tzdbgfs_exit(struct platform_device *pdev)
{
	struct dentry           *dent_dir;

	kzfree(tzdbg.disp_buf);
	dent_dir = platform_get_drvdata(pdev);
	debugfs_remove_recursive(dent_dir);
	if (g_ion_clnt != NULL) {
		if (!IS_ERR_OR_NULL(g_ihandle)) {
			ion_unmap_kernel(g_ion_clnt, g_ihandle);
			ion_free(g_ion_clnt, g_ihandle);
		}
	ion_client_destroy(g_ion_clnt);
}
}

#ifdef CONFIG_MSM_TZ_LOG_WDOG_DUMP

#define MSMWDTD(fmt, args...) persistent_ram_annotation_append(fmt, ##args)

#define MSMWDT_ERR(fmt, args...) do { \
	pr_err("TzLog: "fmt, ##args); \
	MSMWDTD("TzLog: "fmt, ##args); \
} while (0)

#define MSMWDTD_IFWDOG(fmt, args...) do { \
	if (bi_powerup_reason() == PU_REASON_WDOG_AP_RESET) \
		MSMWDTD(fmt, ##args); \
} while (0)

static void tzlog_bck_show_boot_info(struct tzdbg_t *diag_buf)
{
	int cpu;
	struct tzdbg_boot_info_t *ptr;
	struct tzdbg_boot_info2_t *ptr2;
	int v2 = 0;

	if ((tzdbg.diag_buf->reset_info_off - tzdbg.diag_buf->boot_info_off) ==
			(sizeof(*ptr2) * tzdbg.diag_buf->cpu_count))
		v2 = 1;
	ptr = (struct tzdbg_boot_info_t *)((u8 *)diag_buf +
			diag_buf->boot_info_off);
	ptr2 = (struct tzdbg_boot_info2_t *)ptr;
	MSMWDTD("\n--- TZ Power Collapse Counters\n");
	MSMWDTD("     | WarmEntry : WarmExit :  PCEntry :");
	MSMWDTD("   PCExit : JumpAddr |\n");
	for (cpu = 0; cpu < tzdbg.diag_buf->cpu_count; cpu++) {
		int power_collapsed;
		if (v2)
			power_collapsed = ptr2->wb_entry_cnt +
				ptr2->pc_exit_cnt - ptr2->pc_entry_cnt;
		else
			power_collapsed = ptr->wb_entry_cnt +
				ptr->pc_exit_cnt - ptr->pc_entry_cnt;
		if (cpu)
			power_collapsed--;
		MSMWDTD("CPU%d |  %8x : %8x : %8x : %8x : %8lx | %sPC\n",
			cpu, (v2 ? ptr2->wb_entry_cnt : ptr->wb_entry_cnt),
			(v2 ? ptr2->wb_exit_cnt : ptr->wb_exit_cnt),
			(v2 ? ptr2->pc_entry_cnt : ptr->pc_entry_cnt),
			(v2 ? ptr2->pc_exit_cnt : ptr->pc_exit_cnt),
			(unsigned long)(v2 ? ptr2->warm_jmp_addr :
					ptr->warm_jmp_addr),
			power_collapsed ? "IN-" : "NOT-");
		ptr++;
		ptr2++;
	}
}

static void tzlog_bck_show_log(struct tzdbg_t *diag_buf)
{
	struct tzdbg_log_t *log_ptr;
	const char *log_buf, *p, *start;

	if (TZBSP_DIAG_MAJOR_VERSION_LEGACY >=
				(tzdbg.diag_buf->version >> 16))
		return;

	log_buf = (const char *)diag_buf + diag_buf->ring_off;
	log_ptr = (struct tzdbg_log_t *)(log_buf -
				offsetof(struct tzdbg_log_t, log_buf));

	if (log_ptr->log_pos.offset >= diag_buf->ring_len)
		return;
	MSMWDTD("--- TZ Log start ---\n");
	if (log_ptr->log_pos.wrap) {
		for (start = log_buf + log_ptr->log_pos.offset, p = start;
				p < (log_buf + diag_buf->ring_len); p++) {
			if (isprint(*p))
				MSMWDTD("%c", *p);
			else if ((p > start) && isprint(*(p-1)))
				MSMWDTD("\n");
		}
	}
	for (start = log_buf, p = start;
			p < (log_buf + log_ptr->log_pos.offset); p++) {
		if (isprint(*p))
			MSMWDTD("%c", *p);
		else if ((p > start) && isprint(*(p-1)))
			MSMWDTD("\n");
	}
	MSMWDTD("\n--- TZ Log end ---\n");
}

static void tzlog_bck_show(struct tzdbg_t *diag_buf)
{
	memcpy_fromio((void *)tzdbg.diag_buf, tzdbg.virt_iobase,
						DEBUG_MAX_RW_BUF);
	if (memcmp(diag_buf, tzdbg.diag_buf, tzdbg.diag_buf->vmid_info_off)) {
		MSMWDTD_IFWDOG("TzLog: No valid backup.\n");
		goto reset;
	}
	tzlog_bck_show_boot_info(diag_buf);
	tzlog_bck_show_log(diag_buf);
reset:
	memset_io(diag_buf, 0, tzdbg.diag_size);
}

static void tzlog_bck_check(struct platform_device *pdev)
{
	struct device_node *pnode;
	struct tzdbg_t *diag_bck_vaddr;
	phys_addr_t diag_bck_paddr;
	size_t diag_bck_size;

	pnode = of_parse_phandle(pdev->dev.of_node,
			"linux,contiguous-region", 0);
	if (!pnode) {
		MSMWDT_ERR("Unable to find contiguous-region\n");
		goto no_reservation;
	}
	if (!of_get_address(pnode, 0, NULL, NULL)) {
		of_node_put(pnode);
		MSMWDT_ERR("Addr not found for contiguous-region\n");
		goto no_reservation;
	}
	of_node_put(pnode);

	diag_bck_paddr = cma_get_base(&pdev->dev);
	diag_bck_size = cma_get_size(&pdev->dev);

	if (diag_bck_size < tzdbg.diag_size) {
		MSMWDT_ERR("Mem reserve too small %zx/%zx\n",
				diag_bck_size, tzdbg.diag_size);
		goto no_reservation;
	}

	diag_bck_size = tzdbg.diag_size;
	diag_bck_vaddr = dma_remap(&pdev->dev, NULL, diag_bck_paddr,
					diag_bck_size, NULL);
	if (diag_bck_vaddr) {
		tzlog_bck_show(diag_bck_vaddr);
		dma_unremap(&pdev->dev, diag_bck_vaddr, diag_bck_size);
	} else {
		MSMWDT_ERR("Cannot remap buffer %pa size %zx\n",
					&diag_bck_paddr, diag_bck_size);
	}

	if (MSM_DUMP_MAJOR(msm_dump_table_version()) == 1) {
		struct msm_client_dump dump_entry;

		dump_entry.id = MSM_TZ_LOG;
		dump_entry.start_addr = diag_bck_paddr;
		dump_entry.end_addr = diag_bck_paddr + diag_bck_size;
		if (msm_dump_tbl_register(&dump_entry))
			MSMWDT_ERR("Cannot register buffer: %pa\n",
					&diag_bck_paddr);
	} else {
		struct msm_dump_entry dump_entry;
		struct msm_dump_data *tz_dump_data;
		int ret;

		tz_dump_data = kzalloc(sizeof(struct msm_dump_data),
						GFP_KERNEL);
		if (!tz_dump_data) {
			MSMWDT_ERR("Cannot alloc dump data structure.\n");
			return;
		}
		tz_dump_data->addr = diag_bck_paddr;
		tz_dump_data->len = diag_bck_size;
		dump_entry.id = MSM_DUMP_DATA_TZ_LOG;
		dump_entry.addr = virt_to_phys(tz_dump_data);
		ret = msm_dump_data_register(MSM_DUMP_TABLE_APPS, &dump_entry);
		if (ret) {
			MSMWDT_ERR("Registering dump data failed.\n");
			kfree(tz_dump_data);
		}
	}
no_reservation:
	return;
}

#else

static inline void tzlog_bck_check(struct platform_device *pdev)
{
}

#endif /* CONFIG_MSM_TZ_LOG_WDOG_DUMP */

/*
 * Driver functions
 */
static int tz_log_probe(struct platform_device *pdev)
{
	struct resource *resource;
	void __iomem *virt_iobase;
	phys_addr_t tzdiag_phy_iobase;
	uint32_t *ptr = NULL;

	/*
	 * Get address that stores the physical location of 4KB
	 * diagnostic data
	 */
	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource) {
		dev_err(&pdev->dev,
				"%s: ERROR Missing MEM resource\n", __func__);
		return -ENXIO;
	};
	/*
	 * Map address that stores the physical location of 4KB
	 * diagnostic data
	 */
	virt_iobase = devm_ioremap_nocache(&pdev->dev, resource->start,
				resource->end - resource->start + 1);
	if (!virt_iobase) {
		dev_err(&pdev->dev,
			"%s: ERROR could not ioremap: start=%pr, len=%u\n",
			__func__, &resource->start,
			(unsigned int)(resource->end - resource->start + 1));
		return -ENXIO;
	}
	/*
	 * Retrieve the address of 4KB diagnostic data
	 */
	tzdiag_phy_iobase = readl_relaxed(virt_iobase);

	/*
	 * Map the 4KB diagnostic information area
	 */
	tzdbg.virt_iobase = devm_ioremap_nocache(&pdev->dev,
				tzdiag_phy_iobase, DEBUG_MAX_RW_BUF);

	if (!tzdbg.virt_iobase) {
		dev_err(&pdev->dev,
			"%s: ERROR could not ioremap: start=%pr, len=%u\n",
			__func__, &tzdiag_phy_iobase,
			DEBUG_MAX_RW_BUF);
		return -ENXIO;
	}
	tzdbg.diag_size = readl_relaxed(tzdbg.virt_iobase +
				offsetof(struct tzdbg_t, ring_off)) +
			readl_relaxed(tzdbg.virt_iobase +
				offsetof(struct tzdbg_t, ring_len));
	if (tzdbg.diag_size > DEBUG_MAX_RW_BUF) {
		devm_iounmap(&pdev->dev, tzdbg.virt_iobase);
		tzdbg.virt_iobase = devm_ioremap_nocache(&pdev->dev,
				tzdiag_phy_iobase, tzdbg.diag_size);
		if (!tzdbg.virt_iobase) {
			dev_err(&pdev->dev,
				"ERROR could not ioremap: start=%pr, len=%zu\n",
					&tzdiag_phy_iobase, tzdbg.diag_size);
			return -ENXIO;
		}
	}

	ptr = kzalloc(tzdbg.diag_size, GFP_KERNEL);
	if (ptr == NULL) {
		pr_err("%s: Can't Allocate memory: ptr\n",
			__func__);
		return -ENXIO;
	}

	tzdbg.diag_buf = (struct tzdbg_t *)ptr;

	if (tzdbgfs_init(pdev))
		goto err;

	tzdbg_register_qsee_log_buf();
	tzlog_bck_check(pdev);
	return 0;
err:
	kfree(tzdbg.diag_buf);
	return -ENXIO;
}


static int tz_log_remove(struct platform_device *pdev)
{
	kzfree(tzdbg.diag_buf);
	tzdbgfs_exit(pdev);

	return 0;
}

static struct of_device_id tzlog_match[] = {
	{	.compatible = "qcom,tz-log",
	},
	{}
};

static struct platform_driver tz_log_driver = {
	.probe		= tz_log_probe,
	.remove		= tz_log_remove,
	.driver		= {
		.name = "tz_log",
		.owner = THIS_MODULE,
		.of_match_table = tzlog_match,
	},
};

static int __init tz_log_init(void)
{
	return platform_driver_register(&tz_log_driver);
}

static void __exit tz_log_exit(void)
{
	platform_driver_unregister(&tz_log_driver);
}

module_init(tz_log_init);
module_exit(tz_log_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("TZ Log driver");
MODULE_VERSION("1.1");
MODULE_ALIAS("platform:tz_log");

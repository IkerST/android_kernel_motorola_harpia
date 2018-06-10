/*
 * Copyright (C) 2014 Motorola, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Necessary includes for device drivers */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/io.h>
#include <linux/sec_export.h>
#include <linux/platform_device.h>
#include <asm/cacheflush.h>
#include <soc/qcom/scm.h>
#include "sec_core.h"

static ssize_t sec_read(struct file *filp, char *buf,
		size_t count, loff_t *f_pos);
static int sec_open(struct inode *inode, struct file *filp);
static int sec_release(struct inode *inode, struct file *filp);
static long sec_ioctl(struct file *file, unsigned int ioctl_num,
		unsigned long ioctl_param);
static void sec_buf_prepare(void);
static bool sec_buf_updated(void);

#define TZ_OWNER_OEM 3
#define TZBSP_SVC_OEM 254
#define SEC_BUF_SIZE 32

#define TZ_SYSCALL_CREATE_SMC_ID(o, s, f) \
	((u32)((((o & 0x3f) << 24) | (s & 0xff) << 8) | (f & 0xff)))

DEFINE_SCM_BUFFER(sec_shared_mem);
static u32 sec_phy_mem;
static int sec_failures;

/* Structure that declares the usual file */
/* access functions */
const struct file_operations sec_fops = {
	.owner = THIS_MODULE,
	.read = sec_read,
	.open = sec_open,
	.release = sec_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = sec_ioctl,
#endif
	.unlocked_ioctl = sec_ioctl
};

struct sec_cmd {
	u32 mot_cmd;
	u32 parm1;
	u32 parm2;
	u32 parm3;
};

static struct miscdevice sec_dev = {
	MISC_DYNAMIC_MINOR,
	"sec",
	&sec_fops
};

static DEFINE_MUTEX(sec_core_lock);

/****************************************************************************/
/*   KERNEL DRIVER APIs, ONLY IOCTL RESPONDS BACK TO USER SPACE REQUESTS    */
/****************************************************************************/

int sec_open(struct inode *inode, struct file *filp)
{
	/* Not supported, return Success */
	return 0;
}

int sec_release(struct inode *inode, struct file *filp)
{

	/* Not supported, return Success */
	return 0;
}

ssize_t sec_read(struct file *filp, char *buf,
		 size_t count, loff_t *f_pos)
{
	/* Not supported, return Success */
	return 0;
}

static void sec_buf_prepare()
{
	memset(sec_shared_mem, 0xff, SEC_BUF_SIZE);
	dmac_flush_range(sec_shared_mem, sec_shared_mem + SEC_BUF_SIZE);
	sec_phy_mem = SCM_BUFFER_PHYS(sec_shared_mem);
}

static bool sec_buf_updated()
{
	int i;

	/* ARM64 invalidate cache memory */
	dmac_inv_range(sec_shared_mem, sec_shared_mem + PAGE_SIZE);

	for (i = 0; i < SEC_BUF_SIZE; i++)
		if (sec_shared_mem[i] != 0xff)
			return true;

	return false;
}

static int sec_smc(u32 cmd, u32 param1, u32 param2)
{
	if (!is_scm_armv8()) {

		struct sec_cmd my_cmd;

		my_cmd.mot_cmd = cmd;
		my_cmd.parm1 = param1;
		my_cmd.parm2 = param2;

		return scm_call(TZBSP_SVC_OEM, 1, &my_cmd,
			sizeof(my_cmd), NULL, 0);
	} else {

		struct scm_desc desc = {0};
		int ret;
		u32 id = TZ_SYSCALL_CREATE_SMC_ID(TZ_OWNER_OEM, 0x0A, 0x0A);

		desc.arginfo = SCM_ARGS(3);
		desc.args[0] = cmd;
		desc.args[1] = param1;
		desc.args[2] = param2;
		ret = scm_call2(id, &desc);

		if (ret || desc.ret[0])
			pr_err("sec: SCM fail %d %lld\n", ret, desc.ret[0]);

		return ret;
	}
}

long sec_ioctl(struct file *file, unsigned int ioctl_num,
		unsigned long ioctl_param)
{
	struct SEC_EFUSE_PARM_T efuse_data;
	long ret_val = SEC_KM_FAIL;
	u32 ctr;
	int ret;

	mutex_lock(&sec_core_lock);

	sec_buf_prepare();

	switch (ioctl_num) {

	case SEC_IOCTL_READ_PROC_ID:

		for (ctr = 0; ctr < 5 && ret_val != SEC_KM_SUCCESS; ctr++) {

			ret = sec_smc(10, sec_phy_mem, SEC_PROC_ID_SIZE);

			if (ret == 0) {

				if (copy_to_user((void __user *)ioctl_param,
					(const void *) sec_shared_mem,
					SEC_PROC_ID_SIZE) == 0) {

					/* check data was actually updated */
					if (sec_buf_updated())
						ret_val = SEC_KM_SUCCESS;
					else
						pr_err("sec: not updated\n");

					ret_val = SEC_KM_SUCCESS;
				}
			} else
				 pr_err("sec: scm call failed!\n");
		}


		break;

	case SEC_IOCTL_WRITE_FUSE:

		if (copy_from_user(&efuse_data, (void __user *)
			ioctl_param, sizeof(efuse_data)) != 0) {

			break;
		}

		if (sec_smc(2, efuse_data.which_bank,
			efuse_data.efuse_value) == 0) {

			ret_val = SEC_KM_SUCCESS;
		}

		break;

	case SEC_IOCTL_READ_FUSE:

		if (copy_from_user(&efuse_data, (void *)ioctl_param,
			sizeof(efuse_data)) != 0) {

			break;
		}

		for (ctr = 0; ctr < 5 && ret_val != SEC_KM_SUCCESS; ctr++) {

			ret = sec_smc(1, efuse_data.which_bank, sec_phy_mem);

			if (ret == 0) {

				efuse_data.efuse_value = *(u32 *)sec_shared_mem;

				if (copy_to_user((void *)ioctl_param,
					&efuse_data, sizeof(efuse_data)) == 0) {

					/* check data was actually updated */
					if (sec_buf_updated())
						ret_val = SEC_KM_SUCCESS;
					else
						pr_err("sec: not updated!\n");
				}
			} else
				pr_err("sec: scm call failed!\n");
		}

		break;

	case SEC_IOCTL_GET_TZ_VERSION:

		if (sec_smc(11, sec_phy_mem, 0) == 0) {

			if (copy_to_user((void *)ioctl_param,
				sec_shared_mem, 4) == 0) {

				ret_val = SEC_KM_SUCCESS;
			}
		}

		break;

	case SEC_IOCTL_GET_TZ_CODES:

		pr_err("sec: fail counter = %d\n", sec_failures);
		print_hab_fail_codes();
		break;

	default:
		 pr_err("sec: error\n");
		break;
	}

	if (ret_val != SEC_KM_SUCCESS)
		sec_failures++;

	mutex_unlock(&sec_core_lock);

	return ret_val;
}

void print_hab_fail_codes(void)
{
	mutex_lock(&sec_core_lock);

	sec_buf_prepare();
	sec_smc(9, 16, sec_phy_mem);

	pr_err("HAB fail codes: 0x%x 0x%x 0x%x 0x%x\n",
	sec_shared_mem[0], sec_shared_mem[1],
	sec_shared_mem[2], sec_shared_mem[3]);

	mutex_unlock(&sec_core_lock);
}
EXPORT_SYMBOL(print_hab_fail_codes);

static int msm_efuse_probe(struct platform_device *pdev)
{
	int result;

	result = misc_register(&sec_dev);

	if (result) {
		pr_err("sec: cannot obtain major number\n");
		return result;
	}

	return 0;
}

static int msm_efuse_remove(struct platform_device *pdev)
{
	kzfree(sec_shared_mem);
	return 0;
}

static struct of_device_id msm_match_table[] = {
	{.compatible = "qcom,msm-efuse"},
	{},
};

static struct platform_driver msm_efuse_driver = {
	.probe = msm_efuse_probe,
	.remove = msm_efuse_remove,
	.driver         = {
		.name = "msm_efuse",
		.owner = THIS_MODULE,
		.of_match_table = msm_match_table
	},
};

static int __init msm_efuse_init(void)
{
	return platform_driver_register(&msm_efuse_driver);
}

static void __exit msm_efuse_exit(void)
{
	platform_driver_unregister(&msm_efuse_driver);
}

module_init(msm_efuse_init)
module_exit(msm_efuse_exit)
/****************************************************************************/
/*Kernel Module License Information                                         */
/****************************************************************************/
MODULE_LICENSE("GPL");
MODULE_AUTHOR("MOTOROLA");

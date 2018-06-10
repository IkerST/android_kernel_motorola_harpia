/*
 * Copyright (C) 2012 Motorola Mobility, LLC.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _TFA9890_H
#define _TFA9890_H

struct tfa9890_pdata {
	int reset_gpio;
	int max_vol_steps;
	const char *tfa_dev;
	const char *fw_path;
	const char *fw_name;
	int pcm_start_delay;
};

#endif

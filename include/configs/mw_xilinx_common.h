/*
 * (C) Copyright 2017 The MathWorks, Inc.
 *
 * Common MathWorks configuration options for all Zynq boards.
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/* 
 * Initialize environment:
 * Run the saveenv command on the first boot to initialize the env
 * storage.
 */

#ifndef __CONFIG_MW_XILINX_COMMON_H
#define __CONFIG_MW_XILINX_COMMON_H

#if defined(CONFIG_ENV_IS_IN_FAT) || defined(CONFIG_ENV_IS_IN_MMC)
# define ENV_CMD_PRE_SAVEENV		"mmc rescan;"
#else
# define ENV_CMD_PRE_SAVEENV		""
#endif

#if defined(CONFIG_ENV_IS_IN_FAT) || defined(CONFIG_ZYNQMP_INIT_ENV)
# define ENV_CMD_INIT_ENV_ONCE \
	"uenv_init=" \
		"echo Storing default uboot environment...;" \
		"env set uenv_init true;" \
		ENV_CMD_PRE_SAVEENV \
		"saveenv\0"
#else
# define ENV_CMD_INIT_ENV_ONCE \
	"uenv_init=true \0"
#endif

#endif /* __CONFIG_MW_XILINX_COMMON_H */

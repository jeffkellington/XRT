/*
 * This file is part of the Xilinx DMA IP Core driver for Linux
 *
 * Copyright (c) 2017-present,  Xilinx, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */
/**
 * @file
 * @brief This file contains the declarations for QDMA PCIe device
 *
 */
#define pr_fmt(fmt)	KBUILD_MODNAME ":%s: " fmt, __func__

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>

#include "qdma_regs.h"
#include "xdev.h"
#include "qdma_mbox.h"

/**
 * qdma device management
 * maintains a list of the qdma devices
 */
static LIST_HEAD(xdev_list);

/**
 * mutex defined for qdma device management
 */
static DEFINE_MUTEX(xdev_mutex);

#ifndef list_last_entry
#define list_last_entry(ptr, type, member) \
		list_entry((ptr)->prev, type, member)
#endif

/* entern declarations */
void qdma_device_attributes_get(struct xlnx_dma_dev *xdev);
int qdma_device_init(struct xlnx_dma_dev *);
void qdma_device_cleanup(struct xlnx_dma_dev *);

/*****************************************************************************/
/**
 * xdev_list_first() - handler to return the first xdev entry from the list
 *
 * @return	pointer to first xlnx_dma_dev on success
 * @return	NULL on failure
 *****************************************************************************/
struct xlnx_dma_dev *xdev_list_first(void)
{
	struct xlnx_dma_dev *xdev;

	mutex_lock(&xdev_mutex);
	xdev = list_first_entry(&xdev_list, struct xlnx_dma_dev, list_head);
	mutex_unlock(&xdev_mutex);

	return xdev;
}

/*****************************************************************************/
/**
 * xdev_list_next() - handler to return the next xdev entry from the list
 *
 * @param[in]	xdev:	pointer to current xdev
 *
 * @return	pointer to next xlnx_dma_dev on success
 * @return	NULL on failure
 *****************************************************************************/
struct xlnx_dma_dev *xdev_list_next(struct xlnx_dma_dev *xdev)
{
	struct xlnx_dma_dev *next;

	mutex_lock(&xdev_mutex);
	next = list_next_entry(xdev, list_head);
	mutex_unlock(&xdev_mutex);

	return next;
}

/*****************************************************************************/
/**
 * xdev_list_dump() - list the dma device details
 *
 * @param[in]	buflen:		length of the input buffer
 * @param[out]	buf:		message buffer
 *
 * @return	pointer to next xlnx_dma_dev on success
 * @return	NULL on failure
 *****************************************************************************/
int xdev_list_dump(char *buf, int buflen)
{
	struct xlnx_dma_dev *xdev, *tmp;
	int len = 0;

	mutex_lock(&xdev_mutex);
	list_for_each_entry_safe(xdev, tmp, &xdev_list, list_head) {
		len += sprintf(buf + len, "qdma%05x\t%02x:%02x.%02x\n",
				xdev->conf.bdf, xdev->conf.pdev->bus->number,
				PCI_SLOT(xdev->conf.pdev->devfn),
				PCI_FUNC(xdev->conf.pdev->devfn));
		if (len >= buflen)
			break;
	}
	mutex_unlock(&xdev_mutex);

	buf[len] = '\0';
	return len;
}

/*****************************************************************************/
/**
 * xdev_list_add() - add a new node to the xdma device lsit
 *
 * @param[in]	xdev:	pointer to current xdev
 *
 * @return	none
 *****************************************************************************/
static inline void xdev_list_add(struct xlnx_dma_dev *xdev)
{
	u32 bdf = 0;
	struct xlnx_dma_dev *_xdev, *tmp;
	u32 last_bus = 0;
	u32 last_dev = 0;

	mutex_lock(&xdev_mutex);
	bdf = ((xdev->conf.pdev->bus->number << PCI_SHIFT_BUS) |
			(PCI_SLOT(xdev->conf.pdev->devfn) << PCI_SHIFT_DEV) |
			PCI_FUNC(xdev->conf.pdev->devfn));
	xdev->conf.bdf = bdf;
	list_add_tail(&xdev->list_head, &xdev_list);

	/*
	 * Iterate through the list of devices. Increment cfg_done, to
	 * get the mulitplier for initial configuration of queues. A
	 * '0' indicates queue is already configured. < 0, indicates
	 * config done using sysfs entry
	 */
	list_for_each_entry_safe(_xdev, tmp, &xdev_list, list_head) {
		/*are we dealing with a different card?*/
#ifdef __QDMA_VF__
		/** for VF check only bus number, as dev number can change
		 * in a single card
		 */
		if ((last_bus != _xdev->conf.pdev->bus->number))
#else
		if ((last_bus != _xdev->conf.pdev->bus->number) ||
				(last_dev != PCI_SLOT(_xdev->conf.pdev->devfn)))
#endif
			xdev->conf.idx = 0;
		xdev->conf.idx++;
		last_bus = _xdev->conf.pdev->bus->number;
		last_dev = PCI_SLOT(xdev->conf.pdev->devfn);
	}
	xdev->conf.cur_cfg_state = CFG_UNCONFIGURED;
	mutex_unlock(&xdev_mutex);
}


#undef list_last_entry
/*****************************************************************************/
/**
 * xdev_list_add() - remove a node from the xdma device lsit
 *
 * @param[in]	xdev:	pointer to current xdev
 *
 * @return	none
 *****************************************************************************/
static inline void xdev_list_remove(struct xlnx_dma_dev *xdev)
{
	mutex_lock(&xdev_mutex);
	list_del(&xdev->list_head);
	mutex_unlock(&xdev_mutex);
}

/*****************************************************************************/
/**
 * xdev_find_by_pdev() - find the xdev using struct pci_dev
 *
 * @param[in]	pdev:	pointer to struct pci_dev
 *
 * @return	pointer to xlnx_dma_dev on success
 * @return	NULL on failure
 *****************************************************************************/
struct xlnx_dma_dev *xdev_find_by_pdev(struct pci_dev *pdev)
{
	struct xlnx_dma_dev *xdev, *tmp;

	mutex_lock(&xdev_mutex);
	list_for_each_entry_safe(xdev, tmp, &xdev_list, list_head) {
		if (xdev->conf.pdev == pdev) {
			mutex_unlock(&xdev_mutex);
			return xdev;
		}
	}
	mutex_unlock(&xdev_mutex);
	return NULL;
}

/*****************************************************************************/
/**
 * xdev_find_by_idx() - find the xdev using the index value
 *
 * @param[in]	idx:	index value in the xdev list
 *
 * @return	pointer to xlnx_dma_dev on success
 * @return	NULL on failure
 *****************************************************************************/
struct xlnx_dma_dev *xdev_find_by_idx(int idx)
{
	struct xlnx_dma_dev *xdev, *tmp;

	mutex_lock(&xdev_mutex);
	list_for_each_entry_safe(xdev, tmp, &xdev_list, list_head) {
		if (xdev->conf.bdf == idx) {
			mutex_unlock(&xdev_mutex);
			return xdev;
		}
	}
	mutex_unlock(&xdev_mutex);
	return NULL;
}

/*****************************************************************************/
/**
 * xdev_check_hndl() - helper function to validate the device handle
 *
 * @param[in]	pdev:	pointer to struct pci_dev
 * @param[in]	hndl:	device handle
 *
 * @return	0: success
 * @return	<0: on failure
 *****************************************************************************/
int xdev_check_hndl(const char *fname, struct pci_dev *pdev, unsigned long hndl)
{
	struct xlnx_dma_dev *xdev;

	if (!pdev)
		return -EINVAL;

	xdev = xdev_find_by_pdev(pdev);
	if (!xdev) {
		pr_info("%s pdev 0x%p, hndl 0x%lx, NO match found!\n",
			fname, pdev, hndl);
		return -EINVAL;
	}
	if (((unsigned long)xdev) != hndl) {
		pr_info("%s pdev 0x%p, hndl 0x%lx != 0x%p!\n",
			fname, pdev, hndl, xdev);
		return -EINVAL;
	}

	 if (xdev->conf.pdev != pdev) {
		pr_info("pci_dev(0x%lx) != pdev(0x%lx)\n",
				(unsigned long)xdev->conf.pdev,
				(unsigned long)pdev);
		return -EINVAL;
	}

	return 0;
}

/**********************************************************************
 * PCI-level Functions
 **********************************************************************/

/*****************************************************************************/
/**
 * xdev_unmap_bars() - Unmap the BAR regions that had been mapped
 *						earlier using map_bars()
 *
 * @param[in]	xdev:	pointer to current xdev
 * @param[in]	pdev:	pointer to struct pci_dev
 *
 * @return	none
 *****************************************************************************/
static void xdev_unmap_bars(struct xlnx_dma_dev *xdev, struct pci_dev *pdev)
{
	if (xdev->regs) {
		/* unmap BAR */
		pci_iounmap(pdev, xdev->regs);
		/* mark as unmapped */
		xdev->regs = NULL;
	}

	if (xdev->stm_regs) {
		pci_iounmap(pdev, xdev->stm_regs);
		xdev->stm_regs = NULL;
	}
}

/*****************************************************************************/
/**
 * xdev_map_bars() - map device regions into kernel virtual address space
 *						earlier using map_bars()
 *
 * @param[in]	xdev:	pointer to current xdev
 * @param[in]	pdev:	pointer to struct pci_dev
 *
 * Map the device memory regions into kernel virtual address space after
 * verifying their sizes respect the minimum sizes needed
 *
 * @return	length of the bar on success
 * @return	0 on failure
 *****************************************************************************/
static int xdev_map_bars(struct xlnx_dma_dev *xdev, struct pci_dev *pdev)
{
	int map_len;

	/* QDMA: hard code the dma config bar to be 0 */
	xdev->conf.bar_num_config = QDMA_CONFIG_BAR;

	map_len = pci_resource_len(pdev, QDMA_CONFIG_BAR);
	if (map_len > QDMA_MAX_BAR_LEN_MAPPED)
		map_len = QDMA_MAX_BAR_LEN_MAPPED;

	xdev->regs = pci_iomap(pdev, QDMA_CONFIG_BAR, map_len);
	if (!xdev->regs) {
		pr_err("%s unable to map config bar %d.\n", xdev->conf.name,
			QDMA_CONFIG_BAR);
		return -EINVAL;
	}

#ifndef __QDMA_VF__
	{
		/* check if it's dma control BAR */
		u32 id = readl(xdev->regs);

		if ((id & 0xFFFF0000) != 0x1FD30000) {
			pr_info("%s: NO QDMA config bar found, id 0x%x.\n",
				xdev->conf.name, id);

			/* unwind; unmap any BARs that we did map */
			xdev_unmap_bars(xdev, pdev);
			return -EINVAL;
		}
	}
#endif

	if (pdev->device == STM_ENABLED_DEVICE) {
		u32 rev;

		map_len = pci_resource_len(pdev, STM_BAR);
		xdev->stm_regs = pci_iomap(pdev, STM_BAR, map_len);
		if (!xdev->stm_regs) {
			pr_warn("%s unable to map bar %d.\n",
				xdev->conf.name, STM_BAR);
			return -EINVAL;
		}

		rev = readl(xdev->stm_regs + STM_REG_BASE + STM_REG_REV);
		if (!(((rev >> 24) == 'S') && (((rev >> 16) & 0xFF) == 'T') &&
		      (((rev >> 8) & 0xFF) == 'M') &&
		      ((rev & 0xFF) <= STM_SUPPORTED_REV))) {
			pr_err("%s: Unsupported STM Rev found, rev 0x%x\n",
			       xdev->conf.name, rev);
			xdev_unmap_bars(xdev, pdev);
			return -EINVAL;
		}
		xdev->stm_en = 1;
		xdev->stm_rev = rev & 0xFF;
	} else {
		xdev->stm_en = 0;
	}

	return 0;
}

/*****************************************************************************/
/**
 * xdev_map_bars() - allocate the dma device
 *
 * @param[in]	conf:	qdma device configuration
 *
 *
 * @return	pointer to dma device
 * @return	NULL on failure
 *****************************************************************************/
static struct xlnx_dma_dev *xdev_alloc(struct qdma_dev_conf *conf)
{
	struct xlnx_dma_dev *xdev;

	/* allocate zeroed device book keeping structure */
	xdev = kzalloc(sizeof(struct xlnx_dma_dev), GFP_KERNEL);
	if (!xdev) {
		pr_info("OOM, xlnx_dma_dev.\n");
		return NULL;
	}
	spin_lock_init(&xdev->hw_prg_lock);
	spin_lock_init(&xdev->lock);

	/* create a driver to device reference */
	memcpy(&xdev->conf, conf, sizeof(*conf));

	/* !! FIXME default to eanbled for everything */
	xdev->flr_prsnt = 1;
	xdev->st_mode_en = 1;
	xdev->mm_mode_en = 1;
	xdev->mm_channel_max = 1;

	return xdev;
}

/*****************************************************************************/
/**
 * pci_dma_mask_set() - check the pci capability of the dma device
 *
 * @param[in]	pdev:	pointer to struct pci_dev
 *
 *
 * @return	0: on success
 * @return	<0: on failure
 *****************************************************************************/
static int pci_dma_mask_set(struct pci_dev *pdev)
{
	/** 64-bit addressing capability for XDMA? */
	if (!pci_set_dma_mask(pdev, DMA_BIT_MASK(64))) {
		/** use 32-bit DMA for descriptors */
		pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
		/** use 64-bit DMA, 32-bit for consistent */
	} else if (!pci_set_dma_mask(pdev, DMA_BIT_MASK(32))) {
		pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
		/** use 32-bit DMA */
		dev_info(&pdev->dev, "Using a 32-bit DMA mask.\n");
	} else {
		/** use 32-bit DMA */
		dev_info(&pdev->dev, "No suitable DMA possible.\n");
		return -EINVAL;
	}

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static void pci_enable_relaxed_ordering(struct pci_dev *pdev)
{
	pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, PCI_EXP_DEVCTL_RELAX_EN);
}
#else
static void pci_enable_relaxed_ordering(struct pci_dev *pdev)
{
	u16 v;
	int pos;

	pos = pci_pcie_cap(pdev);
	if (pos > 0) {
		pci_read_config_word(pdev, pos + PCI_EXP_DEVCTL, &v);
		v |= PCI_EXP_DEVCTL_RELAX_EN;
		pci_write_config_word(pdev, pos + PCI_EXP_DEVCTL, v);
	}
}
#endif

/*****************************************************************************/
/**
 * qdma_device_offline() - set the dma device in offline mode
 *
 * @param[in]	pdev:		pointer to struct pci_dev
 * @param[in]	dev_hndl:	device handle
 *
 *
 * @return	none
 *****************************************************************************/
void qdma_device_offline(struct pci_dev *pdev, unsigned long dev_hndl)
{
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)dev_hndl;

	if (!dev_hndl)
		return;

	if (xdev_check_hndl(__func__, pdev, dev_hndl) < 0)
		return;

	if (xdev->conf.pdev != pdev) {
		pr_info("pci_dev(0x%lx) != pdev(0x%lx)\n",
			(unsigned long)xdev->conf.pdev, (unsigned long)pdev);
	}

	xdev_flag_set(xdev, XDEV_FLAG_OFFLINE);

#ifdef __QDMA_VF__
	xdev_sriov_vf_offline(xdev, 0);
#elif defined(CONFIG_PCI_IOV)
	xdev_sriov_disable(xdev);
#endif

	qdma_device_cleanup(xdev);

	qdma_mbox_cleanup(xdev);
}

/*****************************************************************************/
/**
 * qdma_device_online() - set the dma device in online mode
 *
 * @param[in]	pdev:		pointer to struct pci_dev
 * @param[in]	dev_hndl:	device handle
 *
 *
 * @return	0: on success
 * @return	<0: on failure
 *****************************************************************************/
int qdma_device_online(struct pci_dev *pdev, unsigned long dev_hndl)
{
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)dev_hndl;
	int rv;

	if (!dev_hndl)
		return -EINVAL;

	if (xdev_check_hndl(__func__, pdev, dev_hndl) < 0)
		return -EINVAL;

	if (xdev->conf.pdev != pdev) {
		pr_info("pci_dev(0x%lx) != pdev(0x%lx)\n",
			(unsigned long)xdev->conf.pdev, (unsigned long)pdev);
	}

	rv = qdma_device_init(xdev);
	if (rv < 0) {
		pr_warn("qdma_init failed %d.\n", rv);
		goto cleanup_qdma;
	}
	xdev_flag_clear(xdev, XDEV_FLAG_OFFLINE);
	qdma_mbox_init(xdev);
#ifdef __QDMA_VF__
	/* PF mbox will start when vf > 0 */
	qdma_mbox_start(xdev);
	rv = xdev_sriov_vf_online(xdev, 0);
	if (rv < 0)
		goto cleanup_qdma;
#elif defined(CONFIG_PCI_IOV)
	if (xdev->conf.vf_max) {
		rv = xdev_sriov_enable(xdev, xdev->conf.vf_max);
		if (rv < 0)
			goto cleanup_qdma;
	}
#endif
	return 0;

cleanup_qdma:
	qdma_device_cleanup(xdev);
	return rv;
}

/*****************************************************************************/
/**
 * qdma_device_open() - open the dma device
 *
 * @param[in]	mod_name:	name of the dma device
 * @param[in]	conf:		device configuration
 * @param[in]	dev_hndl:	device handle
 *
 *
 * @return	0: on success
 * @return	<0: on failure
 *****************************************************************************/
int qdma_device_open(const char *mod_name, struct qdma_dev_conf *conf,
			unsigned long *dev_hndl)
{
	struct pci_dev *pdev = conf->pdev;
	struct xlnx_dma_dev *xdev = NULL;
	int rv = 0;

	*dev_hndl = 0UL;

	if (!mod_name) {
		pr_info("%s: mod_name is NULL.\n", __func__);
		return QDMA_ERR_INVALID_INPUT_PARAM;
	}

	if (!conf) {
		pr_info("%s: queue_conf is NULL.\n", mod_name);
		return QDMA_ERR_INVALID_INPUT_PARAM;
	}

	if (!pdev) {
		pr_info("%s: pci device NULL.\n", mod_name);
		return QDMA_ERR_INVALID_PCI_DEV;
	}

	conf->bar_num_config = -1;
	conf->bar_num_user = -1;

	pr_info("%s, %02x:%02x.%02x, pdev 0x%p, 0x%x:0x%x.\n",
		mod_name, pdev->bus->number, PCI_SLOT(pdev->devfn),
		PCI_FUNC(pdev->devfn), pdev, pdev->vendor, pdev->device);

	xdev = xdev_find_by_pdev(pdev);
	if (xdev) {
		pr_warn("%s, device %s already attached!\n",
			mod_name, dev_name(&pdev->dev));
		return QDMA_ERR_PCI_DEVICE_ALREADY_ATTACHED;
	}

	rv = pci_request_regions(pdev, mod_name);
	if (rv) {
		/* Just info, some other driver may have claimed the device. */
		dev_info(&pdev->dev, "cannot obtain PCI resources\n");
		return rv;
	}

	rv = pci_enable_device(pdev);
	if (rv) {
		dev_err(&pdev->dev, "cannot enable PCI device\n");
		goto release_regions;
	}

	/* enable relaxed ordering */
	pci_enable_relaxed_ordering(pdev);

	/* enable bus master capability */
	pci_set_master(pdev);

	rv = pci_dma_mask_set(pdev);
	if (rv)
		goto disable_device;

	/* allocate zeroed device book keeping structure */
	xdev = xdev_alloc(conf);
	if (!xdev)
		goto disable_device;

	strncpy(xdev->mod_name, mod_name, QDMA_DEV_NAME_MAXLEN - 1);

	xdev_flag_set(xdev, XDEV_FLAG_OFFLINE);
	xdev_list_add(xdev);

	rv = sprintf(xdev->conf.name, "qdma%05x-p%s",
		xdev->conf.bdf, dev_name(&xdev->conf.pdev->dev));
	xdev->conf.name[rv] = '\0';

	rv = xdev_map_bars(xdev, pdev);
	if (rv)
		goto unmap_bars;

	/* program STM port map */
	if (xdev->stm_en) {
		u32 v = readl(xdev->stm_regs + STM_REG_BASE +
			      STM_REG_H2C_MODE);
		v &= 0x0000FFFF;
		v |= (STM_PORT_MAP << 16);
		writel(v, xdev->stm_regs + STM_REG_BASE + STM_REG_H2C_MODE);
	}

#ifndef __QDMA_VF__
	/* get the device attributes */
	qdma_device_attributes_get(xdev);

	if (!xdev->mm_mode_en && !xdev->st_mode_en) {
		pr_info("None of the modes ( ST or MM) are enabled\n");
		rv = QDMA_ERR_INTERFACE_NOT_ENABLED_IN_DEVICE;
		goto unmap_bars;
	}
#endif

	memcpy(conf, &xdev->conf, sizeof(*conf));

	rv = qdma_device_online(pdev, (unsigned long)xdev);
	if (rv < 0)
		goto cleanup_qdma;

	pr_info("%s, %05x, pdev 0x%p, xdev 0x%p, ch %u, q %u, vf %u.\n",
		dev_name(&pdev->dev), xdev->conf.bdf, pdev, xdev,
		xdev->mm_channel_max, conf->qsets_max, conf->vf_max);

	*dev_hndl = (unsigned long)xdev;

	return QDMA_OPERATION_SUCCESSFUL;

cleanup_qdma:
	qdma_device_offline(pdev, (unsigned long)xdev);

unmap_bars:
	xdev_unmap_bars(xdev, pdev);

	xdev_list_remove(xdev);
	kfree(xdev);

disable_device:
	pci_disable_device(pdev);

release_regions:
	pci_release_regions(pdev);

	return rv;
}

/*****************************************************************************/
/**
 * qdma_device_close() - close the dma device
 *
 * @param[in]	pdev:		pointer to struct pci_dev
 * @param[in]	dev_hndl:	device handle
 *
 *
 * @return	none
 *****************************************************************************/
void qdma_device_close(struct pci_dev *pdev, unsigned long dev_hndl)
{
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)dev_hndl;

	if (!dev_hndl)
		return;

	if (xdev_check_hndl(__func__, pdev, dev_hndl) < 0)
		return;

	if (xdev->conf.pdev != pdev) {
		pr_info("pci_dev(0x%lx) != pdev(0x%lx)\n",
			(unsigned long)xdev->conf.pdev, (unsigned long)pdev);
	}

	qdma_device_offline(pdev, dev_hndl);

	xdev_unmap_bars(xdev, pdev);

	pci_release_regions(pdev);
	pci_disable_device(pdev);

	xdev_list_remove(xdev);

	kfree(xdev);
}

/*****************************************************************************/
/**
 * qdma_device_get_config() - get the device configuration
 *
 * @param[in]	dev_hndl:	device handle
 * @param[out]	conf:		dma device configuration
 * @param[out]	buf, buflen:
 *			error message buffer, can be NULL/0 (i.e., optional)
 *
 *
 * @return	none
 *****************************************************************************/
int qdma_device_get_config(unsigned long dev_hndl, struct qdma_dev_conf *conf,
					char *buf, int buflen)
{
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)dev_hndl;

	if (xdev_check_hndl(__func__, xdev->conf.pdev, dev_hndl) < 0)
		return -EINVAL;

	memcpy(conf, &xdev->conf, sizeof(*conf));
	return 0;
}

/*****************************************************************************/
/**
 * qdma_device_set_config() - set the device configuration
 *
 * @param[in]	dev_hndl:	device handle
 * @param[in]	conf:		dma device configuration to set
 *
 * @return	0 on success ,<0 on failure
 *****************************************************************************/
int qdma_device_set_config(unsigned long dev_hndl, struct qdma_dev_conf *conf)
{
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)dev_hndl;

	if (!conf)
		return -EINVAL;

	if (xdev_check_hndl(__func__, xdev->conf.pdev, dev_hndl) < 0)
		return -EINVAL;

	memcpy(&xdev->conf, conf, sizeof(*conf));

	return 0;
}

/*****************************************************************************/
/**
 * qdma_device_set_cfg_state - set the device configuration state
 *
 * @param[in]	dev_hndl:	device handle
 * @param[in]	new_cfg_state:	dma device conf state to set
 *
 *
 * @return	0 on success ,<0 on failure
 *****************************************************************************/

int qdma_device_set_cfg_state(unsigned long dev_hndl, enum cfg_state new_cfg_state)
{
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)dev_hndl;

	if (new_cfg_state > CFG_USER)
		return -EINVAL;

	if (xdev_check_hndl(__func__, xdev->conf.pdev, dev_hndl) < 0)
		return -EINVAL;

	xdev->conf.cur_cfg_state = new_cfg_state;

	return 0;
}


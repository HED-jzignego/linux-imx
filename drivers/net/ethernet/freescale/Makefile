# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Freescale network device drivers.
#

obj-$(CONFIG_FEC) += fec.o
fec-objs :=fec_main.o fec_fixup.o fec_ptp.o eth_mac.o

obj-$(CONFIG_FEC_MPC52xx) += fec_mpc52xx.o
ifeq ($(CONFIG_FEC_MPC52xx_MDIO),y)
	obj-$(CONFIG_FEC_MPC52xx) += fec_mpc52xx_phy.o
endif
obj-$(CONFIG_FS_ENET) += fs_enet/
obj-$(CONFIG_FSL_PQ_MDIO) += fsl_pq_mdio.o
obj-$(CONFIG_FSL_XGMAC_MDIO) += xgmac_mdio.o
obj-$(CONFIG_GIANFAR) += gianfar_driver.o
obj-$(CONFIG_PTP_1588_CLOCK_GIANFAR) += gianfar_ptp.o
gianfar_driver-objs := gianfar.o \
		gianfar_ethtool.o
obj-$(CONFIG_UCC_GETH) += ucc_geth_driver.o
ucc_geth_driver-objs := ucc_geth.o ucc_geth_ethtool.o

obj-$(CONFIG_FSL_FMAN) += fman/
obj-$(CONFIG_FSL_DPAA_ETH) += dpaa/

/* linux/arch/arm/mach-xxxx/board-u1-spr-modem.c
 * Copyright (C) 2010 Samsung Electronics. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/interrupt.h>

/* inlcude platform specific file */
#include <linux/platform_data/modem.h>
#include <mach/gpio.h>
#include <plat/gpio-cfg.h>

#define IDPRAM_SIZE	0x4000
#define IDPRAM_PHY_START	0x13A00000
#define IDPRAM_PHY_END (DPRAM_PHY_START + DPRAM_SIZE)

#define INT_MASK_REQ_ACK_F	0x0020
#define INT_MASK_REQ_ACK_R	0x0010
#define INT_MASK_RES_ACK_F	0x0008
#define INT_MASK_RES_ACK_R	0x0004
#define INT_MASK_SEND_F		0x0002
#define INT_MASK_SEND_R		0x0001

#define INT_MASK_REQ_ACK_RFS	0x0400 /* Request RES_ACK_RFS		*/
#define INT_MASK_RES_ACK_RFS	0x0200 /* Response of REQ_ACK_RFS	*/
#define INT_MASK_SEND_RFS	0x0100 /* Indicate sending RFS data	*/

/*S5PV210 Interanl Dpram Special Function Register*/
#define IDPRAM_MIFCON_INT2APEN      (1<<2)
#define IDPRAM_MIFCON_INT2MSMEN     (1<<3)
#define IDPRAM_MIFCON_DMATXREQEN_0  (1<<16)
#define IDPRAM_MIFCON_DMATXREQEN_1  (1<<17)
#define IDPRAM_MIFCON_DMARXREQEN_0  (1<<18)
#define IDPRAM_MIFCON_DMARXREQEN_1  (1<<19)
#define IDPRAM_MIFCON_FIXBIT        (1<<20)

#define IDPRAM_MIFPCON_ADM_MODE     (1<<6) /* mux / demux mode  */

#define IDPRAM_DMA_ADR_MASK         0x3FFF
#define IDPRAM_DMA_TX_ADR_0         /* shift 0 */
#define IDPRAM_DMA_TX_ADR_1         /* shift 16  */
#define IDPRAM_DMA_RX_ADR_0         /* shift 0  */
#define IDPRAM_DMA_RX_ADR_1         /* shift 16  */

#define IDPRAM_SFR_PHYSICAL_ADDR 0x13A08000
#define IDPRAM_SFR_SIZE 0x1C

/*#define IDPRAM_ADDRESS_DEMUX*/

static int __init init_modem(void);

static void qsc_idpram_reset(void);
static void qsc_idpram_clr_intr(void);
static u16  qsc_idpram_recv_intr(void);
static void qsc_idpram_send_intr(u16 irq_mask);
static u16  qsc_idpram_recv_msg(void);
static void qsc_idpram_send_msg(u16 msg);

static u16  qsc_idpram_get_magic(void);
static void qsc_idpram_set_magic(u16 value);
static u16  qsc_idpram_get_access(void);
static void qsc_idpram_set_access(u16 value);

static u32  qsc_idpram_get_tx_head(int dev_id);
static u32  qsc_idpram_get_tx_tail(int dev_id);
static void qsc_idpram_set_tx_head(int dev_id, u32 head);
static void qsc_idpram_set_tx_tail(int dev_id, u32 tail);
static u8 __iomem *qsc_idpram_get_tx_buff(int dev_id);
static u32  qsc_idpram_get_tx_buff_size(int dev_id);

static u32  qsc_idpram_get_rx_head(int dev_id);
static u32  qsc_idpram_get_rx_tail(int dev_id);
static void qsc_idpram_set_rx_head(int dev_id, u32 head);
static void qsc_idpram_set_rx_tail(int dev_id, u32 tail);
static u8 __iomem *qsc_idpram_get_rx_buff(int dev_id);
static u32  qsc_idpram_get_rx_buff_size(int dev_id);

static u16  qsc_idpram_get_mask_req_ack(int dev_id);
static u16  qsc_idpram_get_mask_res_ack(int dev_id);
static u16  qsc_idpram_get_mask_send(int dev_id);

static void qsc_idpram_set_sleep_cfg(u16 flag);

struct idpram_sfr_reg {
	unsigned int2ap;
	unsigned int2msm;
	unsigned mifcon;
	unsigned mifpcon;
	unsigned msmintclr;
	unsigned dma_tx_adr;
	unsigned dma_rx_adr;
};

/*S5PV210 Internal Dpram GPIO table*/
struct idpram_gpio_data {
	unsigned num;
	unsigned cfg;
	unsigned pud;
	unsigned val;
};

static volatile void __iomem *s5pv310_dpram_sfr_va;


/*
	magic_code +
	access_enable +
	fmt_tx_head + fmt_tx_tail + fmt_tx_buff +
	raw_tx_head + raw_tx_tail + raw_tx_buff +
	fmt_rx_head + fmt_rx_tail + fmt_rx_buff +
	raw_rx_head + raw_rx_tail + raw_rx_buff +
	padding +
	mbx_cp2ap +
	mbx_ap2cp
 =	2 +
	2 +
	2 + 2 + 2044 +
	2 + 2 + 6128 +
	2 + 2 + 2044 +
	2 + 2 + 6128 +
	16 +
	2 +
	2
 =	16384
*/

#define QSC_DP_FMT_TX_BUFF_SZ	1020
#define QSC_DP_RAW_TX_BUFF_SZ	7160
#define QSC_DP_FMT_RX_BUFF_SZ	1020
#define QSC_DP_RAW_RX_BUFF_SZ	7160

#define MAX_QSC_IDPRAM_IPC_DEV	(IPC_RAW + 1)	/* FMT, RAW */

struct qsc_idpram_ipc_cfg {
	u16 magic;
	u16 access;

	u16 fmt_tx_head;
	u16 fmt_tx_tail;
	u8  fmt_tx_buff[QSC_DP_FMT_TX_BUFF_SZ];

	u16 raw_tx_head;
	u16 raw_tx_tail;
	u8  raw_tx_buff[QSC_DP_RAW_TX_BUFF_SZ];

	u16 fmt_rx_head;
	u16 fmt_rx_tail;
	u8  fmt_rx_buff[QSC_DP_FMT_RX_BUFF_SZ];

	u16 raw_rx_head;
	u16 raw_rx_tail;
	u8  raw_rx_buff[QSC_DP_RAW_RX_BUFF_SZ];

	u16 mbx_ap2cp; /*0x3FFC*/
	u16 mbx_cp2ap; /*0x3FFE*/
};

struct qsc_idpram_circ {
	u16 __iomem *head;
	u16 __iomem *tail;
	u8  __iomem *buff;
	u32          size;
};

struct qsc_idpram_ipc_device {
	char name[16];
	int  id;

	struct qsc_idpram_circ txq;
	struct qsc_idpram_circ rxq;

	u16 mask_req_ack;
	u16 mask_res_ack;
	u16 mask_send;
};

struct qsc_idpram_ipc_map {
	u16 __iomem *magic;
	u16 __iomem *access;

	struct qsc_idpram_ipc_device dev[MAX_QSC_IDPRAM_IPC_DEV];

	u16 __iomem *mbx_ap2cp;
	u16 __iomem *mbx_cp2ap;
};

static struct qsc_idpram_ipc_map qsc_ipc_map;

static struct modemlink_dpram_control qsc_idpram_ctrl = {
	.reset      = qsc_idpram_reset,

	.clear_intr = qsc_idpram_clr_intr,
	.recv_intr  = qsc_idpram_recv_intr,
	.send_intr  = qsc_idpram_send_intr,
	.recv_msg   = qsc_idpram_recv_msg,
	.send_msg   = qsc_idpram_send_msg,

	.get_magic  = qsc_idpram_get_magic,
	.set_magic  = qsc_idpram_set_magic,
	.get_access = qsc_idpram_get_access,
	.set_access = qsc_idpram_set_access,

	.get_tx_head = qsc_idpram_get_tx_head,
	.get_tx_tail = qsc_idpram_get_tx_tail,
	.set_tx_head = qsc_idpram_set_tx_head,
	.set_tx_tail = qsc_idpram_set_tx_tail,
	.get_tx_buff = qsc_idpram_get_tx_buff,
	.get_tx_buff_size = qsc_idpram_get_tx_buff_size,

	.get_rx_head = qsc_idpram_get_rx_head,
	.get_rx_tail = qsc_idpram_get_rx_tail,
	.set_rx_head = qsc_idpram_set_rx_head,
	.set_rx_tail = qsc_idpram_set_rx_tail,
	.get_rx_buff = qsc_idpram_get_rx_buff,
	.get_rx_buff_size = qsc_idpram_get_rx_buff_size,

	.get_mask_req_ack = qsc_idpram_get_mask_req_ack,
	.get_mask_res_ack = qsc_idpram_get_mask_res_ack,
	.get_mask_send    = qsc_idpram_get_mask_send,

	.set_sleep_cfg = qsc_idpram_set_sleep_cfg,

	.dp_base = NULL,
	.dp_size = 0,
	.dp_type = AP_IDPRAM,

	.dpram_irq        = IRQ_MODEM_IF,
	.dpram_irq_flags  = IRQF_DISABLED,
	.dpram_irq_name   = "QSC6085_IDPRAM_IRQ",
	.dpram_wlock_name = "QSC6085_IDPRAM_WLOCK",

	.max_ipc_dev = MAX_QSC_IDPRAM_IPC_DEV,
};

/*
** CDMA target platform data
*/
static struct modem_io_t cdma_io_devices[] = {
	[0] = {
		.name = "cdma_boot0",
		.id = 0x1,
		.format = IPC_BOOT,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[1] = {
		.name = "cdma_ipc0",
		.id = 0x1,
		.format = IPC_FMT,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[2] = {
		.name = "cdma_rfs0",
		.id = 0x33,		/* 0x13 (ch.id) | 0x20 (mask) */
		.format = IPC_RAW,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[3] = {
		.name = "cdma_multipdp",
		.id = 0x1,
		.format = IPC_MULTI_RAW,
		.io_type = IODEV_DUMMY,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[4] = {
		.name = "cdma_csd0",
		.id = 0x21,
		.format = IPC_RAW,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[5] = {
		.name = "cdma_cdma0",
		.id = 0x27,
		.format = IPC_RAW,
		.io_type = IODEV_NET,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[6] = {
		.name = "cdma_trfb0",
		.id = 0x29,
		.format = IPC_RAW,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[7] = {
		.name = "cdma_ciq0",
		.id = 0x47,
		.format = IPC_RAW,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[8] = {
		.name = "cdma_cplog0",
		.id = 0x49,
		.format = IPC_RAW,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[9] = {
		.name = "cdma_rmnet5", /* DM Port IO device */
		.id = 0x3A,
		.format = IPC_RAW,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[10] = {
		.name = "cdma_rmnet6", /* AT CMD IO device */
		.id = 0x31,
		.format = IPC_RAW,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
	[11] = {
		.name = "cdma_ramdump0",
		.id = 0x1,
		.format = IPC_RAMDUMP,
		.io_type = IODEV_MISC,
		.links = LINKTYPE(LINKDEV_DPRAM),
	},
};

static struct modem_data cdma_modem_data = {
	.name = "qsc6085",

	.gpio_cp_on        = GPIO_QSC_PHONE_ON,
	.gpio_cp_reset     = GPIO_QSC_PHONE_RST,
	.gpio_pda_active   = GPIO_PDA_ACTIVE,
	.gpio_phone_active = GPIO_QSC_PHONE_ACTIVE,
	.gpio_host_wakeup = GPIO_C210_DPRAM_INT_N,
	.gpio_cp_dump_int = GPIO_CP_DUMP_INT,

	.modem_net  = CDMA_NETWORK,
	.modem_type = QC_QSC6085,
	.link_types = LINKTYPE(LINKDEV_DPRAM),
	.link_name  = "qsc6085_idpram",
	.dpram_ctl  = &qsc_idpram_ctrl,

	.num_iodevs = ARRAY_SIZE(cdma_io_devices),
	.iodevs     = cdma_io_devices,

	.use_handover = false,

	.ipc_version = SIPC_VER_40,
};

static struct resource cdma_modem_res[] = {
	[0] = {
		.name  = "phone_active_irq",
		.flags = IORESOURCE_IRQ,
	},
};

static struct platform_device cdma_modem = {
	.name = "modem_if",
	.id = -1,
	.num_resources = ARRAY_SIZE(cdma_modem_res),
	.resource = cdma_modem_res,
	.dev = {
		.platform_data = &cdma_modem_data,
	},
};

static struct idpram_gpio_data idpram_gpio_address[] = {
#ifdef IDPRAM_ADDRESS_DEMUX
	{
		.num = EXYNOS4210_GPE1(0),	/* MSM_ADDR 0 -12 */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE1(1),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE1(2),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE1(3),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE1(4),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE1(5),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE1(6),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE1(7),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE2(0),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE2(1),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE2(2),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE2(3),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE2(4),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE2(5),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	},
#endif
};

static struct idpram_gpio_data idpram_gpio_data[] = {
	{
		.num = EXYNOS4210_GPE3(0), /* MSM_DATA 0 - 15 */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE3(1),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE3(2),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE3(3),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE3(4),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE3(5),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE3(6),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE3(7),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE4(0),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE4(1),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE4(2),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE4(3),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE4(4),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE4(5),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE4(6),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE4(7),
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	},
};

static struct idpram_gpio_data idpram_gpio_init_control[] = {
	{
		.num = EXYNOS4210_GPE0(1), /* MDM_CSn */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE0(0), /* MDM_WEn */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE0(2), /* MDM_Rn */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE0(3), /* MDM_IRQn */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_UP,
	},
#ifndef IDPRAM_ADDRESS_DEMUX
	{
		.num = EXYNOS4210_GPE0(4), /* MDM_ADVN */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	},
#endif
};

static struct idpram_gpio_data idpram_gpio_deinit_control[] = {
	{
		.num = EXYNOS4210_GPE0(1), /* MDM_CSn */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE0(0), /* MDM_WEn */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE0(2), /* MDM_Rn */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}, {
		.num = EXYNOS4210_GPE0(3), /* MDM_IRQn */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	},
#ifndef IDPRAM_ADDRESS_DEMUX
	{
		.num = EXYNOS4210_GPE0(4), /* MDM_ADVN */
		.cfg = S3C_GPIO_SFN(0x2),
		.pud = S3C_GPIO_PULL_NONE,
	}
#endif
};


static void idpram_gpio_cfg(struct idpram_gpio_data *gpio)
{
	printk(KERN_DEBUG "idpram set gpio num=%d, cfg=0x%x, pud=%d, val=%d\n",
		gpio->num, gpio->cfg, gpio->pud, gpio->val);

	s3c_gpio_cfgpin(gpio->num, gpio->cfg);
	s3c_gpio_setpull(gpio->num, gpio->pud);
	if (gpio->val)
		gpio_set_value(gpio->num, gpio->val);
}

static void idpram_gpio_init(void)
{
	int i;

#ifdef IDPRAM_ADDRESS_DEMUX
	for (i = 0; i < ARRAY_SIZE(idpram_gpio_address); i++)
		idpram_gpio_cfg(&idpram_gpio_address[i]);
#endif

	for (i = 0; i < ARRAY_SIZE(idpram_gpio_data); i++)
		idpram_gpio_cfg(&idpram_gpio_data[i]);

	for (i = 0; i < ARRAY_SIZE(idpram_gpio_init_control); i++)
		idpram_gpio_cfg(&idpram_gpio_init_control[i]);
}

static int idpram_sfr_init(void)
{
	volatile struct idpram_sfr_reg __iomem *sfr;
	struct clk *clk;

	/* enable internal dpram clock */
	clk = clk_get(NULL, "modem");
	if (!clk) {
		printk(KERN_ERR  "idpram failed to get clock %s\n", __func__);
		return -EFAULT;
	}
	clk_enable(clk);

	if (!s5pv310_dpram_sfr_va) {
		s5pv310_dpram_sfr_va =
			(volatile struct idpram_sfr_reg __iomem *)
		ioremap_nocache(IDPRAM_SFR_PHYSICAL_ADDR, IDPRAM_SFR_SIZE);
		if (!s5pv310_dpram_sfr_va) {
			printk(KERN_ERR "idpram_sfr_base io-remap fail\n");
			/*iounmap(idpram_base);*/
			return -EFAULT;
		}
	}
	sfr = s5pv310_dpram_sfr_va;

	sfr->mifcon = (IDPRAM_MIFCON_FIXBIT | IDPRAM_MIFCON_INT2APEN |
		IDPRAM_MIFCON_INT2MSMEN);
#ifndef IDPRAM_ADDRESS_DEMUX
	sfr->mifpcon = (IDPRAM_MIFPCON_ADM_MODE);
#endif
	return 0;
}

static void qsc_idpram_reset(void)
{
	return;
}

static void qsc_idpram_clr_intr(void)
{
	volatile struct idpram_sfr_reg __iomem *sfr = s5pv310_dpram_sfr_va;
	sfr->msmintclr = 0xFF;
}

static u16 qsc_idpram_recv_intr(void)
{
	return ioread16(qsc_ipc_map.mbx_cp2ap);
}

static void qsc_idpram_send_intr(u16 irq_mask)
{
	iowrite16(irq_mask, qsc_ipc_map.mbx_ap2cp);
}

static u16 qsc_idpram_recv_msg(void)
{
	return ioread16(qsc_ipc_map.mbx_cp2ap);
}

static void qsc_idpram_send_msg(u16 msg)
{
	iowrite16(msg, qsc_ipc_map.mbx_ap2cp);
}

static u16 qsc_idpram_get_magic(void)
{
	return ioread16(qsc_ipc_map.magic);
}

static void qsc_idpram_set_magic(u16 value)
{
	iowrite16(value, qsc_ipc_map.magic);
}

static u16 qsc_idpram_get_access(void)
{
	return ioread16(qsc_ipc_map.access);
}

static void qsc_idpram_set_access(u16 value)
{
	iowrite16(value, qsc_ipc_map.access);
}

static u32 qsc_idpram_get_tx_head(int dev_id)
{
	return ioread16(qsc_ipc_map.dev[dev_id].txq.head);
}

static u32 qsc_idpram_get_tx_tail(int dev_id)
{
	return ioread16(qsc_ipc_map.dev[dev_id].txq.tail);
}

static void qsc_idpram_set_tx_head(int dev_id, u32 head)
{
	iowrite16((u16)head, qsc_ipc_map.dev[dev_id].txq.head);
}

static void qsc_idpram_set_tx_tail(int dev_id, u32 tail)
{
	iowrite16((u16)tail, qsc_ipc_map.dev[dev_id].txq.tail);
}

static u8 __iomem *qsc_idpram_get_tx_buff(int dev_id)
{
	return qsc_ipc_map.dev[dev_id].txq.buff;
}

static u32 qsc_idpram_get_tx_buff_size(int dev_id)
{
	return qsc_ipc_map.dev[dev_id].txq.size;
}

static u32 qsc_idpram_get_rx_head(int dev_id)
{
	return ioread16(qsc_ipc_map.dev[dev_id].rxq.head);
}

static u32 qsc_idpram_get_rx_tail(int dev_id)
{
	return ioread16(qsc_ipc_map.dev[dev_id].rxq.tail);
}

static void qsc_idpram_set_rx_head(int dev_id, u32 head)
{
	return iowrite16((u16)head, qsc_ipc_map.dev[dev_id].rxq.head);
}

static void qsc_idpram_set_rx_tail(int dev_id, u32 tail)
{
	return iowrite16((u16)tail, qsc_ipc_map.dev[dev_id].rxq.tail);
}

static u8 __iomem *qsc_idpram_get_rx_buff(int dev_id)
{
	return qsc_ipc_map.dev[dev_id].rxq.buff;
}

static u32 qsc_idpram_get_rx_buff_size(int dev_id)
{
	return qsc_ipc_map.dev[dev_id].rxq.size;
}

static u16 qsc_idpram_get_mask_req_ack(int dev_id)
{
	return qsc_ipc_map.dev[dev_id].mask_req_ack;
}

static u16 qsc_idpram_get_mask_res_ack(int dev_id)
{
	return qsc_ipc_map.dev[dev_id].mask_res_ack;
}

static u16 qsc_idpram_get_mask_send(int dev_id)
{
	return qsc_ipc_map.dev[dev_id].mask_send;
}

static void qsc_idpram_set_sleep_cfg(u16 flag)
{
	int i = 0;
	volatile struct idpram_sfr_reg __iomem *sfr = s5pv310_dpram_sfr_va;

	switch (flag) {
	case 0: /*suspend*/
		for (i = 0; i < ARRAY_SIZE(idpram_gpio_init_control); i++)
			idpram_gpio_cfg(&idpram_gpio_init_control[i]);
		s3c_gpio_cfgpin(idpram_gpio_init_control[3].num, 1);
		 /*mdm_IRQn sets output high.*/
		gpio_set_value(idpram_gpio_init_control[3].num, 1);
		break;
	case 1: /*resume*/
		sfr->mifcon = (IDPRAM_MIFCON_FIXBIT | IDPRAM_MIFCON_INT2APEN |
			IDPRAM_MIFCON_INT2MSMEN);
#ifndef IDPRAM_ADDRESS_DEMUX
		sfr->mifpcon = (IDPRAM_MIFPCON_ADM_MODE);
#endif
		for (i = 0; i < ARRAY_SIZE(idpram_gpio_init_control); i++)
			idpram_gpio_cfg(&idpram_gpio_init_control[i]);
		break;
	default:
		break;
	}
}

static void s5pv310_dpram_int_clear(void)
{
	volatile struct idpram_sfr_reg __iomem *sfr = s5pv310_dpram_sfr_va;
	sfr->msmintclr = 0xFF;
}

static void config_cdma_modem_gpio(void)
{
	int err;
	unsigned gpio_cp_on = cdma_modem_data.gpio_cp_on;
	unsigned gpio_cp_rst = cdma_modem_data.gpio_cp_reset;
	unsigned gpio_pda_active = cdma_modem_data.gpio_pda_active;
	unsigned gpio_phone_active = cdma_modem_data.gpio_phone_active;
	unsigned gpio_host_wakeup = cdma_modem_data.gpio_host_wakeup;
	unsigned gpio_cp_dump_int = cdma_modem_data.gpio_cp_dump_int;

	cdma_modem_res[0].start = gpio_to_irq(GPIO_QSC_PHONE_ACTIVE);
	cdma_modem_res[0].end = gpio_to_irq(GPIO_QSC_PHONE_ACTIVE);

	pr_info("[MDM] <%s>\n", __func__);

	if (gpio_cp_on) {
		err = gpio_request(gpio_cp_on, "QSC_ON");
		if (err) {
			pr_err("fail to request gpio %s\n", "QSC_ON");
		} else {
			gpio_direction_output(gpio_cp_on, 0);
			s3c_gpio_setpull(gpio_cp_on, S3C_GPIO_PULL_NONE);
		}
	}

	if (gpio_cp_rst) {
		err = gpio_request(gpio_cp_rst, "QSC_RST");
		if (err) {
			pr_err("fail to request gpio %s\n", "QSC_RST");
		} else {
			gpio_direction_output(gpio_cp_rst, 0);
			s3c_gpio_setpull(gpio_cp_rst, S3C_GPIO_PULL_NONE);
		}
	}

	if (gpio_pda_active) {
		err = gpio_request(gpio_pda_active, "PDA_ACTIVE");
		if (err) {
			pr_err("fail to request gpio %s\n", "PDA_ACTIVE");
		} else {
			gpio_direction_output(gpio_pda_active, 0);
			s3c_gpio_setpull(gpio_pda_active, S3C_GPIO_PULL_NONE);
		}
	}

	if (gpio_phone_active) {
		err = gpio_request(gpio_phone_active, "PHONE_ACTIVE");
		if (err) {
			pr_err("fail to request gpio %s\n", "PHONE_ACTIVE");
		} else {
			s3c_gpio_cfgpin(gpio_phone_active, S3C_GPIO_SFN(0xF));
			s3c_gpio_setpull(gpio_phone_active, S3C_GPIO_PULL_NONE);
		}
	}

	if (gpio_host_wakeup) {
		err = gpio_request(gpio_host_wakeup, "HOST_WAKEUP");
		if (err) {
			pr_err("fail to request gpio %s\n", "HOST_WAKEUP");
		} else {
			s3c_gpio_cfgpin(gpio_host_wakeup, S3C_GPIO_SFN(0xF));
			s3c_gpio_setpull(gpio_host_wakeup, S3C_GPIO_PULL_NONE);
		}
	}

	if (gpio_cp_dump_int) {
		err = gpio_request(gpio_cp_dump_int, "CP_DUMP_INT");
		if (err) {
			pr_err("fail to request gpio %s\n", "CP_DUMP_INT");
		} else {
			s3c_gpio_cfgpin(gpio_cp_dump_int, S3C_GPIO_SFN(0xF));
			s3c_gpio_setpull(gpio_cp_dump_int, S3C_GPIO_PULL_DOWN);
		}
	}
}

static u8 *qsc_idpram_remap_mem_region(void)
{
	int			      dp_addr = 0;
	int			      dp_size = 0;
	u8 __iomem                   *dp_base = NULL;
	struct qsc_idpram_ipc_cfg    *ipc_map = NULL;
	struct qsc_idpram_ipc_device *dev = NULL;

	dp_addr = IDPRAM_PHY_START;
	dp_size = IDPRAM_SIZE;
	dp_base = (u8 *)ioremap_nocache(dp_addr, dp_size);
	if (!dp_base) {
		pr_err("[MDM] <%s> dpram base ioremap fail\n", __func__);
		return NULL;
	}
	pr_info("[MDM] <%s> DPRAM VA=0x%08X\n", __func__, (int)dp_base);

	qsc_idpram_ctrl.dp_base = (u8 __iomem *)dp_base;
	qsc_idpram_ctrl.dp_size = dp_size;

	/* Map for IPC */
	ipc_map = (struct qsc_idpram_ipc_cfg *)dp_base;

	/* Magic code and access enable fields */
	qsc_ipc_map.magic  = (u16 __iomem *)&ipc_map->magic;
	qsc_ipc_map.access = (u16 __iomem *)&ipc_map->access;

	/* FMT */
	dev = &qsc_ipc_map.dev[IPC_FMT];

	strcpy(dev->name, "FMT");
	dev->id = IPC_FMT;

	dev->txq.head = (u16 __iomem *)&ipc_map->fmt_tx_head;
	dev->txq.tail = (u16 __iomem *)&ipc_map->fmt_tx_tail;
	dev->txq.buff = (u8 __iomem *)&ipc_map->fmt_tx_buff[0];
	dev->txq.size = QSC_DP_FMT_TX_BUFF_SZ;

	dev->rxq.head = (u16 __iomem *)&ipc_map->fmt_rx_head;
	dev->rxq.tail = (u16 __iomem *)&ipc_map->fmt_rx_tail;
	dev->rxq.buff = (u8 __iomem *)&ipc_map->fmt_rx_buff[0];
	dev->rxq.size = QSC_DP_FMT_RX_BUFF_SZ;

	dev->mask_req_ack = INT_MASK_REQ_ACK_F;
	dev->mask_res_ack = INT_MASK_RES_ACK_F;
	dev->mask_send    = INT_MASK_SEND_F;

	/* RAW */
	dev = &qsc_ipc_map.dev[IPC_RAW];

	strcpy(dev->name, "RAW");
	dev->id = IPC_RAW;

	dev->txq.head = (u16 __iomem *)&ipc_map->raw_tx_head;
	dev->txq.tail = (u16 __iomem *)&ipc_map->raw_tx_tail;
	dev->txq.buff = (u8 __iomem *)&ipc_map->raw_tx_buff[0];
	dev->txq.size = QSC_DP_RAW_TX_BUFF_SZ;

	dev->rxq.head = (u16 __iomem *)&ipc_map->raw_rx_head;
	dev->rxq.tail = (u16 __iomem *)&ipc_map->raw_rx_tail;
	dev->rxq.buff = (u8 __iomem *)&ipc_map->raw_rx_buff[0];
	dev->rxq.size = QSC_DP_RAW_RX_BUFF_SZ;

	dev->mask_req_ack = INT_MASK_REQ_ACK_R;
	dev->mask_res_ack = INT_MASK_RES_ACK_R;
	dev->mask_send    = INT_MASK_SEND_R;

	/* Mailboxes */
	qsc_ipc_map.mbx_ap2cp = (u16 __iomem *)&ipc_map->mbx_ap2cp;
	qsc_ipc_map.mbx_cp2ap = (u16 __iomem *)&ipc_map->mbx_cp2ap;

	return dp_base;
}


static int __init init_modem(void)
{
	pr_err("[MDM] <%s>\n", __func__);

	/* interanl dpram gpio configure */
	idpram_gpio_init();
	idpram_sfr_init();

	config_cdma_modem_gpio();

	if (!qsc_idpram_remap_mem_region())
		return -1;
	platform_device_register(&cdma_modem);
	return 0;
}
late_initcall(init_modem);
/*device_initcall(init_modem);*/

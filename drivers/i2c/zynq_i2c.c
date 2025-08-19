/*
 * Driver for the Zynq-7000 PS I2C controller
 * IP from Cadence (ID T-CS-PE-0007-100, Version R1p10f2)
 *
 * Author: Joe Hershberger <joe.hershberger@ni.com>
 * Copyright (c) 2012 Joe Hershberger.
 *
 * Copyright (c) 2012-2013 Xilinx, Michal Simek
 *
 * SPDX-License-Identifier:	GPL-2.0+
 *
 * NOTE: This driver should be converted to driver model before June 2017.
 * Please see doc/driver-model/i2c-howto.txt for instructions.
 */

#include <common.h>
#include <asm/io.h>
#include <i2c.h>
#include <linux/errno.h>
#include <asm/arch/hardware.h>

/* i2c register set */
struct zynq_i2c_registers {
	u32 control;
	u32 status;
	u32 address;
	u32 data;
	u32 interrupt_status;
	u32 transfer_size;
	u32 slave_mon_pause;
	u32 time_out;
	u32 interrupt_mask;
	u32 interrupt_enable;
	u32 interrupt_disable;
};

/* Control register fields */
#define ZYNQ_I2C_CONTROL_RW		0x00000001
#define ZYNQ_I2C_CONTROL_MS		0x00000002
#define ZYNQ_I2C_CONTROL_NEA		0x00000004
#define ZYNQ_I2C_CONTROL_ACKEN		0x00000008
#define ZYNQ_I2C_CONTROL_HOLD		0x00000010
#define ZYNQ_I2C_CONTROL_SLVMON		0x00000020
#define ZYNQ_I2C_CONTROL_CLR_FIFO	0x00000040
#define ZYNQ_I2C_CONTROL_DIV_B_SHIFT	8
#define ZYNQ_I2C_CONTROL_DIV_B_MASK	0x00003F00
#define ZYNQ_I2C_CONTROL_DIV_A_SHIFT	14
#define ZYNQ_I2C_CONTROL_DIV_A_MASK	0x0000C000

/* Status register values */
#define ZYNQ_I2C_STATUS_RXDV	0x00000020
#define ZYNQ_I2C_STATUS_TXDV	0x00000040
#define ZYNQ_I2C_STATUS_RXOVF	0x00000080
#define ZYNQ_I2C_STATUS_BA	0x00000100

/* Interrupt register fields */
#define ZYNQ_I2C_INTERRUPT_COMP		0x00000001
#define ZYNQ_I2C_INTERRUPT_DATA		0x00000002
#define ZYNQ_I2C_INTERRUPT_NACK		0x00000004
#define ZYNQ_I2C_INTERRUPT_TO		0x00000008
#define ZYNQ_I2C_INTERRUPT_SLVRDY	0x00000010
#define ZYNQ_I2C_INTERRUPT_RXOVF	0x00000020
#define ZYNQ_I2C_INTERRUPT_TXOVF	0x00000040
#define ZYNQ_I2C_INTERRUPT_RXUNF	0x00000080
#define ZYNQ_I2C_INTERRUPT_ARBLOST	0x00000200

#define ZYNQ_I2C_FIFO_DEPTH		16
#define ZYNQ_I2C_TRANSFERT_SIZE_MAX	255 /* Controller transfer limit */

#define ZYNQ_I2C_DIVA_MAX	4
#define ZYNQ_I2C_DIVB_MAX	64
#define ZYNQ_I2C_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

static struct zynq_i2c_registers *i2c_select(struct i2c_adapter *adap)
{
	return adap->hwadapnr ?
		/* Zynq PS I2C1 */
		(struct zynq_i2c_registers *)ZYNQ_I2C_BASEADDR1 :
		/* Zynq PS I2C0 */
		(struct zynq_i2c_registers *)ZYNQ_I2C_BASEADDR0;
}

#ifdef DEBUG
static void zynq_i2c_debug_status(struct zynq_i2c_registers *zynq_i2c)
{
	int int_status;
	int status;
	int_status = readl(&zynq_i2c->interrupt_status);

	status = readl(&zynq_i2c->status);
	if (int_status || status) {
		debug("Status: ");
		if (int_status & ZYNQ_I2C_INTERRUPT_COMP)
			debug("COMP ");
		if (int_status & ZYNQ_I2C_INTERRUPT_DATA)
			debug("DATA ");
		if (int_status & ZYNQ_I2C_INTERRUPT_NACK)
			debug("NACK ");
		if (int_status & ZYNQ_I2C_INTERRUPT_TO)
			debug("TO ");
		if (int_status & ZYNQ_I2C_INTERRUPT_SLVRDY)
			debug("SLVRDY ");
		if (int_status & ZYNQ_I2C_INTERRUPT_RXOVF)
			debug("RXOVF ");
		if (int_status & ZYNQ_I2C_INTERRUPT_TXOVF)
			debug("TXOVF ");
		if (int_status & ZYNQ_I2C_INTERRUPT_RXUNF)
			debug("RXUNF ");
		if (int_status & ZYNQ_I2C_INTERRUPT_ARBLOST)
			debug("ARBLOST ");
		if (status & ZYNQ_I2C_STATUS_RXDV)
			debug("RXDV ");
		if (status & ZYNQ_I2C_STATUS_TXDV)
			debug("TXDV ");
		if (status & ZYNQ_I2C_STATUS_RXOVF)
			debug("RXOVF ");
		if (status & ZYNQ_I2C_STATUS_BA)
			debug("BA ");
		debug("TS%d ", readl(&zynq_i2c->transfer_size));
		debug("\n");
	}
}
#endif

/*
 * zynq_i2c_calc_divs - Calculate clock dividers
 * @f:		I2C clock frequency
 * @input_clk:	Input clock frequency
 * @a:		First divider (return value)
 * @b:		Second divider (return value)
 *
 * f is used as input and output variable. As input it is used as target I2C
 * frequency. On function exit f holds the actually resulting I2C frequency.
 *
 * Return: 0 on success, negative errno otherwise.
 */
static int zynq_i2c_calc_divs(unsigned long *f, unsigned long input_clk,
		unsigned int *a, unsigned int *b)
{
	unsigned long fscl = *f, best_fscl = *f, actual_fscl, temp;
	unsigned int div_a, div_b, calc_div_a = 0, calc_div_b = 0;
	unsigned int last_error, current_error;

	/* calculate (divisor_a+1) x (divisor_b+1) */
	temp = input_clk / (22 * fscl);

	/*
	 * If the calculated value is negative or 0, the fscl input is out of
	 * range. Return error.
	 */
	if (!temp || (temp > (ZYNQ_I2C_DIVA_MAX * ZYNQ_I2C_DIVB_MAX)))
		return -EINVAL;

	last_error = -1;
	for (div_a = 0; div_a < ZYNQ_I2C_DIVA_MAX; div_a++) {
		div_b = ZYNQ_I2C_DIV_ROUND_UP(input_clk, 22 * fscl * (div_a + 1));

		if ((div_b < 1) || (div_b > ZYNQ_I2C_DIVB_MAX))
			continue;
		div_b--;

		actual_fscl = input_clk / (22 * (div_a + 1) * (div_b + 1));

		if (actual_fscl > fscl)
			continue;

		current_error = ((actual_fscl > fscl) ? (actual_fscl - fscl) :
							(fscl - actual_fscl));

		if (last_error > current_error) {
			calc_div_a = div_a;
			calc_div_b = div_b;
			best_fscl = actual_fscl;
			last_error = current_error;
		}
	}

	*a = calc_div_a;
	*b = calc_div_b;
	*f = best_fscl;

	return 0;
}

static void zynq_i2c_setup_master_mode(struct i2c_adapter *adap)
{
	struct zynq_i2c_registers *zynq_i2c = i2c_select(adap);
	
	/* Enable master mode, ack, and 7-bit addressing */
	setbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_MS |
		ZYNQ_I2C_CONTROL_ACKEN | ZYNQ_I2C_CONTROL_NEA);
}

/* Wait for an interrupt */
static u32 zynq_i2c_wait(struct zynq_i2c_registers *zynq_i2c, u32 mask)
{
	int timeout, int_status;

	for (timeout = 0; timeout < 100; timeout++) {
		udelay(100);
		int_status = readl(&zynq_i2c->interrupt_status);
		if (int_status & mask)
			break;
	}
#ifdef DEBUG
	zynq_i2c_debug_status(zynq_i2c);
#endif
	/* Clear interrupt status flags */
	writel(int_status & mask, &zynq_i2c->interrupt_status);

	return int_status & mask;
}

/*
 * I2C probe called by cmd_i2c when doing 'i2c probe'.
 * Begin read, nak data byte, end.
 */
static int zynq_i2c_probe(struct i2c_adapter *adap, u8 dev)
{
	struct zynq_i2c_registers *zynq_i2c = i2c_select(adap);

	/* Attempt to read a byte */
	setbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_CLR_FIFO |
		ZYNQ_I2C_CONTROL_RW);
	clrbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_HOLD);
	writel(0xFF, &zynq_i2c->interrupt_status);
	writel(dev, &zynq_i2c->address);
	writel(1, &zynq_i2c->transfer_size);

	return (zynq_i2c_wait(zynq_i2c, ZYNQ_I2C_INTERRUPT_COMP |
		ZYNQ_I2C_INTERRUPT_NACK) &
		ZYNQ_I2C_INTERRUPT_COMP) ? 0 : -ETIMEDOUT;
}

/*
 * I2C read called by cmd_i2c when doing 'i2c read' and by cmd_eeprom.c
 * Begin write, send address byte(s), begin read, receive data bytes, end.
 */
static int zynq_i2c_read(struct i2c_adapter *adap, u8 dev, uint addr,
			 int alen, u8 *data, int length)
{
	u32 status;
	u32 i = 0;
	u8 *cur_data = data;
	struct zynq_i2c_registers *zynq_i2c = i2c_select(adap);

	/* Check the hardware can handle the requested bytes */
	if ((length < 0) || (length > ZYNQ_I2C_TRANSFERT_SIZE_MAX))
		return -EINVAL;

	/* Write the register address */
	setbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_CLR_FIFO |
		ZYNQ_I2C_CONTROL_HOLD);
	writel(0xFF, &zynq_i2c->interrupt_status);
	if (alen) {
		clrbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_RW);
		writel(dev, &zynq_i2c->address);
		while (alen--)
			writel(addr >> (8 * alen), &zynq_i2c->data);

		/* Wait for the address to be sent */
		if (!zynq_i2c_wait(zynq_i2c, ZYNQ_I2C_INTERRUPT_COMP)) {
			/* Release the bus */
			clrbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_HOLD);
			return -ETIMEDOUT;
		}
		debug("Device acked address\n");
	}

	setbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_CLR_FIFO |
		ZYNQ_I2C_CONTROL_RW);
	/* Start reading data */
	writel(dev, &zynq_i2c->address);
	writel(length, &zynq_i2c->transfer_size);

	/* Wait for data */
	do {
		status = zynq_i2c_wait(zynq_i2c, ZYNQ_I2C_INTERRUPT_COMP |
			ZYNQ_I2C_INTERRUPT_DATA);
		if (!status) {
			/* Release the bus */
			clrbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_HOLD);
			return -ETIMEDOUT;
		}
		debug("Read %d bytes\n",
		      length - readl(&zynq_i2c->transfer_size));
		for (; i < length - readl(&zynq_i2c->transfer_size); i++)
			*(cur_data++) = readl(&zynq_i2c->data);
	} while (readl(&zynq_i2c->transfer_size) != 0);
	/* All done... release the bus */
	clrbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_HOLD);

#ifdef DEBUG
	zynq_i2c_debug_status(zynq_i2c);
#endif
	return 0;
}

/*
 * I2C write called by cmd_i2c when doing 'i2c write' and by cmd_eeprom.c
 * Begin write, send address byte(s), send data bytes, end.
 */
static int zynq_i2c_write(struct i2c_adapter *adap, u8 dev, uint addr,
			  int alen, u8 *data, int length)
{
	u8 *cur_data = data;
	struct zynq_i2c_registers *zynq_i2c = i2c_select(adap);

	/* Write the register address */
	setbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_CLR_FIFO |
		ZYNQ_I2C_CONTROL_HOLD);
	clrbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_RW);
	writel(0xFF, &zynq_i2c->interrupt_status);
	writel(dev, &zynq_i2c->address);
	if (alen) {
		while (alen--)
			writel(addr >> (8 * alen), &zynq_i2c->data);
		/* Start the tranfer */
		if (!zynq_i2c_wait(zynq_i2c, ZYNQ_I2C_INTERRUPT_COMP)) {
			/* Release the bus */
			clrbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_HOLD);
			return -ETIMEDOUT;
		}
		debug("Device acked address\n");
	}

	while (length--) {
		writel(*(cur_data++), &zynq_i2c->data);
		if (readl(&zynq_i2c->transfer_size) == ZYNQ_I2C_FIFO_DEPTH) {
			if (!zynq_i2c_wait(zynq_i2c, ZYNQ_I2C_INTERRUPT_COMP)) {
				/* Release the bus */
				clrbits_le32(&zynq_i2c->control,
					     ZYNQ_I2C_CONTROL_HOLD);
				return -ETIMEDOUT;
			}
		}
	}

	/* All done... release the bus */
	clrbits_le32(&zynq_i2c->control, ZYNQ_I2C_CONTROL_HOLD);
	/* Wait for the address and data to be sent */
	if (!zynq_i2c_wait(zynq_i2c, ZYNQ_I2C_INTERRUPT_COMP))
		return -ETIMEDOUT;
	return 0;
}

static unsigned int zynq_i2c_set_bus_speed(struct i2c_adapter *adap,
			unsigned int speed)
{
	unsigned int div_a, div_b;
	unsigned long fscl = speed;
	struct zynq_i2c_registers *zynq_i2c = i2c_select(adap);

	/* Supported speed grades are 100 kHz, 400 kHz, 1 MHz */
	if ((speed != 100000) && (speed != 400000) && (speed != 1000000)) {
		debug("%s: Unsupported i2c clock speed: %u\n",__func__, speed);
		return -EINVAL;
	}

	if (zynq_i2c_calc_divs(&fscl, CONFIG_SYS_I2C_ZYNQ_INPUT_CLK_SPEED, &div_a, &div_b)) {
		debug("%s: Error when calculating clock divisors, using default values.\n",__func__);
		div_a = 2;
		div_b = 16;
	}
	
	debug("%s: i2c clock speed = %lu (a=%u, b=%u)\n",__func__,fscl,div_a,div_b);

	/* Write the clock divisors to set the i2c clock speed*/
	writel((div_b << ZYNQ_I2C_CONTROL_DIV_B_SHIFT) |
			(div_a << ZYNQ_I2C_CONTROL_DIV_A_SHIFT), &zynq_i2c->control);

	return 0;
}

/* I2C init called by cmd_i2c when doing 'i2c reset'. */
static void zynq_i2c_init(struct i2c_adapter *adap, int requested_speed,
			  int slaveadd)
{
	zynq_i2c_set_bus_speed(adap, CONFIG_SYS_I2C_ZYNQ_SPEED); // ignore requested speed, use config value
	zynq_i2c_setup_master_mode(adap);
}

#ifdef CONFIG_ZYNQ_I2C0
U_BOOT_I2C_ADAP_COMPLETE(zynq_0, zynq_i2c_init, zynq_i2c_probe, zynq_i2c_read,
			 zynq_i2c_write, zynq_i2c_set_bus_speed,
			 CONFIG_SYS_I2C_ZYNQ_SPEED, CONFIG_SYS_I2C_ZYNQ_SLAVE,
			 0)
#endif
#ifdef CONFIG_ZYNQ_I2C1
U_BOOT_I2C_ADAP_COMPLETE(zynq_1, zynq_i2c_init, zynq_i2c_probe, zynq_i2c_read,
			 zynq_i2c_write, zynq_i2c_set_bus_speed,
			 CONFIG_SYS_I2C_ZYNQ_SPEED, CONFIG_SYS_I2C_ZYNQ_SLAVE,
			 1)
#endif

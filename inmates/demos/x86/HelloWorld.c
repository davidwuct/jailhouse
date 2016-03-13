/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2014
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 *
 * WARNING: This is just a demo, using a commonly available resource. IRQ 9
 * is bluntly stolen from Linux while it may still listen to this source in
 * the ACPI driver. Even when destroying the cell, Linux won't regain control
 * as it won't reprogram the IOAPIC. Moreover, the IRQ might be shared with
 * other devices which will lose their IRQs when starting this cell.
 *
 * Strong recommendation: Avoid using legacy interrupt for non-root cells!
 * Many of them can't be isolated easily from other other cells - if at all.
 */

#include <inmate.h>

#ifdef CONFIG_UART_OXPCIE952
#define UART_BASE		0xe000
#else
#define UART_BASE		0x3f8
#endif

void inmate_main(void){
	printk_uart_base = UART_BASE;
	printk("Hello World!\n");
}


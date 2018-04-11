/*
 * arch/arm/include/debug/stm32f7.S
 *
 *  Copyright (C) 1994-1999 Russell King
 *  Copyright (C) 2018 Sheng Yang
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

.macro  addruart, rp, rv, tmp
ldr \rp, =CONFIG_DEBUG_STM32F7_UART_PHYS
.endm

.macro	waituart,rd,rx
.endm

#define STM32_UART_TDR  0x28
.macro  senduart,rd,rx
strb   \rd, [\rx, #STM32_UART_TDR]
.endm

#define STM32_UART_ISR      0x1c
#define STM32_UART_TC_FLAG  0x1 << 6      
.macro	busyuart,rd,rx
3201:		ldrb	\rd, [\rx, #STM32_UART_ISR]
and	\rd, \rd, #STM32_UART_TC_FLAG
teq	\rd, #0x0
beq	3201b
.endm


/**
 * @file    bootloader.h
 */

#ifndef BOOTLOADER_H_
#define BOOTLOADER_H_

#include "stm32l0xx_hal.h"
#include "main.h"

#define TRANSMIT_DRIVER_ON	DIR_MCU_ON
#define TRANSMIT_DRIVER_OFF	DIR_MCU_OFF

#define VALID_FIRMWARE_VALUE	0x55555555
#define JUMP_FROM_APPLICATION_TAG_VALUE 0xAABBCCDD

#define BOOTLOADER_VERSION 0x10
#define DEVICE_ID 0x3344

#define	BOOTLOADER_TIMEOUT_MS	60000U

typedef void (*DISABLE_HARDWARE_FUNC_PTR)(void);

void bootloaderInit(UART_HandleTypeDef *huart, DISABLE_HARDWARE_FUNC_PTR disableHardwareCallbackFunction);
void bootloaderHandler(void);
void bootloaderTimer(void);
void bootloaderRxCompletedCallback(void);
void bootloaderTxCompletedCallback(void);

#endif /* BOOTLOADER_H_ */

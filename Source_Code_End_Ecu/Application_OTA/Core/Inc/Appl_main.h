/*
 * Appl_main.h
 *
 *  Created on: Aug 14, 2026
 *      Author: hegde
 */

#ifndef INC_APPL_MAIN_H_
#define INC_APPL_MAIN_H_

#include "stm32f3xx_hal.h"


/* Uart rx buffer length */
#define RX_BUFFER_LEN  10U

#define BOOTLOADER_START_ADDRESS    0x8000000U
#define BOOTLOADER_LENGTH           (40U*1024U)
#define BOOTLOADER_RESET_HANDLER_ADDRESS   0x8000004U

#define BOOTLOADER_JUMP_MAGIC_VALUE  0x7F8E9DAB
#define VALID_FLAG                   0x80800808
#define INVALID_FALG				 0x12345678

#define VALIDITY_FLAG_AREA       0x8027000
#define VERSION_INFO_AREA        0x8027800

#define APPLICATION_VERSION      0x4

#define APPL_SESSION_INFO  0x86

#define APPL_JUMP_TO_PROGRAMMING_SESSION   0x85

#define LD2_Pin GPIO_PIN_5
#define LD2_GPIO_Port GPIOA

extern volatile uint32_t session_info;
extern uint32_t current_session;

#define APPLICATION 0x02

void ApplTask();
void printmsg(char *format,...);
void jumpToBootloader();
void application_uart_response_data(uint8_t *pBuffer,uint32_t len);
void application_write_validity_flag(void);
HAL_StatusTypeDef erase_validityflag_area(void);

#endif /* INC_APPL_MAIN_H_ */

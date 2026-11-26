/*
 * Appl_main.c
 *
 *  Created on: Aug 14, 2026
 *      Author: hegde
 */

#ifndef SRC_APPL_MAIN_C_
#define SRC_APPL_MAIN_C_

#include "Appl_main.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#endif /* SRC_APPL_MAIN_C_ */

/* MACRO definitions */

/* Uart handler alias */
#define D_UART      &huart2
#define C_UART      &huart3

/* UART Handler*/
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;

/* UART Rx buffer for receiving Command for BL Operation*/
uint8_t uart_rx[RX_BUFFER_LEN];

uint8_t service_id = 0;


uint32_t current_session;
__attribute__((section(".validity_flag"))) volatile uint32_t Validity_Flag;

__attribute__((section(".version_info"), used))
const volatile uint32_t Version_Info = APPLICATION_VERSION;

/* Function Definitions */

void ApplTask()
{
	uint8_t response_buffer[2];
	HAL_UART_Receive_IT(C_UART, &uart_rx[0], 1);
	while(1)
	{
		switch(service_id)
		{
			case APPL_SESSION_INFO:
			{
				uint8_t response_buffer[2];

				printmsg("Current Session is Application\n");

			    /* UART Response Buffer Prepearation*/
			    response_buffer[0] = APPL_SESSION_INFO;
			    response_buffer[1] = APPLICATION;

			    /* Call Uart response data to send the response to flashing tool*/
			    application_uart_response_data(&response_buffer[0], 2);
				break;
			}

			case APPL_JUMP_TO_PROGRAMMING_SESSION:
			{
				erase_validityflag_area();

				application_write_validity_flag();

				response_buffer[0] = APPL_JUMP_TO_PROGRAMMING_SESSION;
				response_buffer[1] = 0x00;

				application_uart_response_data(&response_buffer[0], 2);

				jumpToBootloader();
				break;
			}
		}
	}
}

void printmsg(char *format,...)
{
	char str[80];

	/* Extract the argument list using VA apis */
	va_list args;
	va_start(args, format);
	vsprintf(str, format, args);
	HAL_UART_Transmit(D_UART, (uint8_t*)str, strlen(str), HAL_MAX_DELAY);
	va_end(args);

}

void jumpToBootloader()
{
	// Software Reset is triggered, Control automatically goes to Bootloader
	NVIC_SystemReset();
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	service_id = uart_rx[0];

	HAL_UART_Receive_IT(C_UART, &uart_rx[0], 1);
}

/**
  * @brief Uart response to flashing tool handler
  * param[in] pBuffer  Pointer to Uart Rx buffer
  * param[in] len length of uart response data
  * @retval None
  */
void application_uart_response_data(uint8_t *pBuffer,uint32_t len)
{
	/* Driver dunction to transmit data via UART */
	HAL_UART_Transmit(C_UART,pBuffer,len,HAL_MAX_DELAY);
}


/**
  * @brief Validity flag write function
  * @retval None
  */
void application_write_validity_flag()
{
	HAL_StatusTypeDef Erase_Status;
	uint32_t Val_value;
	uint32_t* storagePtr = (uint32_t*)VALIDITY_FLAG_AREA;

	HAL_FLASH_Unlock();

	(void)erase_validityflag_area();

	Val_value = INVALID_FALG;
	HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, storagePtr , Val_value );

	HAL_FLASH_Lock();
}

/**
  * @brief Signature erase function before flashing application
  * @retval None
  */
HAL_StatusTypeDef erase_validityflag_area()
{
	FLASH_EraseInitTypeDef validityflag_area;
	uint32_t pageError;
	HAL_StatusTypeDef Erase_Status;

	/* Erase the Validity Flag area custom function*/

	validityflag_area.TypeErase = FLASH_TYPEERASE_PAGES;
	validityflag_area.PageAddress = (uint32_t)VALIDITY_FLAG_AREA;
	validityflag_area.NbPages = 1;

	Erase_Status = HAL_FLASHEx_Erase(&validityflag_area, &pageError);
	return Erase_Status;
}

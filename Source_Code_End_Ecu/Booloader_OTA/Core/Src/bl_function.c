/*
 * bl_function.c
 *
 *  Created on: Apr 28, 2026
 *      Author: hegde
 */


#include "bl_function.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* MACRO definitions */
#define BL_DEBUG_MSG_EN
/* Uart handler alias */
#define C_UART     &huart3
#define D_UART     &huart2

uint8_t toggle_flag = GPIO_PIN_SET;

uint8_t service_id = 0;


/* Logical Block Info */
struct logical_block_info logical_block = {
		1,
		{
			{APPLICATION_IDENTIFIER, APPLICATION_START_ADDRESS, APPLCIATION_LENGTH},
		}
};

__attribute__((section(".noinit"))) volatile uint32_t session_info;
uint32_t current_session;
__attribute__((section(".validity_flag"))) volatile uint32_t Validity_Flag;

__attribute__((section(".version_info"))) volatile uint32_t current_Version_Application;

__attribute__((section(".new_version_info"))) volatile uint32_t new_Version_Application;

/* Current flashing logical block Info */
uint32_t current_logical_block_start_address;
static uint16_t current_logical_block_length = 0;
uint16_t curr_Falshed_length = 0;
uint32_t currentAddress;

/* UART Rx buffer for receiving Command for BL Operation*/
uint8_t uart_rx[RX_BUFFER_LEN];

/* UART Handler*/
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;

/* Function Definitions */


/**
  * @brief fbl task
  * @retval None
  */
void fbltask(void)
{
	HAL_UART_Receive_IT(C_UART, uart_rx, 1);

	while(1)
	{
		fblstatemachine();

		HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, toggle_flag);
		toggle_flag = !toggle_flag;
	}
}

/**
  * @brief fbl uart command statemachine
  * @retval None
  */
void fblstatemachine(void)
{
	switch(service_id)
	{
		case BL_GET_VER:
		{
			/* Get version Command 0x80 */
			bootloader_handle_getver_cmd();
			break;
		}

		case BL_GET_SESSION:
		{
			/*Session Info*/
			bootloader_session_info();
			break;
		}

		case BL_FLASH_START:
		{
			/* Command to start the flashing 0x81*/
			bootloader_uart_flash_data();
			break;
		}
	}
}

void bootloader_session_info(void)
{
	uint8_t response_buffer[2];

	printmsg("Current Session is Bootloader\n");

    /* UART Response Buffer Prepearation*/
    response_buffer[0] = BL_GET_SESSION;
    response_buffer[1] = BOOTLOADER;

    /* Call Uart response data to send the response to flashing tool*/
    bootloader_uart_response_data(&response_buffer[0], 2);
}

/**
  * @brief Read UART Command and switch to particular case based on command
  * @retval None
  */
void bootloader_uart_flash_data(void)
{
	uint8_t response_buffer[2];

	/* Stop recieving data via interrupt since flashing started*/
	HAL_UART_AbortReceive(C_UART);

	response_buffer[0] = service_id;
	response_buffer[1] = 0x00;

	bootloader_uart_response_data(&response_buffer[0], 2);

	while(1)
	{
		memset(uart_rx, 0, 10);
		/* Read only one byte from the UART, which will tell about how many bytes are there after*/
		HAL_UART_Receive(C_UART, &uart_rx[0], 1, HAL_MAX_DELAY);

		/* Switch to cases based on command*/
		switch ( uart_rx[0] )
		{
		      /* Flash Erase Command 0x82 */
		      case BL_FLASH_ERASE:
		      {
		    	  HAL_UART_Receive(C_UART, &uart_rx[1], 2, HAL_MAX_DELAY);
		          bootloader_handle_flash_erase_cmd(uart_rx);
		          break;
		      }

		      /*Flash Write Command 0x83 */
		      case BL_MEM_WRITE:
		      {
		          bootloader_handle_mem_write_cmd(uart_rx);
		          break;
		      }

		      default:
		      {
		          printmsg("Invalid Command recieved from Flashing tool \n");
		          break;
		      }

		}

	}

}

/**
  * @brief Jump to application logic
  * @retval None
  */
void bootloader_jump_to_user_app(void)
{
	/* Create a function pointer to hold the reset handler*/
	void(*app_reset_handler)(void);

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

	__disable_irq();

	HAL_RCC_DeInit();

	HAL_UART_DeInit(&huart2);
	HAL_UART_DeInit(&huart3);

	/* Explicitly clear USART flags */
	__HAL_UART_CLEAR_FLAG(&huart2, UART_FLAG_TC | UART_FLAG_RXNE | UART_FLAG_TXE);
	__HAL_UART_CLEAR_FLAG(&huart3, UART_FLAG_TC | UART_FLAG_RXNE | UART_FLAG_TXE);

	/* Disable USART NVIC explicitly */
	HAL_NVIC_DisableIRQ(USART2_IRQn);
	HAL_NVIC_DisableIRQ(USART3_IRQn);
	HAL_NVIC_ClearPendingIRQ(USART2_IRQn);
	HAL_NVIC_ClearPendingIRQ(USART3_IRQn);

	/* Configure the MSP */
	uint32_t msp_value = *((volatile uint32_t*)APPLICATION_START_ADDRESS);

	__set_MSP(msp_value);

	/* Fetch the reset handler and store that in Function pointer, Reset handler present at Flash start address + 4 */
	uint32_t reset_handler_address = *((volatile uint32_t*)APPLICATION_RESET_HANDLER_ADDRESS);

	app_reset_handler = (void*)reset_handler_address;

	/* Jump to reset handler of Application*/
	app_reset_handler();

}

/**
  * @brief Bootloader version handler
  * @retval None
  */
void bootloader_handle_getver_cmd()
{
	uint8_t bl_version;
	uint8_t response_buffer[2];

	/* Call Bootlder version handler*/
    bl_version = get_bootloader_version();

    /* UART Response Buffer Prepearation*/
    response_buffer[0] = E_OK;
    response_buffer[1] = bl_version;

    /* Call Uart response data to send the response to flashing tool*/
    bootloader_uart_response_data(&response_buffer[0], 2);
}


/**
  * @brief Erase logic Handler
  * param[in] pBuffer  Pointer to UART Rx buffer
  * @retval None
  */
void bootloader_handle_flash_erase_cmd(uint8_t *pBuffer)
{
	FLASH_EraseInitTypeDef decoded_value;
	uint32_t pageError;
	uint8_t response_buffer[2];
	HAL_StatusTypeDef Erase_Status = HAL_ERROR;
	Std_Return_Type status = E_NOT_OK;

	/* Function decode the erase command sent by tester*/
	status = decode__erase_command(pBuffer, &decoded_value);

	if(status == E_OK)
	{
		/*
		 * 1. Unlock Flash using driver API
		 * 2. Call HAL_FLASHEx_Erase() API to erase the Page
		 * 3. Erase the previous signature when Application erase is called
		 * 4. Lock the flash using driver API*/

		uint32_t t_start = DWT_GetCycles();

		HAL_FLASH_Unlock();
		Erase_Status = HAL_FLASHEx_Erase(&decoded_value, &pageError);
		Erase_Status = erase_signature_area();
		HAL_FLASH_Lock();

		g_perf_metrics.erase_cycles = DWT_GetCycles() - t_start;
	}
	else
	{
		printmsg("\r\n--- Wrong Identifier ---\r\n");
	}


	/* Send the erase status to to tester*/
	if(Erase_Status == HAL_OK)
	{
		response_buffer[0] = pBuffer[0];
		response_buffer[1] = Erase_Status;
		printmsg("\r\n--- Erase Success ---\r\n");
	}
	else
	{
		response_buffer[0] = pBuffer[0];
		response_buffer[1] = Erase_Status;
	}

	bootloader_uart_response_data(&response_buffer[0], 2);
}


/**
  * @brief Flash write logic
  * param[in] pBuffer  Pointer to UART Rx buffer
  * @retval None
  */
void bootloader_handle_mem_write_cmd(uint8_t *pBuffer)
{
	uint8_t response_buffer[3];
	HAL_StatusTypeDef returnStatus;
	uint8_t flash_data_receive[256];
	uint8_t plain_text[256];

	/*
	 * 1. Unlock the flash
	 * 2. Receive the frame number
	 * 3. Receive the Application data
	 * 4. Call HAL_FLASH_Program() to flash the data
	 * 5. When flashing is successful, call the verify function
	 * */

	HAL_FLASH_Unlock();
	/* Receive the Frame Number */
	HAL_UART_Receive(C_UART, &pBuffer[1], 1, HAL_MAX_DELAY);

	/* Receive the Application data in chunk of 256 bytes*/
	HAL_UART_Receive(C_UART, &flash_data_receive[0], BL_MESSAGE_CHUNK_LENGTH, HAL_MAX_DELAY);

	/* --- Benchmark AES-256-CBC Decryption --- */
	uint32_t t_dec_start = DWT_GetCycles();

	/*AES Decryption Call*/
	(void)Application_Decrypt(&flash_data_receive[0], &plain_text[0], CHUNK_SIZE);

	g_perf_metrics.total_decrypt_cycles += (DWT_GetCycles() - t_dec_start);

	/* Assign the data pointer to uint32_t pointer so that when it is incremented, it will be incremented in words*/
	uint32_t *pData = (uint32_t *)&plain_text[0];

	/* --- Benchmark Flash Writing --- */
	uint32_t t_write_start = DWT_GetCycles();

	/* Iterate through all the words and flash it to destination address*/
	for(int j = 0; j< TOTAL_NO_WORDS; j++)
	{
		returnStatus = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, currentAddress , pData[j] );
		if(returnStatus == HAL_OK)
		{
			/* Flashing happens in the length of words, hence incrementing the address by 4*/
			currentAddress += 4;
			response_buffer[2] = returnStatus;
		}
		else
		{
			response_buffer[2] = returnStatus;
			break;
		}

	}

	g_perf_metrics.total_flash_write_cycles += (DWT_GetCycles() - t_write_start);
	g_perf_metrics.chunk_count++;

	/* Clearing the buffer */
	memset(flash_data_receive, 0, sizeof(flash_data_receive));

	curr_Falshed_length += BL_MESSAGE_CHUNK_LENGTH;
	response_buffer[0] = pBuffer[0];
	response_buffer[1] = pBuffer[1];
	bootloader_uart_response_data(&response_buffer[0], 3);
	HAL_FLASH_Lock();

	/* When Flashing of all bytes are successful, Verification function is called*/
	if(curr_Falshed_length == current_logical_block_length)
	{
		bootloader_handle_mem_verify();
	}

}


/**
  * @brief Jump to application logic
  * @retval uint8_t   Bootloder version
  */
uint8_t get_bootloader_version(void)
{
	return BL_VERSION;
}


/**
  * @brief Uart response to flashing tool handler
  * param[in] pBuffer  Pointer to Uart Rx buffer
  * param[in] len length of uart response data
  * @retval None
  */
void bootloader_uart_response_data(uint8_t *pBuffer,uint32_t len)
{
	/* Driver dunction to transmit data via UART */
	HAL_UART_Transmit(C_UART,pBuffer,len,HAL_MAX_DELAY);
}


/**
  * @brief decode erase command coming from flashing tool and prepare structure of values for driver level flash api
  * param[in]   pBuffer           Pointer to UART Rx buffer
  * param[out]  decoded_value     structure data needed for driver erase command
  * @retval None
  */
Std_Return_Type decode__erase_command(uint8_t* pBuffer, FLASH_EraseInitTypeDef* decoded_value)
{
	uint16_t identifier;
	Std_Return_Type status = E_NOT_OK;

	/*
	 * 1. Get the identifier
	 * 2. Using the identifier, Get the logical address and length from logical block stored in Bootloader software
	 * 3. Fill the required structure for Erase API
	 * */

	identifier = (((uint32_t)pBuffer[1] << 8)  |
                  ((uint32_t)pBuffer[2]));


	for(int i=0;i<logical_block.no_of_blocks;i++)
	{
		if(identifier == logical_block.block_info[i].identifier)
		{
			current_logical_block_start_address = logical_block.block_info[i].start_address;
			current_logical_block_length = logical_block.block_info[i].length;
			currentAddress = current_logical_block_start_address;

			decoded_value->TypeErase = FLASH_TYPEERASE_PAGES;
			decoded_value->PageAddress = current_logical_block_start_address;
			decoded_value->NbPages = (current_logical_block_length/2048);

			status = E_OK;
		}
	}

	return status;
}


/**
  * @brief Verification function
  * @retval None
  */
void bootloader_handle_mem_verify()
{
	uint8_t signature_command;
	uint8_t signatureRec[64] __attribute__((aligned(4)));
	uint8_t outputDigest[32] __attribute__((aligned(4)));
	Std_Security_Return_type returnStatus_Security;
	uint8_t response_buffer[2];

	/*
	 * 1. Receive the Signature command 0x84
	 * 2. Receive the Signature data
	 * 3. Calculate the Application Hash
	 * 4. Verify the signature
	 * 5. Decision logic based on signature verification
	 * */

	/* Receive the Signature Command */
	HAL_UART_Receive(C_UART, &signature_command , SIGNATURE_VERIFICATION_CMD_SIZE , HAL_MAX_DELAY);

	/* Receive the Signature Data */
	HAL_UART_Receive(C_UART, &signatureRec[0], SIGNATURE_SIZE, HAL_MAX_DELAY);

	response_buffer[0] = signature_command;

	/* --- Benchmark SHA-256 Calculation --- */
	uint32_t t_hash_start = DWT_GetCycles();

	/* Call Security wrapper for Calculating Hash */
	returnStatus_Security = Calculate_Firmware_Hash(&outputDigest[0], current_logical_block_start_address, (uint16_t)current_logical_block_length);

	printmsg("\r\n--Hashing Successful--\r\n");

	g_perf_metrics.sha256_cycles = DWT_GetCycles() - t_hash_start;

	if(returnStatus_Security == 0)
	{
		/* --- Benchmark ECDSA-P256 Signature Verification --- */
		uint32_t t_sig_start = DWT_GetCycles();

		/* Call Security wrapper for verifying the Signature */

		returnStatus_Security = Calculate_Signature(&outputDigest[0], &signatureRec[0]);

		g_perf_metrics.ecdsa_verify_cycles = DWT_GetCycles() - t_sig_start;

		Decrypt_Finalize();

		if(returnStatus_Security == 0)
		{

			printmsg("\r\n--- Signature verification Successful ---\r\n");

			/* If verification successful, store the signature in dedicated area in flash */
			returnStatus_Security = application_signature_write(&signatureRec[0]);

			returnStatus_Security = Version_Check();

			if(returnStatus_Security == 0)
			{
				printmsg("\r\n--- Version Check Successful ---\r\n");

				returnStatus_Security = bootloader_Update_Application_version();

				if(returnStatus_Security == 0)
				{
					printDebugMessage();

					response_buffer[1] = returnStatus_Security;

					bootloader_uart_response_data(&response_buffer[0], 2);


					session_info = 0x00000001;

					bootloader_write_validity_flag(returnStatus_Security);

					NVIC_SystemReset();
				}
				else
				{
					response_buffer[1] = returnStatus_Security;

					bootloader_uart_response_data(&response_buffer[0], 2);

					bootloader_write_validity_flag(returnStatus_Security);

					/* Reset the system when verification failed, after reset, control stays in Bootloader */
					NVIC_SystemReset();
				}

			}
			else
			{
				printmsg("\r\n--- Version Check Unsuccessful ---\r\n");

				HAL_Delay(1000);

				NVIC_SystemReset();
			}

		}
		else
		{
			printmsg("\r\n--- Signature Check Unsuccessful ---\r\n");

			HAL_Delay(1000);

			NVIC_SystemReset();
		}

	}

}

Std_Security_Return_type Version_Check(void)
{
	printmsg("Currrent Version is %d and Newer version is %d", current_Version_Application, new_Version_Application);
	return(!(new_Version_Application > current_Version_Application));
}

void printDebugMessage()
{
#ifdef BL_DEBUG_MSG_EN
	printmsg("\r\n--- Performance Metrics (Clock = 72MHz) ---\r\n");

	/* Erase */
	printmsg("Erase: %lu cycles (%lu.%02lu ms)\r\n",
			 (unsigned long)g_perf_metrics.erase_cycles,
			 (unsigned long)(g_perf_metrics.erase_cycles / 72000),
			 (unsigned long)((g_perf_metrics.erase_cycles % 72000) * 100 / 72000));

	/* Decrypt */
	printmsg("Total Decrypt: %lu cycles (%lu.%02lu ms) across %lu chunks\r\n",
			 (unsigned long)g_perf_metrics.total_decrypt_cycles,
			 (unsigned long)(g_perf_metrics.total_decrypt_cycles / 72000),
			 (unsigned long)((g_perf_metrics.total_decrypt_cycles % 72000) * 100 / 72000),
			 (unsigned long)g_perf_metrics.chunk_count);

	/* Flash Write */
	printmsg("Total Flash Write: %lu cycles (%lu.%02lu ms)\r\n",
			 (unsigned long)g_perf_metrics.total_flash_write_cycles,
			 (unsigned long)(g_perf_metrics.total_flash_write_cycles / 72000),
			 (unsigned long)((g_perf_metrics.total_flash_write_cycles % 72000) * 100 / 72000));

	/* SHA-256 */
	printmsg("SHA256 Hash: %lu cycles (%lu.%02lu ms)\r\n",
			 (unsigned long)g_perf_metrics.sha256_cycles,
			 (unsigned long)(g_perf_metrics.sha256_cycles / 72000),
			 (unsigned long)((g_perf_metrics.sha256_cycles % 72000) * 100 / 72000));

	/* ECDSA */
	printmsg("ECDSA Verify: %lu cycles (%lu.%02lu ms)\r\n",
			 (unsigned long)g_perf_metrics.ecdsa_verify_cycles,
			 (unsigned long)(g_perf_metrics.ecdsa_verify_cycles / 72000),
			 (unsigned long)((g_perf_metrics.ecdsa_verify_cycles % 72000) * 100 / 72000));
#endif
}

/**
  * @brief Validity flag write function
  * @retval None
  */
void bootloader_write_validity_flag(Std_Security_Return_type varification_status)
{
	HAL_StatusTypeDef Erase_Status;
	uint32_t Val_value;
	uint32_t* storagePtr = (uint32_t*)VALIDITY_FLAG_AREA;

	HAL_FLASH_Unlock();

	(void)erase_validityflag_area();

	if(varification_status == 0)
	{
		Val_value = VALID_FLAG;
		HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, storagePtr , Val_value );
	}
	else
	{
		Val_value = INVALID_FALG;
		HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, storagePtr , Val_value );
	}

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

/**
  * @brief Version info write function
  * @retval None
  */
Std_Security_Return_type bootloader_Update_Application_version()
{
	HAL_StatusTypeDef Erase_Status;
	uint32_t* storagePtr = (uint32_t*)NEW_VERSION_AREA;

	HAL_FLASH_Unlock();

	(void)erase_version_area();


	HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, storagePtr , new_Version_Application );

	HAL_FLASH_Lock();
}

/**
  * @brief Version erase function before updating application version
  * @retval None
  */
HAL_StatusTypeDef erase_version_area()
{
	FLASH_EraseInitTypeDef version_area;
	uint32_t pageError;
	HAL_StatusTypeDef Erase_Status;

	/* Erase the Validity Flag area custom function*/

	version_area.TypeErase = FLASH_TYPEERASE_PAGES;
	version_area.PageAddress = (uint32_t)NEW_VERSION_AREA;
	version_area.NbPages = 1;

	Erase_Status = HAL_FLASHEx_Erase(&version_area, &pageError);
	return Erase_Status;
}


/**
  * @brief Signature erase function before flashing application
  * @retval None
  */
HAL_StatusTypeDef erase_signature_area()
{
	FLASH_EraseInitTypeDef signature_area;
	uint32_t pageError;
	HAL_StatusTypeDef Erase_Status;

	/* Erase the Signature area custom function*/

	signature_area.TypeErase = FLASH_TYPEERASE_PAGES;
	signature_area.PageAddress = (uint32_t)SIGNATURE_STORAGE_AREA;
	signature_area.NbPages = 1;

	Erase_Status = HAL_FLASHEx_Erase(&signature_area, &pageError);
	return Erase_Status;

}


/**
  * @brief Write Signature to dedicated flash area
  * param[in]  Signature  Signature pointer which contains the currently flashing software signature
  * @retval None
  */
Std_Security_Return_type application_signature_write(uint8_t* signature)
{
	uint8_t no_of_words = (SIGNATURE_SIZE/4U);
	uint32_t* TempPtr = (uint32_t*)signature;
	uint32_t storagePtr = (uint32_t)SIGNATURE_STORAGE_AREA;
	HAL_StatusTypeDef returnStatus;

	/*
	 * 1. Unlock the flash
	 * 2. Write the signature
	 * 3. Lock the flash
	 * */
	HAL_FLASH_Unlock();

	for(int j = 0; j< no_of_words; j++)
	{
		returnStatus = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, storagePtr , TempPtr[j] );
		if(returnStatus == HAL_OK)
		{
			storagePtr += 4;
		}
		else
		{
			return E_NOT_OK;
		}
	}

	HAL_FLASH_Lock();

	return E_OK;
}


/**
  * @brief Security verification function before boot the application
  * @retval None
  */
Std_Security_Return_type signature_check_boot(void)
{
	uint8_t outputDigest[32] __attribute__((aligned(4)));
	Std_Security_Return_type returnStatus_Security;

	/* Verify the signature before boot*/

	returnStatus_Security = Calculate_Firmware_Hash(&outputDigest[0], APPLICATION_START_ADDRESS, (uint16_t)APPLCIATION_LENGTH);

	if(returnStatus_Security == 0)
	{
		returnStatus_Security = Calculate_Signature(&outputDigest[0], SIGNATURE_PTR);
	}
	return returnStatus_Security;
}


/**
  * @brief Writ protection logic for page number 20 and 21
  * @retval None
  */
void Enable_Protection_Page20_21(void)
{
    FLASH_OBProgramInitTypeDef OBConfig;

    /* 1. Unlock Flash and Option Bytes
     * 2. Clear and Configure the structure
     * 3. Program the Option Bytes
     * 4. Launch the option byte changes */

    HAL_FLASH_Unlock();
    HAL_FLASH_OB_Unlock();

    /* Configure OBconfig needed for write protection */
    OBConfig.OptionType = OPTIONBYTE_WRP;
    OBConfig.WRPState   = OB_WRPSTATE_ENABLE;
    OBConfig.WRPPage    = OB_WRP_PAGES20TO21; // Bit 0x00000400U


    if (HAL_FLASHEx_OBProgram(&OBConfig) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_FLASH_OB_Launch();

}

/**
  * @brief UART Async Recieve callback
  * param[in]   huart   UART Handle
  * param[in]   Size    Size of the data filled
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	service_id = uart_rx[0];

	HAL_UART_Receive_IT(C_UART, uart_rx, 1);
}

/**
  * @brief Print message via UART
  * @retval None
  */
void printmsg(char *format,...)
{
#ifdef BL_DEBUG_MSG_EN
	char str[80];

	/* Extract the argument list using VA apis */
	va_list args;
	va_start(args, format);
	vsprintf(str, format, args);
	HAL_UART_Transmit(D_UART, (uint8_t*)str, strlen(str), HAL_MAX_DELAY);
	va_end(args);
#endif

}



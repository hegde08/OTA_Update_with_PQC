/*
 * bl_function.h
 *
 *  Created on: Apr 28, 2026
 *      Author: hegde
 */

#ifndef INC_BL_FUNCTION_H_
#define INC_BL_FUNCTION_H_

#include "stm32f3xx_hal.h"
#include "secure_wrapper.h"

#endif /* INC_BL_FUNCTION_H_ */

/* Typdef Defines */
typedef uint8_t  Std_Return_Type ;

/* Macro Definitions */
#define LD2_Pin GPIO_PIN_5
#define LD2_GPIO_Port GPIOA

/* Application Identifier */
#define APPLICATION_IDENTIFIER       0x0808

/* Application start address and length */
#define APPLICATION_START_ADDRESS    0x08020000U
#define APPLCIATION_LENGTH           (28U*1024U)
#define APPLICATION_RESET_HANDLER_ADDRESS   0x08020004U

/* No of logical block */
#define NO_OF_LOGICAL_BLOCK                1U

/* This command is to get the session software is in */
#define BL_GET_SESSION   0x86

/* This command is used to read the bootloader version from the MCU */
#define BL_GET_VER        0x80

/* This command is used to start the flashing */
# define BL_FLASH_START  0x81


/* This command is used to mass erase or sector erase of the user flash */
#define BL_FLASH_ERASE          0x82

/* This command is used to write data in to different memories of the MCU */
#define BL_MEM_WRITE             0x83

/* Bootloder Version */
#define BL_VERSION 0x10

/*Current Session */
#define BOOTLOADER 0x01

/* Acknowledgement */
#define E_OK    0x00
#define E_NOT_OK  0x01

/* Erase success and failure defines */
#define BL_ERASE_SUCCESS   0x00
#define BL_ERASE_FAILURE   0x01

/* Uart rx buffer length */
#define RX_BUFFER_LEN  10U
//#define BL_DEBUG_MSG_EN

/* Message chunk length for flashing*/
#define BL_MESSAGE_CHUNK_LENGTH  256U
#define TOTAL_NO_WORDS   (256U/4U)

#define BOOTLOADER_JUMP_MAGIC_VALUE  0x7F8E9DAB
#define VALID_FLAG                   0x80800808
#define INVALID_FALG				 0x12345678

/* Logical block structure */
struct logical_block_def
{
	uint16_t identifier;
	uint32_t start_address;
	uint16_t length;
};

struct logical_block_info
{
	uint8_t no_of_blocks;
	struct logical_block_def block_info[NO_OF_LOGICAL_BLOCK];
};

extern volatile uint32_t session_info;
extern uint32_t current_session;
extern volatile uint32_t Validity_Flag;

/* Custom Function declarations */

void Error_Handler(void);
void printmsg(char *format,...);
void bootloader_handle_getver_cmd();
void bootloader_handle_flash_erase_cmd(uint8_t *pBuffer);
void bootloader_handle_mem_write_cmd(uint8_t *pBuffer);
void bootloader_handle_mem_verify(void);
void bootloader_uart_flash_data(void);
void bootloader_jump_to_user_app(void);
Std_Return_Type decode__erase_command(uint8_t* pBuffer, FLASH_EraseInitTypeDef* decoded_value);
uint8_t get_bootloader_version(void);
void bootloader_uart_response_data(uint8_t *pBuffer,uint32_t len);
HAL_StatusTypeDef erase_signature_area(void);
Std_Security_Return_type application_signature_write(uint8_t* signature);
Std_Security_Return_type signature_check_boot();
void Enable_Protection_Page20_21(void);
void fbltask(void);
void fblstatemachine(void);
void bootloader_session_info(void);
HAL_StatusTypeDef erase_validityflag_area(void);
void bootloader_write_validity_flag(Std_Security_Return_type varification_status);
Std_Security_Return_type Version_Check(void);
void printDebugMessage();
Std_Security_Return_type bootloader_Update_Application_version();
HAL_StatusTypeDef erase_version_area();

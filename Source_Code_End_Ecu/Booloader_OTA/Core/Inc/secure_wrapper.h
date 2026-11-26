/*
 * secure_wrapper.h
 *
 *  Created on: Apr 26, 2026
 *      Author: hegde
 */

#ifndef INC_SECURE_WRAPPER_H_
#define INC_SECURE_WRAPPER_H_

#include "cmox_crypto.h"
#include "perf_metrics.h"

#endif /* INC_SECURE_WRAPPER_H_ */

/* Macro Definitions*/

#define SIGNATURE_SIZE                          64U
#define PUBLIC_KEY_SIZE                         64U
#define SIGNATURE_VERIFICATION_CMD_SIZE         1U
#define SIGNATURE_MSG_BUFFER_SIZE               (SIGNATURE_SIZE+SIGNATURE_VERIFICATION_CMD_SIZE)
#define SEC_OK                                  0U
#define SEC_NOT_OK		                        1U

#define PUBLIC_KEY_ADDR   0x800E800
#define PUB_KEY_PTR        ((const uint8_t*)(PUBLIC_KEY_ADDR))

#define SIGNATURE_STORAGE_AREA   0x800F000
#define SIGNATURE_PTR       ((uint8_t*)(SIGNATURE_STORAGE_AREA))

#define VALIDITY_FLAG_AREA       0x8027000

#define NEW_VERSION_AREA         0x8027800

#define CHUNK_SIZE          256
#define AES_256_KEY_SIZE    32
#define CBC_IV_SIZE         16


/* Typedef Definitions*/

typedef uint8_t  Std_Security_Return_type;

/* Function Declarations*/

void init_crypto_system(void);
Std_Security_Return_type Calculate_Firmware_Hash(uint8_t *output_digest, uint32_t startAddress, uint16_t size);
Std_Security_Return_type Calculate_Signature(uint8_t* outputDigest, uint8_t* signatureRec);
Std_Security_Return_type Application_Decrypt(const uint8_t *encrypted_chunk, uint8_t* plain_text, uint16_t chunk_len);
void Decrypt_Finalize(void);



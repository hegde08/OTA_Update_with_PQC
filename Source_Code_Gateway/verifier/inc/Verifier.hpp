/*
 * Verifier.hpp
 *
 *  Created on: Aug 11, 2026
 *      Author: hegde
 */

#ifndef INC_VERIFIER_HPP_
#define INC_VERIFIER_HPP_

#include <iostream>
#include <vector>
#include <string>
#include "shared_ipc.hpp"
#include <oqs/oqs.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

using namespace std;

class UpdateVerifier : public IpcServer
{
public:
	UpdateVerifier();

	void processMessage(int rcvid, const UpdateMsg& msg) override;

private:
	bool verifyECDSA(const uint8_t* digest, size_t digestLen, const uint8_t* sig, size_t sigLen, const vector<uint8_t>& pubKey);
	bool verifyOQS(const char* algName, const uint8_t* digest, size_t digestLen, const uint8_t* sig, size_t sigLen, const vector<uint8_t>& pubKey);

	// Streaming decrypt & hash
	bool streamDecryptAndHash(
		const string& cipherFilePath,
		uint32_t payloadOffset,
		const string& plainTempFilePath,
		const vector<uint8_t>& aesKey,
		const uint8_t iv[12],
		const uint8_t tag[16],
		uint8_t outDigest[SHA256_DIGEST_LENGTH]
	);
};

#endif /* INC_VERIFIER_HPP_ */

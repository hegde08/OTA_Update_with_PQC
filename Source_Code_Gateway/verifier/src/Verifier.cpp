#include "Verifier.hpp"
#include <fstream>
#include <vector>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/trace.h>

constexpr size_t STREAM_CHUNK_SIZE = 64 * 1024; // 64 KB buffer
constexpr size_t CACHE_LINE_SIZE   = 64;        // Cache line width

UpdateVerifier::UpdateVerifier() : IpcServer(VERIFIER_SERVER_NAME)
{
}

bool UpdateVerifier::streamDecryptAndHash(
	const string& cipherFilePath,
	uint32_t payloadOffset,
	const string& plainTempFilePath,
	const vector<uint8_t>& aesKey,
	const uint8_t iv[12],
	const uint8_t tag[16],
	uint8_t outDigest[SHA256_DIGEST_LENGTH])
{
	int inFd = open(cipherFilePath.c_str(), O_RDONLY);
	if (inFd < 0)
	{
		cerr << "[Verifier] Cannot open ciphertext file: " << cipherFilePath << endl;
		return false;
	}

	if (payloadOffset > 0)
	{
		if (lseek(inFd, payloadOffset, SEEK_SET) == (off_t)-1)
		{
			cerr << "[Verifier] Failed to seek to payload offset: " << payloadOffset << endl;
			close(inFd);
			return false;
		}
	}

	ofstream outFile(plainTempFilePath, ios::binary | ios::trunc);
	if (!outFile)
	{
		cerr << "[Verifier] Cannot create temporary plaintext file: " << plainTempFilePath << endl;
		close(inFd);
		return false;
	}

	uint8_t* inBuf = nullptr;
	uint8_t* outBuf = nullptr;
	if (posix_memalign((void**)&inBuf, CACHE_LINE_SIZE, STREAM_CHUNK_SIZE) != 0 ||
	    posix_memalign((void**)&outBuf, CACHE_LINE_SIZE, STREAM_CHUNK_SIZE + 16) != 0)
	{
		cerr << "[Verifier] Failed to allocate cache-aligned memory." << endl;
		if (inBuf) free(inBuf);
		if (outBuf) free(outBuf);
		close(inFd);
		outFile.close();
		return false;
	}

	EVP_CIPHER_CTX* cipherCtx = EVP_CIPHER_CTX_new();
	EVP_MD_CTX* mdCtx = EVP_MD_CTX_new();

	if (!cipherCtx || !mdCtx)
	{
		cerr << "[Verifier] Failed to allocate OpenSSL contexts." << endl;
		free(inBuf);
		free(outBuf);
		if (cipherCtx) EVP_CIPHER_CTX_free(cipherCtx);
		if (mdCtx) EVP_MD_CTX_free(mdCtx);
		close(inFd);
		outFile.close();
		return false;
	}

	bool success = false;

	do
	{
		if (EVP_DecryptInit_ex(cipherCtx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) break;
		if (EVP_CIPHER_CTX_ctrl(cipherCtx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1) break;
		if (EVP_DecryptInit_ex(cipherCtx, nullptr, nullptr, aesKey.data(), iv) != 1) break;

		if (EVP_DigestInit_ex(mdCtx, EVP_sha256(), nullptr) != 1) break;

		int outLen = 0;
		ssize_t bytesRead = 0;
		bool streamError = false;

		while ((bytesRead = read(inFd, inBuf, STREAM_CHUNK_SIZE)) > 0)
		{
			__builtin_prefetch(inBuf, 0, 3);

			if (EVP_DecryptUpdate(cipherCtx, outBuf, &outLen, inBuf, bytesRead) != 1)
			{
				cerr << "[Verifier] EVP_DecryptUpdate failed during stream." << endl;
				streamError = true;
				break;
			}

			if (outLen > 0)
			{
				outFile.write(reinterpret_cast<const char*>(outBuf), outLen);
				if (EVP_DigestUpdate(mdCtx, outBuf, outLen) != 1)
				{
					cerr << "[Verifier] EVP_DigestUpdate failed during stream." << endl;
					streamError = true;
					break;
				}
			}
		}

		if (streamError || bytesRead < 0) break;

		if (EVP_CIPHER_CTX_ctrl(cipherCtx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) != 1)
		{
			cerr << "[Verifier] Failed to set expected GCM tag." << endl;
			break;
		}

		int finalLen = 0;
		if (EVP_DecryptFinal_ex(cipherCtx, outBuf, &finalLen) <= 0)
		{
			cerr << "[Verifier] AEAD Tag Verification Failed! Payload is corrupt or tampered." << endl;
			break;
		}

		if (finalLen > 0)
		{
			outFile.write(reinterpret_cast<const char*>(outBuf), finalLen);
			if (EVP_DigestUpdate(mdCtx, outBuf, finalLen) != 1) break;
		}

		unsigned int digestLen = 0;
		if (EVP_DigestFinal_ex(mdCtx, outDigest, &digestLen) != 1)
		{
			cerr << "[Verifier] Failed to finalize SHA-256." << endl;
			break;
		}

		success = true;
	} while (false);

	free(inBuf);
	free(outBuf);
	EVP_CIPHER_CTX_free(cipherCtx);
	EVP_MD_CTX_free(mdCtx);
	close(inFd);
	outFile.close();

	return success;
}

bool UpdateVerifier::verifyECDSA(const uint8_t* digest, size_t digestLen, const uint8_t* sig, size_t sigLen, const vector<uint8_t>& pubKey)
{
	if (sigLen != 64)
	{
		cerr << "[Verifier][ECDSA Error] Expected raw 64-byte signature, got " << sigLen << endl;
		return false;
	}

	BIO* bio = BIO_new_mem_buf(pubKey.data(), static_cast<int>(pubKey.size()));
	if (!bio) return false;

	EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	if (!pkey) return false;

	BIGNUM* r = BN_bin2bn(sig, 32, nullptr);
	BIGNUM* s = BN_bin2bn(sig + 32, 32, nullptr);
	if (!r || !s)
	{
		if (r) BN_free(r);
		if (s) BN_free(s);
		EVP_PKEY_free(pkey);
		return false;
	}

	ECDSA_SIG* ecSig = ECDSA_SIG_new();
	if (!ecSig)
	{
		BN_free(r);
		BN_free(s);
		EVP_PKEY_free(pkey);
		return false;
	}

	ECDSA_SIG_set0(ecSig, r, s);

	uint8_t* derSig = nullptr;
	int derLen = i2d_ECDSA_SIG(ecSig, &derSig);
	ECDSA_SIG_free(ecSig);

	if (derLen <= 0 || !derSig)
	{
		EVP_PKEY_free(pkey);
		return false;
	}

	EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new(pkey, nullptr);
	bool isValid = false;

	if (pctx)
	{
		if (EVP_PKEY_verify_init(pctx) > 0)
		{
			int ret = EVP_PKEY_verify(pctx, derSig, derLen, digest, digestLen);
			isValid = (ret == 1);
		}
		EVP_PKEY_CTX_free(pctx);
	}

	OPENSSL_free(derSig);
	EVP_PKEY_free(pkey);
	return isValid;
}

bool UpdateVerifier::verifyOQS(const char* algName, const uint8_t* digest, size_t digestLen, const uint8_t* sig, size_t sigLen, const vector<uint8_t>& pubKey)
{
	OQS_SIG* oqsSig = OQS_SIG_new(algName);
	if (oqsSig == nullptr)
	{
		cerr << "[Verifier] Failed to initialize OQS algorithm: " << algName << endl;
		return false;
	}

	OQS_STATUS status = OQS_SIG_verify(
		oqsSig,
		digest,
		digestLen,
		sig,
		sigLen,
		pubKey.data()
	);

	OQS_SIG_free(oqsSig);
	return (status == OQS_SUCCESS);
}

void UpdateVerifier::processMessage(int rcvid, const UpdateMsg& msg)
{
	if (msg.type == MSG_VERIFY_FILE || msg.type == MSG_VERIFY_FILE_ZEROCOPY)
	{
		cout << "[Verifier] Analyzing: " << msg.filepath << " (Payload Offset: " << msg.payloadOffset << ")" << endl;

		ExecutionMetrics metrics{};
		string keyPath;
		if (msg.header.sigAlgo == SIG_ALGO_ECDSA)
		{
			keyPath = "/var/keys/public_key_ecdsa.pem";
		}
		else if (msg.header.sigAlgo == SIG_ALGO_FALCON512)
		{
			keyPath = "/var/keys/public_key_fn_dsa.bin";
		}
		else if (msg.header.sigAlgo == SIG_ALGO_MLDSA44)
		{
			keyPath = "/var/keys/public_key_ml_dsa.bin";
		}
		else
		{
			cerr << "[Verifier] Unrecognized signature algorithm ID." << endl;
			sendReply(rcvid, -1);
			return;
		}

		ifstream pkFile(keyPath, ios::binary);
		if (!pkFile)
		{
			cerr << "[Verifier] Cannot open public key file: " << keyPath << endl;
			sendReply(rcvid, -1);
			return;
		}
		vector<uint8_t> publicKey((istreambuf_iterator<char>(pkFile)), istreambuf_iterator<char>());
		pkFile.close();

		ifstream aesKeyFile("/var/keys/aead_key.bin", ios::binary);
		if (!aesKeyFile)
		{
			cerr << "[Verifier] Cannot open AES key file." << endl;
			sendReply(rcvid, -1);
			return;
		}
		vector<uint8_t> aesKey((istreambuf_iterator<char>(aesKeyFile)), istreambuf_iterator<char>());
		aesKeyFile.close();

		if (aesKey.size() != 32)
		{
			cerr << "[Verifier] Invalid AES key size (expected 32 bytes)." << endl;
			sendReply(rcvid, -1);
			return;
		}

		// Calculate PQC overhead metrics
		size_t sigLen = msg.header.sigLen;
		metrics.signatureSize = static_cast<uint32_t>(sigLen);
		metrics.publicKeySize = static_cast<uint32_t>(publicKey.size());
		metrics.pqcOverheadPct = (sizeof(OtaHeader) > 0)
			? ((static_cast<double>(sigLen) / sizeof(OtaHeader)) * 100.0)
			: 0.0;

		// Destination is always the target tar.gz in /dev/shmem
		string decryptedTarPath = "/dev/shmem/ota_package.tar.gz";
		string tempFilePath     = decryptedTarPath + ".tmp";
		uint8_t digest[SHA256_DIGEST_LENGTH];

		TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_STREAM_DEC_START, "STREAM_DEC_START");
		auto dec_start = chrono::steady_clock::now();

		bool decryptOk = streamDecryptAndHash(
			msg.filepath,
			msg.payloadOffset,
			tempFilePath,
			aesKey,
			msg.header.iv,
			msg.header.gcmTag,
			digest
		);

		auto dec_end = chrono::steady_clock::now();
		TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_STREAM_DEC_END, "STREAM_DEC_END");

		metrics.streamDecryptMs = chrono::duration_cast<chrono::milliseconds>(dec_end - dec_start).count();

		if (!decryptOk)
		{
			cerr << "[Verifier] Decryption and Hash streaming failed. Aborting." << endl;
			std::remove(tempFilePath.c_str());
			sendReply(rcvid, -1, metrics);
			return;
		}

		cout << "[Verifier] Streaming Decrypt & Hash completed in: " << metrics.streamDecryptMs << " ms" << endl;

		if (sigLen == 0 || sigLen > MAX_SIGNATURE_SIZE)
		{
			cerr << "[Verifier] Invalid signature length specified in header: " << sigLen << endl;
			std::remove(tempFilePath.c_str());
			sendReply(rcvid, -1, metrics);
			return;
		}

		bool isValid = false;
		TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_SIG_VERIFY_START, "SIG_VERIFY_START");
		auto start_time = chrono::steady_clock::now();

		switch (msg.header.sigAlgo)
		{
			case SIG_ALGO_ECDSA:
			{
				cout << "[Verifier] Running Verification: ECDSA (secp256r1)" << endl;
				isValid = verifyECDSA(digest, SHA256_DIGEST_LENGTH, msg.header.signature, sigLen, publicKey);
				break;
			}
			case SIG_ALGO_FALCON512:
			{
				cout << "[Verifier] Running Verification: FN-DSA-512 (Falcon-512)" << endl;
				isValid = verifyOQS(OQS_SIG_alg_falcon_512, digest, SHA256_DIGEST_LENGTH, msg.header.signature, sigLen, publicKey);
				break;
			}
			case SIG_ALGO_MLDSA44:
			{
				cout << "[Verifier] Running Verification: ML-DSA-44" << endl;
				isValid = verifyOQS(OQS_SIG_alg_ml_dsa_44, digest, SHA256_DIGEST_LENGTH, msg.header.signature, sigLen, publicKey);
				break;
			}
			default:
				cerr << "[Verifier] Unknown Algorithm ID: " << static_cast<int>(msg.header.sigAlgo) << endl;
				break;
		}

		auto end_time = chrono::steady_clock::now();
		TraceEvent(_NTO_TRACE_INSERTUSRSTREVENT, TRACE_EVT_SIG_VERIFY_END, "SIG_VERIFY_END");
		metrics.signatureVerifyUs = chrono::duration_cast<chrono::microseconds>(end_time - start_time).count();

		cout << "[Verifier] Signature Verification Time: "
			 << (metrics.signatureVerifyUs / 1000.0) << " ms (" << metrics.signatureVerifyUs << " us) | Wire Sig Size: "
			 << metrics.signatureSize << " B | Public Key Size: " << metrics.publicKeySize << " B" << endl;

		if (isValid)
		{
			cout << "[Verifier] Signature Verified. Staging decrypted package to " << decryptedTarPath << endl;

			// If raw file was an in-place bin, delete it now to immediately reclaim RAM
			if (msg.type == MSG_VERIFY_FILE_ZEROCOPY && msg.filepath != decryptedTarPath) {
				std::remove(msg.filepath);
			}

			std::remove(decryptedTarPath.c_str());

			ifstream src(tempFilePath, ios::binary);
			ofstream dst(decryptedTarPath, ios::binary | ios::trunc);

			if (src && dst)
			{
				dst << src.rdbuf();
				src.close();
				dst.close();
				std::remove(tempFilePath.c_str());
			}
			else
			{
				cerr << "[Verifier] Failed to copy decrypted file to destination: " << strerror(errno) << endl;
				std::remove(tempFilePath.c_str());
				sendReply(rcvid, -1, metrics);
				return;
			}

			sendReply(rcvid, 0, metrics);
		}
		else
		{
			cerr << "[Verifier] Signature Verification FAILED." << endl;
			std::remove(tempFilePath.c_str());
			sendReply(rcvid, -1, metrics);
		}
	}
	else
	{
		MsgError(rcvid, ENOSYS);
	}
}

int main()
{
	try
	{
		UpdateVerifier verifier;
		verifier.run();
	}
	catch (const exception &e)
	{
		cout << "[Verifier] Fatal error : " << e.what() << endl;
		return 1;
	}
	return 0;
}

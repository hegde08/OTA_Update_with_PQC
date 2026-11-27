# Post-Quantum Secure Over-the-Air (OTA) Firmware Update Architecture

A hybrid **Post-Quantum Cryptographic (PQC)** and classical cryptographic Over-the-Air (OTA) firmware update architecture designed for multi-ECU embedded automotive systems. The testbed demonstrates end-to-end encrypted, authenticated, and rollback-protected firmware delivery across a **Central Gateway ECU (Raspberry Pi 4 running QNX Neutrino RTOS 8.0)** and a constrained **End ECU (STM32F303RE ARM Cortex-M4F)** over **UART**.

---

## System Architecture

<img width="3799" height="8191" alt="Functional diagram" src="https://github.com/user-attachments/assets/6d944b81-27d7-4271-8aaf-bc9922811033" />

**Key Features**
**Cryptographic Agility (Gateway):** Supports runtime selection between classical ECDSA (secp256r1) and NIST-standardized Post-Quantum algorithms FN-DSA-512 (Falcon-512) and ML-DSA-44 (Dilithium2) via liboqs and OpenSSL 3.x (oqsprovider).

**SHA-256 Pre-Hashing:** Hashes large multi-hundred-megabyte QNX filesystem archives prior to PQC/classical signature generation and verification so that comparison between ECDSA and PQ Signature Algorithm in Reasonable way as PQ Signature Algorithms include Hashign inside them and ECDSA needs explicit hashing

**Authenticated Payload Encryption:** Uses AES-256-GCM (with 96-bit random IV and 128-bit authentication tag) for Gateway OTA packages and AES-256-CBC (via ST CMOX) for End ECU firmware images.

**Dual-Bank A/B Fail-Safe Updates (Gateway):** Dynamically mounts Bank A (/dev/sd0.lnxdata.3) or Bank B (/dev/sd0.lnxdata.4) at boot using uEnv.txt (ota_bank=A|B) and switches banks atomically upon verification.

**Anti-Rollback Protection:**

**Gateway:** Enforces monotonic semantic version checks against /var/version/gateway_version before accepting updates.

**End ECU:** Validates Security Version Numbers (SVN) stored in dedicated Flash metadata sectors (VERSION_INFO at 0x08027800).

**Zero-Copy / Shared-Memory IPC Pipeline:** Decouples network download (download_agent), cryptographic verification (verifier), and flash/UART installation (writer) into isolated QNX processes.

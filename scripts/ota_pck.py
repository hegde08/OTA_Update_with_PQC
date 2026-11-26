import os
import sys
import shutil
import struct
import tarfile
import hashlib
import argparse
import subprocess
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.asymmetric import utils
import signing_end_ecu

# --- Target ECU Configuration (Update paths here as needed) ---
ECU_CONFIG = {
    "gateway": {
        "name": "Central Gateway ECU (Raspberry Pi 4 / QNX)",
        "update_type": 1,
        "input_files": [
            "../Source_Code_Gateway/project_minimal/images/build/ifs-rpi4.ui",
            "../Source_Code_Gateway/project_minimal/images/build/rootfs.img"
        ]
    },
    "end_ecu": {
        "name": "End ECU (STM32F303RE)",
        "update_type": 2,
        "input_files": [
            "Application.sig",
            "Application_OTA_Encrypted.bin"
        ]
    }
}

AES_KEY_PATH = "../keys/gateway/aead_key.bin"
SIGNATURE_FILE = "signature.sig"
TEMP_DIGEST_FILE = "digest.bin"
TEMP_ARCHIVE = "sign_pack.tar.gz"
OUTPUT_FILE = "ota_package.bin"

# Target destination folder path
DESTINATION_DIR = "../Server"

# Algorithm Constants (Must match enum SigAlgo in shared_ipc.hpp)
SIG_ALGO_ECDSA = 0
SIG_ALGO_FALCON512 = 1
SIG_ALGO_MLDSA44 = 2

# Internal Key Mapping
ALGO_CONFIG = {
    "ecdsa": {
        "id": SIG_ALGO_ECDSA,
        "name": "ECDSA (secp256r1)",
        "key_path": "../keys/gateway/private_key_ecdsa.pem",
        "expected_sig_size": 64    # Raw 32-byte r || 32-byte s
    },
    "falcon": {
        "id": SIG_ALGO_FALCON512,
        "name": "FN-DSA-512 (Falcon-512)",
        "key_path": "../keys/gateway/private_key_fn_dsa.pem",
        "expected_sig_size": 666   # Falcon-512 signature
    },
    "mldsa": {
        "id": SIG_ALGO_MLDSA44,
        "name": "ML-DSA-44",
        "key_path": "../keys/gateway/private_key_ml_dsa.pem",
        "expected_sig_size": 2420  # ML-DSA-44 (Dilithium2) signature
    }
}

# Buffer size matching shared_ipc.hpp (constexpr size_t MAX_SIGNATURE_SIZE = 2420)
MAX_SIGNATURE_SIZE = 2420
MAGIC_NUMBER = 0x4F544131  # "OTA1"

# Struct format: <IIQBB I16s12s16s2420s
HEADER_FORMAT = f"<IIQBB I16s12s16s{MAX_SIGNATURE_SIZE}s".replace(" ", "")
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


def cleanup_temp_files(remove_local_output=False):
    """Removes intermediate .tar.gz, .sig, digest.bin, End ECU staging files, and optionally local .bin."""
    files_to_remove = [
        TEMP_ARCHIVE,
        SIGNATURE_FILE,
        TEMP_DIGEST_FILE,
        "Application.sig",
        "Application_OTA_Encrypted.bin"
    ]
    if remove_local_output:
        files_to_remove.append(OUTPUT_FILE)

    for filepath in files_to_remove:
        if os.path.exists(filepath):
            os.remove(filepath)


def compute_sha256(filepath):
    """Computes SHA-256 hash in 1MB chunks."""
    hasher = hashlib.sha256()
    with open(filepath, "rb") as f:
        while chunk := f.read(1024 * 1024):
            hasher.update(chunk)
    return hasher.digest()


def sign_payload_digest(digest_bytes, algo_id, key_path, expected_len):
    """Signs the pre-computed 32-byte SHA-256 digest using system OpenSSL."""
    with open(TEMP_DIGEST_FILE, "wb") as f:
        f.write(digest_bytes)

    # Prefer system OpenSSL over QNX SDP host OpenSSL so oqsprovider resolves cleanly
    openssl_bin = "/usr/bin/openssl" if os.path.exists("/usr/bin/openssl") else "openssl"

    # Strip QNX SDP OpenSSL environment overrides if qnxsdp-env.sh is active
    clean_env = os.environ.copy()
    for var in ("OPENSSL_CONF", "OPENSSL_MODULES", "LD_LIBRARY_PATH"):
        clean_env.pop(var, None)

    if algo_id == SIG_ALGO_ECDSA:
        openssl_cmd = [
            openssl_bin, "pkeyutl",
            "-sign",
            "-inkey", key_path,
            "-in", TEMP_DIGEST_FILE,
            "-out", SIGNATURE_FILE
        ]
    elif algo_id in (SIG_ALGO_FALCON512, SIG_ALGO_MLDSA44):
        openssl_cmd = [
            openssl_bin, "pkeyutl",
            "-sign",
            "-inkey", key_path,
            "-rawin",
            "-in", TEMP_DIGEST_FILE,
            "-out", SIGNATURE_FILE,
            "-provider", "oqsprovider",
            "-provider", "default"
        ]
    else:
        raise ValueError(f"Unknown signature algorithm ID: {algo_id}")

    try:
        subprocess.run(openssl_cmd, check=True, env=clean_env)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"OpenSSL signing command failed: {e}")

    with open(SIGNATURE_FILE, "rb") as sf:
        sig_data = sf.read()

    # If ECDSA, convert DER format to raw 64-byte (r || s)
    if algo_id == SIG_ALGO_ECDSA:
        r, s = utils.decode_dss_signature(sig_data)
        sig_data = r.to_bytes(32, byteorder="big") + s.to_bytes(32, byteorder="big")

    if len(sig_data) != expected_len:
        raise ValueError(
            f"Signature length mismatch! Expected {expected_len} bytes, got {len(sig_data)} bytes."
        )

    return sig_data


def create_ota_package(ecu_key, algo_key, version_str):
    ecu_cfg = ECU_CONFIG[ecu_key]
    update_type = ecu_cfg["update_type"]
    input_files = ecu_cfg["input_files"]

    # For end_ecu, outer header version doesn't matter (STM32 checks internal SVN)
    effective_version = version_str if version_str else "0.0.0"
    version_bytes = effective_version.encode("utf-8")

    cfg = ALGO_CONFIG[algo_key]
    algo_id = cfg["id"]
    algo_name = cfg["name"]
    key_path = cfg["key_path"]
    expected_sig_size = cfg["expected_sig_size"]

    moved_to_server = False

    try:
        print("==================================================")
        print(f" Target ECU:    {ecu_cfg['name']} (UpdateType: {update_type})")
        print(f" Header Ver:    {effective_version}" + (" (ignored by End ECU)" if update_type == 2 else ""))
        print(f" Target Scheme: {algo_name} (ID: {algo_id})")
        print(f" Private Key:   {key_path}")
        print(f" Destination:   {os.path.join(DESTINATION_DIR, OUTPUT_FILE)}")
        print("==================================================")

        # 0. Pre-step for End ECU (UpdateType == 2): Sign & encrypt STM32 binary first
        if update_type == 2:
            print("0. Running End ECU signing and encryption (signing_end_ecu.py)...")
            signing_end_ecu.main()
            print("   -> End ECU artifacts generated successfully.")

        # 1. Create Tar Archive
        print("1. Creating TAR.GZ archive...")
        with tarfile.open(TEMP_ARCHIVE, "w:gz") as tar:
            for file in input_files:
                if not os.path.exists(file):
                    raise FileNotFoundError(f"Input payload file not found: '{file}'")
                tar.add(file, arcname=os.path.basename(file))
                print(f"   -> Added {file} to archive")

        with open(TEMP_ARCHIVE, "rb") as f:
            payload_data = f.read()

        # 2. SHA-256 Pre-hashing
        print("2. Generating SHA-256 Digest of archive...")
        digest = compute_sha256(TEMP_ARCHIVE)
        print(f"   -> SHA-256: {digest.hex()}")

        # 3. Signature Generation
        print(f"3. Generating {algo_name} signature...")
        if not os.path.exists(key_path):
            raise FileNotFoundError(f"Private key file not found: {key_path}")

        raw_signature = sign_payload_digest(digest, algo_id, key_path, expected_sig_size)
        sig_len = len(raw_signature)
        print(f"   -> Signature generated: {sig_len} bytes")

        padded_signature = raw_signature.ljust(MAX_SIGNATURE_SIZE, b"\x00")

        # 4. Encryption
        print("4. Encrypting Payload (AES-256-GCM)...")
        if not os.path.exists(AES_KEY_PATH):
            raise FileNotFoundError(f"AES key not found: {AES_KEY_PATH}")

        with open(AES_KEY_PATH, "rb") as kf:
            aes_key = kf.read()

        if len(aes_key) != 32:
            raise ValueError(f"AES key must be exactly 32 bytes. Found {len(aes_key)} bytes.")

        iv = os.urandom(12)
        aesgcm = AESGCM(aes_key)
        ciphertext = aesgcm.encrypt(iv, payload_data, None)

        encrypted_payload = ciphertext[:-16]
        gcm_tag = ciphertext[-16:]

        # 5. Pack Binary Header
        print("5. Packing Binary Header...")
        padded_version = version_bytes[:15].ljust(16, b"\x00")

        binary_header = struct.pack(
            HEADER_FORMAT,
            MAGIC_NUMBER,
            HEADER_SIZE,
            len(encrypted_payload),
            update_type,
            algo_id,
            sig_len,
            padded_version,
            iv,
            gcm_tag,
            padded_signature
        )

        # 6. Write and move final package to ../Server
        print("6. Writing final OTA package...")
        os.makedirs(DESTINATION_DIR, exist_ok=True)
        final_dest = os.path.join(DESTINATION_DIR, OUTPUT_FILE)

        with open(OUTPUT_FILE, "wb") as out_file:
            out_file.write(binary_header)
            out_file.write(encrypted_payload)

        shutil.move(OUTPUT_FILE, final_dest)
        moved_to_server = True

        print(f"\n[DONE] Successfully generated '{final_dest}'!")
        print(f"Target ECU:   {ecu_key} (updateType={update_type})")
        print(f"Header Size:  {HEADER_SIZE} bytes")
        print(f"Payload Size: {len(encrypted_payload)} bytes")

    finally:
        cleanup_temp_files(remove_local_output=not moved_to_server)
        print("[CLEANUP] Removed temporary .tar.gz, .sig, and .bin artifacts from working directory.")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Package, Hash, Sign, and Encrypt OTA images for Gateway or End ECU."
    )
    parser.add_argument(
        "-e", "--ecu",
        type=str.lower,
        required=True,
        choices=["gateway", "end_ecu"],
        help="Target ECU: 'gateway' (updateType=1) or 'end_ecu' (updateType=2)."
    )
    parser.add_argument(
        "-a", "--algo",
        type=str.lower,
        required=True,
        choices=["ecdsa", "falcon", "mldsa"],
        help="Signature scheme: 'ecdsa', 'falcon', or 'mldsa'."
    )
    parser.add_argument(
        "-v", "--version",
        type=str,
        default=None,
        help="Firmware version string (required when --ecu gateway, e.g., '1.0.6')."
    )

    args = parser.parse_args()
    if args.ecu == "gateway" and not args.version:
        parser.error("--version (-v) is required when --ecu is 'gateway'.")

    return args


if __name__ == "__main__":
    args = parse_args()

    try:
        create_ota_package(args.ecu, args.algo, args.version)
    except Exception as e:
        print(f"\n[ERROR] Packing failed: {e}", file=sys.stderr)
        sys.exit(1)
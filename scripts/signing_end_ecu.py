from ecdsa import SigningKey, VerifyingKey, BadSignatureError, NIST256p
try:
    from Crypto.Cipher import AES
    from Crypto.Util.Padding import pad
    from Crypto.Random import get_random_bytes
except ImportError:
    from Cryptodome.Cipher import AES
    from Cryptodome.Util.Padding import pad
    from Cryptodome.Random import get_random_bytes

from ecdsa.util import sigdecode_string
import hashlib
from binary_to_hex_text import convert_binary_to_hex

class Ed25519Signer:
    def __init__(self, key_path: str, public_key_path: str):
        self.private_key = self._load_private_key(key_path)
        self.public_key = self._load_public_key(public_key_path)

    def _load_private_key(self, path: str) -> SigningKey:
        with open(path, "r") as f:
            sk = f.read()
            sk_bytes = bytes.fromhex(sk)
        return SigningKey.from_string(sk_bytes, curve = NIST256p)

    def _load_public_key(self, path: str) -> VerifyingKey:
        with open(path, "r") as f:
            hex_key = f.read().strip()
        key_bytes = bytes.fromhex(hex_key)
        return VerifyingKey.from_string(key_bytes, curve=NIST256p)

    def sign(self, message: bytes) -> bytes:
        return self.private_key.sign(message, hashfunc=hashlib.sha256)

    def convert_to_bytes(self, signature: bytes):
        r, s = sigdecode_string(signature, self.private_key.curve.order)
        r_bytes = r.to_bytes((r.bit_length() + 7) // 8, byteorder='big')
        s_bytes = s.to_bytes((s.bit_length() + 7) // 8, byteorder='big')
        final_sig = r_bytes + s_bytes
        return final_sig

    def verify(self, message: bytes, signature: bytes) -> bool:
        try:
            return self.public_key.verify(signature, message, hashfunc=hashlib.sha256)
        except BadSignatureError:
            return False

def read_hex_file(file_path: str) -> bytes:
    with open(file_path, "rb") as f:
        return f.read()

def write_hex_file(data: bytes, file_path: str):
    with open(file_path, "wb") as f:
        f.write(data)

def convert_to_text(file_path: str, file_path_out: str):
    with open(file_path, 'rb') as bin_file:
        binary_data = bin_file.read()
    hex_data = binary_data.hex()
    with open(file_path_out, 'w') as f:
        f.write(hex_data)

def encrypt_payload(plaintext: bytes, key_path: str) -> bytes:
    """Encrypts the plaintext using AES-256-CBC with a constant IV."""
    
    # Read the hex string from the text file and convert to bytes
    with open(key_path, "r") as f:
        hex_key = f.read().strip()
    
    aes_key = bytes.fromhex(hex_key)

    print(f"Loaded AES Key: {aes_key.hex()}")
    
    if len(aes_key) != 32:
        raise ValueError(f"AES key must be exactly 32 bytes for AES-256. Found {len(aes_key)} bytes.")

    # Using the constant IV defined in the C firmware[cite: 8]
    fixed_iv = bytes([
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    ])

    # Initialize cipher in CBC mode[cite: 8]
    cipher = AES.new(aes_key, AES.MODE_CBC, fixed_iv)
    
    # Encrypt the plaintext[cite: 8]
    return cipher.encrypt(plaintext)

def main():
    message = read_hex_file("../Source_Code_End_Ecu/Application_OTA/Debug/Application_OTA.bin")
    signer = Ed25519Signer("../keys/end_ecu/ecdsa_private.txt", "../keys/end_ecu/ecdsa_public.txt")
    signature = signer.sign(message)
    write_hex_file(signature, "Application.sig")
    Verify = signer.verify(message, signature)
    print(Verify)

    aes_key_path = "../keys/end_ecu/aes_key.txt" 
    encrypted_message = encrypt_payload(message, aes_key_path)

    encrypted_out_path = "Application_OTA_Encrypted.bin"
    write_hex_file(encrypted_message, encrypted_out_path)
    print(f"Encryption successful. Encrypted binary saved to: {encrypted_out_path}")

if __name__ == "__main__":
    main()
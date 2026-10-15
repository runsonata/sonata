--[[
	Type definitions for @sonata/crypto, used by the Luau Language Server.
	Written into .sonata/types by `sn init --lsp`; edits are lost when they are regenerated.
]]

--[=[
	Hashing, message authentication, key derivation, encryption, signatures and secure random values.

	Keys, digests, ciphertexts and signatures are binary strings; functions with an optional `enc`
	argument can encode their output instead. Bad arguments raise; failures that depend on the data
	return `nil, message`.
]=]
local crypto = {}

--- `"md5"`, `"sha1"`, `"sha224"`, `"sha256"`, `"sha384"`, `"sha512"`, `"sha3-224"`, `"sha3-256"`, `"sha3-384"`, `"sha3-512"`, `"blake2b"` or `"blake2s"`.
export type HashAlgorithm = string

export type Encoding = "hex" | "base64" | "base64url" | "base32"

export type CipherOptions = {
	--- Additional data that is authenticated but not encrypted (authenticated ciphers only).
	aad: string?,
	--- An explicit IV/nonce. Without it, a random one is generated and prepended to the result.
	iv: string?,
}

export type TotpOptions = {
	--- Default 6.
	digits: number?,
	--- Seconds; default 30.
	period: number?,
	--- Default `"sha1"`.
	hash: HashAlgorithm?,
	--- Unix seconds; default the current time.
	time: number?,
}

--- The supported hash algorithms.
crypto.hashes = (nil :: any) :: { string }

--- The supported cipher names.
crypto.ciphers = (nil :: any) :: { string }

--- `true` if AES runs on dedicated CPU instructions (AES-NI).
crypto.hardwareAes = (nil :: any) :: boolean

--- The digest of `data`.
function crypto.hash(algorithm: HashAlgorithm, data: string, enc: Encoding?): string
	return nil :: any
end

--- An HMAC of `data` with `key`.
function crypto.hmac(algorithm: HashAlgorithm, key: string, data: string, enc: Encoding?): string
	return nil :: any
end

--- Compares two strings in constant time (for equal lengths).
function crypto.equals(a: string, b: string): boolean
	return nil :: any
end

--- PBKDF2 with the given hash. `length` is the number of output bytes (up to 65536).
function crypto.pbkdf2(
	algorithm: HashAlgorithm,
	password: string,
	salt: string,
	iterations: number,
	length: number,
	enc: Encoding?
): string
	return nil :: any
end

--[=[
	Like `pbkdf2`, but picks the iteration count so the derivation takes about `ms` milliseconds.
	Returns the key and the iteration count used.
]=]
function crypto.pbkdf2Timed(
	algorithm: HashAlgorithm,
	password: string,
	salt: string,
	ms: number,
	length: number,
	enc: Encoding?
): (string, number)
	return nil :: any, nil :: any
end

--- HKDF (RFC 5869): derives `length` bytes from input key material.
function crypto.hkdf(
	algorithm: HashAlgorithm,
	ikm: string,
	salt: string?,
	info: string?,
	length: number,
	enc: Encoding?
): string
	return nil :: any
end

--- scrypt. `N` must be a power of two ≥ 2, `r` in 1..256, `p` in 1..1024, and N × r at most 2^20.
function crypto.scrypt(password: string, salt: string, N: number, r: number, p: number, length: number, enc: Encoding?): string
	return nil :: any
end

--[=[
	Hashes a password for storage with PBKDF2-SHA256 and a random salt, tuned to take about `ms`
	milliseconds (default 100). Returns `$pbkdf2-sha256$<iterations>$<salt>$<hash>`.
]=]
function crypto.hashPassword(password: string, ms: number?): string
	return nil :: any
end

--- Checks a password against a string produced by `hashPassword`. Malformed strings give `false`.
function crypto.verifyPassword(password: string, encoded: string): boolean
	return nil :: any
end

--[=[
	Encrypts `data`. Unless `options.iv` is given, a random IV/nonce is prepended to the result.
	Authenticated ciphers (`*-gcm`, `*-poly1305`) append a 16-byte tag. Prefer
	`"xchacha20-poly1305"` or `"aes-256-gcm"`.
]=]
function crypto.encrypt(cipher: string, key: string, data: string, options: CipherOptions?): string?
	return nil :: any
end

--- Decrypts what `encrypt` produced. Returns `nil, "authentication failed"` if it was tampered with.
function crypto.decrypt(cipher: string, key: string, data: string, options: CipherOptions?): (string?, string?)
	return nil :: any
end

--[=[
	Generates a key pair and returns `publicKey, privateKey` (raw 32-byte strings). `ed25519` keys
	are for signatures, `x25519` keys for key agreement and sealing.
]=]
function crypto.keypair(kind: "ed25519" | "x25519"): (string, string)
	return nil :: any, nil :: any
end

--- An Ed25519 signature (64 bytes) of `message`.
function crypto.sign(privateKey: string, message: string, enc: Encoding?): string
	return nil :: any
end

--- Checks an Ed25519 signature.
function crypto.verify(publicKey: string, message: string, signature: string): boolean
	return nil :: any
end

--- X25519 key agreement. Returns `nil, "invalid public key"` for a degenerate key.
function crypto.sharedSecret(privateKey: string, peerPublicKey: string, enc: Encoding?): (string?, string?)
	return nil :: any
end

--- Encrypts `message` for the holder of an X25519 private key, without any prior exchange.
function crypto.seal(recipientPublicKey: string, message: string): string?
	return nil :: any
end

--- Decrypts a sealed message. Returns `nil, message` if it doesn't authenticate.
function crypto.open(privateKey: string, sealed: string): (string?, string?)
	return nil :: any
end

--- `n` random bytes (up to 1 MiB).
function crypto.randomBytes(n: number, enc: Encoding?): string
	return nil :: any
end

--- A uniformly distributed integer between `min` and `max`, inclusive.
function crypto.randomInt(min: number, max: number): number
	return nil :: any
end

--[=[
	A UUID string. Version 4 (default) is random, 7 sorts by creation time, 1 is time-based, and 3
	and 5 are name-based (`namespace` is a UUID or one of `"dns"`, `"url"`, `"oid"`, `"x500"`).
]=]
function crypto.uuid(version: number?, namespace: string?, name: string?): string
	return nil :: any
end

--- A ULID: 26 characters of Crockford base32 that sort by creation time.
function crypto.ulid(): string
	return nil :: any
end

--- A time-based one-time password (RFC 6238). `secret` is the raw key.
function crypto.totp(secret: string, options: TotpOptions?): string
	return nil :: any
end

--- Encodes binary data.
function crypto.encode(format: Encoding, data: string): string
	return nil :: any
end

--- Decodes text, or returns `nil` if it isn't valid.
function crypto.decode(format: Encoding, text: string): string?
	return nil :: any
end

return crypto

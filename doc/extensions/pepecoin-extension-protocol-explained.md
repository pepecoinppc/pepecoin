# Pepecoin Extension Protocol

This _draft_ document proposes a protocol for encoding Layer 2 operations on
the Pepecoin blockchain. It provides:

- a compact binary encoding for operations represented after `OP_RETURN` in a
transaction script
 - a standardized script template using `OP_RETURN` so clients can identify and
 decode operations
 - an interoperability mechanism so clients can display operation metadata even
 if they do not understand the operation details
 - an upgrade path to evolve the protocol while remaining parsable by older
 clients

## Transaction Encoding

Command extensions MUST encode their payloads inside a single `OP_RETURN`
script. A transaction MUST NOT use multiple OP_RETURN outputs to fragment a
single protocol payload; each protocol payload is represented by at most one
`OP_RETURN` output.

`OP_RETURN` is widely supported and allowed in standard Pepecoin nodes. The
payload carried by `OP_RETURN` MUST be no larger than the common 80-byte
standard to remain relayable and economical; when larger payloads are needed use
off-chain storage (see IPFS section below) with a small on-chain pointer.

Script template (high level):

```
OP_RETURN <protocol-payload>
```

The `protocol-payload` is a compact binary blob (see "Binary Encoding" below).
Because `OP_RETURN` outputs are provably unspendable they are ideal for
carrying small protocol metadata without affecting UTXO set semantics.

## Protocol Encoding

The `protocol-payload` is intentionally compact and binary. Arguments are
positional and only deserializable into named values by a client that
understands the specific command language. `OP_RETURN` payloads MUST be raw
binary bytes. Do NOT base-encode (base58/base64/base32 or similar) the on- chain
payload. Base encodings waste space and are unnecessary inside `OP_RETURN`'s
binary field.

### Binary Encoding (compact TLV-like, big-endian):

We use a small TLV-style layout optimized for messages that fit inside the
typical 80-byte `OP_RETURN` limit. All integer fields use network byte order
(big-endian). Field encodings:

- Header (4 bytes):
  - protocol_version: 2 bytes (uint16 BE)
  - command_language: 1 byte (uint8)
  - command_id: 1 byte (uint8)

- Arguments: a sequence of argument entries. Each argument entry is encoded as
  `arg_type (1 byte) | arg_len (1 byte) | arg_value (arg_len bytes)`.
  - arg_type: small identifier chosen by the command language
  - arg_len: length of arg_value in bytes (0-255)
  - arg_value: raw bytes (semantic meaning depends on command language)

### Rules and rationale:

- Using 1-byte sized `command_language` and `command_id` reduces header size
  and is sufficient for a large set of registered languages and commands.
- The argument TLV is compact and self-describing; clients that don't
  understand an `arg_type` can skip it using `arg_len` and still parse later
  arguments.
- All integers are big-endian (network order) as specified above.

Checksum (CRC-8):

To provide a lightweight integrity check, a single trailing CRC-8 byte
SHALL be appended to the `protocol-payload`. The CRC covers the entire
payload (header + all argument TLVs) and is appended as the final byte inside
the `OP_RETURN` data. For `protocol_version = 0x0001`, producers MUST include
the CRC-8 byte; consumers MUST verify it.

Canonical profile:

- Name: CRC-8 (polynomial = `0x07`). This is a common CRC-8 profile used by many
libraries.
- Parameters: `poly=0x07`, `init=0x00`, `xorout=0x00`, `refin=false`,
  `refout=false`

Reference test vectors (CRC is computed over the raw bytes):

- Input (ASCII): `"123456789"` (hex: `31 32 33 34 35 36 37 38 39`) ->
  CRC-8 = `0xF4`
- Empty input (zero-length) -> CRC-8 = `0x00`

Producer / consumer semantics:

- Producers MUST compute and append the CRC-8 byte to the payload before
  placing it in `OP_RETURN` for `protocol_version = 0x0001`.
- Consumers MUST verify the CRC-8. If verification fails the payload MUST be
  treated as invalid and the command MUST be rejected. Consumers MUST NOT
  display any part of an invalid command; they MUST treat the entire command
  as invalid. Implementations MAY indicate that invalid payloads were seen
  but MUST NOT present partial decoded content from a rejected command.

Note that CRC-8 is not cryptographic and only guards against accidental
corruption or common transcription errors. For cryptographic guarantees, rely
on signatures, registry confirmations, or other cryptographic constructs.

Example (hex layout with CRC):

```
00 01 10 01 10 20 <32-byte-songid> CC
```

This represents:

- `protocol_version` = `0x0001` (two bytes: `00 01`)
- `command_language` = `0x10` (one byte)
- `command_id` = `0x01` (one byte)
- one argument: `arg_type` = `0x10`, `arg_len` = `0x20` (32 bytes), followed by 32-byte songId
- trailing CRC-8 (CC) calculated over all preceding bytes in the payload

Payload size and available space:

Let `MAX_OP_RETURN_RELAY` be the node's `OP_RETURN` byte limit (commonly
80). For a given transaction the maximum number of payload bytes available
for header and TLVs is:

```
available = MAX_OP_RETURN_RELAY - 1  # 1 byte reserved for CRC-8
- header_bytes (4)
- sum(2 + arg_len_i) for each TLV
```

Example calculation: header (4) + one TLV (2 + 32) + CRC (1) => total = 39
bytes, which is well under the common 80-byte relay limit.

## Client Responsibilities

Conforming clients MUST identify `OP_RETURN` outputs that begin with a valid
Header as defined above (2-byte version, 1-byte language, 1-byte command).
Clients must also parse argument TLV entries in order and skip unknown argument
types.

Clients MAY display a human-readable name for a registered command language or
command (looked up by code). They may also decode argument semantics for known
command languages and present them to users.

## Registering command languages and upgrade path

To register a new command language, submit a definition (including the assigned
`command_language` code, command ids, and argument types) to the Pepecoin
Extension Protocol Encyclopedia repository. Implementations SHOULD reserve a
small block of well-known `command_language` values for core or widely-adopted
features. `0x00`..`0x0F` are reserved for core protocol features and
widely-adopted extensions.

For example, The Digital Assets Protocol (DAP) uses `command_language` `0x10`
(see `pepecoin-dap-proposal.md`).

The registry SHOULD publish assigned values and the process for requesting a
`command_language` value outside the reserved core block.

## Upgrade path and versioning

Clients MUST check `protocol_version` (uint16). Clients SHOULD understand how to
ignore unknown later versions as follows: if a client encounters an unknown
`protocol_version` it MUST attempt to parse the header and all argument TLVs;
unknown fields should be skipped using the `arg_len` value. This allows
forward-compatible parsing when new optional fields are added.

If a breaking change is introduced, `protocol_version` will increment
monotonically. Non-breaking additions (new optional argument types) do not
require a version bump.

## IPFS and off-chain storage

Because on-chain space is limited and costly, store large or frequently changing
content off-chain and include an on-chain pointer in the argument TLV. We
recommend using IPFS (CIDv1, base32) for content addressing. Example argument
type for IPFS pointers:

- arg_type = 0xF0 is an IPFS CID (variable length). `arg_value` contains the
  binary CID bytes. For human-friendly displays the base32 string can be
  derived from the CID bytes.

When including an IPFS pointer, the on-chain payload can remain tiny while
still referencing arbitrarily large resources. Registries and clients SHOULD
fetch content from IPFS to validate or present the referenced data.

## Security considerations

Relying on registries and off-chain IPFS content introduces trust and
availability trade-offs. Clients SHOULD clearly indicate when displayed data
is off-chain and whether it has been validated by a trusted registry.

## Implementation Guidelines

### Compression guidance

When producers want to pack textual or structured metadata into the limited
`OP_RETURN` payload, compression MAY be used to save space. However, because
compression can add overhead and introduces decompression work for consumers,
the protocol defines a canonical compressed-argument TLV and simple heuristics
to ensure interoperability and safety.

Compressed argument TLV (arg_type = `0xFE`)

- arg_type = 0xFE (Compressed argument)
- arg_len = N (1 + 1 + 2 + len(comp_payload))
- arg_value layout:
  - comp_alg (1 byte): algorithm identifier (see table below)
  - comp_flags (1 byte): algorithm-specific flags (reserved, set to 0x00 unless
    otherwise specified)
  - orig_len (2 bytes BE): original uncompressed length (0 allowed if unknown)
  - comp_payload (remaining bytes): compressed data bytes

Suggested `comp_alg` values:

- `0x01` = gzip (RFC 1952) — widely available, deterministic if consistent
  parameters are used
- `0x02` = zlib/deflate (RFC 1950/1951)
- `0x03` = brotli (RFC 7932) — often better compression on short text but
  more CPU intensive

### Producer heuristics

Producers SHOULD only use compression when the compressed payload (including the
4-byte header, TLV overhead, CRC byte, and compression headers) is strictly
smaller than the uncompressed data and fits within the node's relay limit
(typically 80 bytes).

Recommended rule: attempt compression and include a compressed TLV only if
compressed_size + (TLV_overhead = 4) < uncompressed_size and compressed_size +
header_bytes + 1 (CRC) <= `MAX_OP_RETURN_RELAY`.

For very small payloads (< 32 bytes) compression is unlikely to help. Use
compression only if it helps.

### Consumer safety and DoS mitigations

Consumers MUST verify the CRC-8 before attempting decompression. If CRC fails,
treat the payload as invalid.

Implementations MUST limit decompressed output to a reasonable bound (for
example, MAX_DECOMPRESSED_BYTES = 4096 bytes) and abort decompression if the
limit is exceeded.

Implement timeouts/resource limits for decompression. If decompression fails,
the arg SHOULD be treated as invalid and skipped, or the command MAY be
considered invalid depending on command-language semantics.

Unknown `comp_alg` values MUST be rejected for that argument.

### Determinism

Producers SHOULD use canonical compressor parameters to reduce variance. For
example, for gzip use the default wrapper with compression level 6. For brotli,
use quality=4.
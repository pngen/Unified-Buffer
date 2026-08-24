# Security

This document describes the adversarial model Unified Buffer 1.0.0 is designed against and the protections that exist,
along with their limits. It is a *behavioral* security description, not a cryptographic promise. In particular: the
integrity mechanism is **CRC-32C for content checks** and **a metadata checksum for metadata** — it is **not** a
cryptographic hash and provides **no** authenticity, confidentiality, or tamper-resistance.

The content checksum helper is [core/integrity.hpp](../include/unified_buffer/core/integrity.hpp). The metadata checksum
is computed in [src/export.cpp](../src/export.cpp) (`meta_crc_of`).

---

## Scope and non-goals

Unified Buffer is an in-process library. It is not a security boundary between mutually hostile processes. It is designed
to make *accidental* misuse and *untrusted input* fail safely (reject loudly, never dereference a stale or forged handle),
not to defend against a determined attacker that already has code execution in the process.

Non-goals: no TLS, no authentication, no encryption of data at rest or in transit, no key management, no attestation, no
signature over descriptors, and no guarantee against a caller that deliberately lies about memory it owns.

---

## Adversarial model

The following classes of attack or misuse are the threat model. Each is described below with what the runtime does (and does
not) do.

| Threat | What the runtime does | Limit |
| --- | --- | --- |
| Integer overflow in sizes/offsets | Checked arithmetic; rejects with `overflow` or `bounds_error`. | Only on metadata paths; a caller-provided pointer is trusted. |
| Use-after-free | Generation + `finalized` flag + registry removal; stale references rejected. | Only *within* the runtime; it cannot stop a caller from keeping a raw pointer past release and using it. |
| Stale handle / generation | `stale_handle` / `stale_generation` rejection. | Detects library misuse; does not make a stale *data pointer* safe. |
| Double free | `finalized` atomic flag; backing cleared after release; registry entry erased. | Prevents double-free of a *runtime-owned* backing. |
| Forged export descriptors | Format/version checks, namespace authorization, size bounds, alignment checks, backend handle validation. | Descriptors are serialized by the caller; an attacker can craft one, but the runtime validates fields before acting. |
| Path traversal on file import | Rejects `..`, traversal segments, long paths, and paths outside the base dir. | Only for the `MMAP_STORAGE` backend. |
| Oversized allocation | Rejects `size == 0` and `size > 2^48`; enforces domain caps and namespace quotas. | The absolute size bound is `2^48`, not the actual memory available; a backend may still fail with `out_of_capacity`. |
| Unbounded pinned / shared memory | Per-domain capacity caps and namespace quotas. | Caps default to `0` (unlimited) unless configured. |
| Cross-namespace import | Requires source/target `allow_import` and rejects a cross-namespace import. | Namespace policy is per-namespace configuration. |
| Malformed input | Structured `ErrorCode`s; no operation dereferences an unvalidated descriptor field. | A malformed *data payload* is the caller responsibility. |

---

## Integer overflow

Every size, offset, and alignment computation on a metadata path uses [checked_math.hpp](../include/unified_buffer/core/checked_math.hpp).

- `add_overflow` / `mul_overflow` detect wrap-around and report it instead of producing a wrapped result.
- `align_up` returns `nullopt` on overflow or for a non-power-of-two alignment.
- `range_in_bounds(offset, length, total)` returns false if `offset + length` overflows or exceeds `total`.
- `validate_request` rejects a zero size, a size beyond `2^48`, a non-power-of-two alignment, an alignment above `4096`,
  and an `align_up` overflow.
- A `view` and `copy_to` / `copy_from` use `range_in_bounds` and explicit length checks so an offset plus length cannot wrap
  past the buffer.

These checks prevent a size/offset overflow from corrupting accounting or allowing an out-of-bounds access. They are *not* a
defense against a caller that supplies a bogus payload pointer directly.

---

## Use-after-free, stale handle, and stale generation

The runtime never dereferences a buffer it no longer trusts:

- A handle is only usable while it holds the live control block and its stored generation equals the buffer current
  generation. A mismatch is `stale_generation`.
- A reference to a finalized or unknown buffer id is `stale_handle`.
- On finalization the registry entry is erased, so a subsequent `lookup` by that id returns `stale_handle`.
- The `finalized` atomic flag ensures the buffer backing is released at most once.

**Limit:** these guard the *runtime* bookkeeping. They do not make a raw data pointer safe. If you capture `host_data()` or
the mapped address, release the handle, and then dereference the saved address, that is a use-after-free that the runtime
cannot detect. The lifetime contract is yours to honor: an address is borrowed and tied to the handle.

---

## Forged export descriptors

`export_buffer` only produces a descriptor for a buffer flagged `exportable`; otherwise it returns `permission_failure`.
On `import`, the runtime validates the descriptor before acting:

- `format_version` must equal `1`.
- `size` must be non-zero and at most `2^48`.
- `alignment`, if set, must be a power of two.
- The backend is resolved from the `domain` and `backend`; an unknown or disabled domain is rejected.
- The `handle` is passed to the backend `import_handle`, which does its own checks (for shared: name is non-empty, at most
  512 characters, no path separators; for file: path is non-empty, at most 1024 characters, no traversal, beneath the base
  dir).
- **Cross-namespace import is refused.** A descriptor whose `ns` differs from the import target namespace is rejected with
  `permission_failure`.

**Limit:** the descriptor is produced and serialized by the caller. A malicious caller can craft a descriptor, but the
runtime validates every field it acts on before side effects. Descriptor integrity is not authenticated; an attacker who can
modify the descriptor in transit can still influence what is opened.

---

## Path traversal protection (file import)

The `MMAP_STORAGE` backend rejects a file handle that is unsafe:

- A path containing `..` or a backslash-based traversal segment is rejected as `import_failure`.
- A path longer than 1024 characters is rejected.
- A path that is not **beneath** the configured base directory is rejected (`path_is_beneath`).

This prevents an import from opening an arbitrary file outside the domain base directory. **Limit:** the check uses a string
prefix test on the resolved base directory; it guards against naive traversal but is not a substitute for running in a
restricted environment, and the file contents are still trusted per the caller policy.

---

## Oversized, unbounded, and quota'd allocations

- A request with `size == 0` or `size > 2^48` is rejected.
- `reserve_amount` enforces the per-domain capacity cap and the per-namespace quota before the backend allocates.
- If a cap or quota is exceeded, the operation returns `out_of_capacity` or `quota_exceeded` and never commits.

**Limit:** the per-domain caps and per-namespace quotas default to `0` (unlimited) unless you configure them. To bound
pinned and shared memory, set the relevant cap and quota in `RuntimeConfig` or `NamespaceConfig`; the runtime does not
impose a default pinned or shared ceiling.

---

## Cross-namespace import authorization

Import is authorized at the namespace level:

- The target namespace must exist.
- The source namespace must set `allow_import` (if it exists), and the target must set `allow_import`.
- A cross-namespace import (`desc.ns != ns`) is always refused with `permission_failure`.

This means an imported buffer cannot gain capabilities beyond its export and cannot be placed into a namespace that forbids
import. Import always mints a **fresh** `BufferId` for shared/file (it does not reuse the exporter id), and the imported
buffer never inherits the exporter runtime identity.

---

## Integrity: CRC-32C, not cryptography

The runtime does **not** use cryptographic hashing anywhere. The integrity facilities are:

- **Content checksum:** `crc32c(data, len)` computes CRC-32C (Castagnoli). `BufferHandle::checksum()` returns it for the
  buffer content. A self-check (`crc32c_self_test`) verifies the table against the known value for `123456789`
  (`0xE3069283`).
- **Metadata checksum:** `meta_crc_of(record)` hashes the id, generation, domain, size, and alignment. It is stored in
  `ExportDescriptor::integrity_crc` and compared by `BufferHandle::verify()`.

Both are **non-cryptographic** and provide:

- Corruption detection for *accidental* bit flips (checksum mismatch on verify).
- No authenticity (an attacker can recompute a valid CRC).
- No confidentiality (the data is not encrypted).
- No tamper-resistance (a CRC does not prevent a deliberate modification).

> **Do not use CRC-32C as a security control.** It is a fast checksum for integrity checks and self-consistency, not a MAC.

---

## Malformed input

All public operations accept structured inputs and return a structured `ErrorCode`. A malformed request, descriptor, view,
or copy bounds produces a specific error (`invalid_argument`, `bounds_error`, `alignment_error`, `overflow`,
`import_failure`, and so on) rather than crashing. The runtime does not assume a descriptor field is valid before checking
it, and it refuses to operate on a closed runtime (`closed`).

---

## Recommended hardening (application side)

- Set explicit per-domain caps (`host_cap`, `pinned_cap`, `device_cap`, `shared_cap`, `file_cap`) and per-namespace quotas
  so unbounded pinned/shared allocations are impossible.
- Treat every imported `handle` as untrusted; validate the descriptor fields before and after import.
- Never store a data pointer past the buffer lifetime; re-acquire it from the handle after any migration.
- Use `verify()` for explicit content/metadata integrity checks where it matters; do not rely on CRC for security.

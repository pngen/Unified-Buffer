# Lifecycle

This document describes the `BufferState` state machine in Unified Buffer 1.0.0: the exact legal transitions, how a
buffer is finalized, what happens at shutdown, and the stale-handle / stale-generation rules.

The state enum and its string form are in [core/types.hpp](../include/unified_buffer/core/types.hpp). The transition table
is implemented in `RuntimeImpl::valid_transition` ([src/runtime.cpp](../src/runtime.cpp)).

---

## The states

| `BufferState` | Meaning |
| --- | --- |
| `DECLARED` | The buffer record has been created (default initial state). |
| `RESERVED` | Capacity/quotas have been reserved for the allocation. |
| `ALLOCATED` | The backing has been allocated in a domain; the buffer is live and usable. |
| `MAPPED` | A CPU view is currently mapped. |
| `IN_USE` | A read/write/exclusive lease is active. |
| `EXPORTING` | The buffer is being exported. |
| `EXPORTED` | The buffer has been exported (descriptor produced). |
| `IMPORTING` | The buffer is being imported. |
| `IMPORTED` | The buffer was reconstructed from an export descriptor. |
| `COPYING` | A copy is in progress. |
| `MIGRATING` | A domain migration is in progress. |
| `RELEASING` | The buffer is being released. |
| `RELEASED` | The buffer has been released (finalized). |
| `FAILED` | A lifecycle operation failed. |
| `QUARANTINED` | The buffer was quarantined after a failure. |
| `INVALID` | The buffer is no longer usable. |

---

## Legal transition table

The following is the exact, exhaustive table encoded in `valid_transition`. A transition from a state to a state that is
not on the row is rejected with `ErrorCode::lifecycle_violation`.

| From (`BufferState`) | Legal destinations |
| --- | --- |
| `DECLARED` | `RESERVED`, `ALLOCATED`, `FAILED`, `INVALID` |
| `RESERVED` | `ALLOCATED`, `RELEASED`, `FAILED`, `INVALID` |
| `ALLOCATED` | `MAPPED`, `COPYING`, `IN_USE`, `EXPORTING`, `RELEASING`, `MIGRATING`, `RELEASED`, `FAILED`, `INVALID` |
| `MAPPED` | `ALLOCATED`, `IN_USE`, `COPYING`, `RELEASING`, `RELEASED`, `FAILED`, `INVALID` |
| `IN_USE` | `ALLOCATED`, `COPYING`, `RELEASING`, `RELEASED`, `FAILED`, `INVALID` |
| `EXPORTING` | `EXPORTED`, `ALLOCATED`, `FAILED` |
| `EXPORTED` | `ALLOCATED`, `RELEASED`, `FAILED` |
| `IMPORTING` | `IMPORTED`, `FAILED`, `INVALID` |
| `IMPORTED` | `ALLOCATED`, `RELEASED`, `FAILED`, `INVALID` |
| `COPYING` | `ALLOCATED`, `IN_USE`, `RELEASED`, `FAILED` |
| `MIGRATING` | `ALLOCATED`, `FAILED` |
| `RELEASING` | `RELEASED`, `FAILED` |
| `RELEASED` | `INVALID` |
| `FAILED` | `QUARANTINED`, `INVALID` |
| `QUARANTINED` | `INVALID` |
| `INVALID` | *(no legal destination)* |

Notes on the table:

- `INVALID` is terminal; nothing may transition out of it.
- `RELEASED` can only move to `INVALID` (it is effectively terminal short of that).
- `FAILED` can be quarantined or invalidated, but cannot return to a live state.
- `DECLARED` and `RESERVED` are the pre-allocation states; both can short-circuit to `FAILED` or `INVALID`.

---

## How transitions are enforced

`RuntimeImpl::set_state(ctl, to)` (and its alias `transition`) locks the control mutex, checks `valid_transition(from, to)`,
and either commits the new state or returns `ErrorCode::lifecycle_violation`. This is the authority for any staged
transition.

Several internal paths assign `Record::state` directly rather than going through `set_state`, but they never perform an
illegal move. The direct assignments in 1.0.0 are:

| Path | State set | Notes |
| --- | --- | --- |
| `allocate_record` | `ALLOCATED` | Fresh allocation becomes live. |
| `wrap_external` | `ALLOCATED` | External wrap becomes live. |
| `import_descriptor` | `IMPORTED` | Imported buffer starts as imported. |
| `map` | `MAPPED` | Only if `valid_transition` from the current state to `MAPPED` passes; otherwise state is left as is. |
| `acquire_read` / `acquire_write` / `acquire_exclusive` | `IN_USE` | When starting from `ALLOCATED` (and `MAPPED` for write/exclusive). |
| lease release (last lease cleared) | `ALLOCATED` | When `IN_USE` and no leases remain. |
| `BufferMap::release` (last map cleared) | `ALLOCATED` | When `MAPPED` and `map_count` reaches zero. |
| `migrate` (after success) | `ALLOCATED` | The migrated buffer is re-live with a bumped generation. |
| `finalize` | `RELEASED` | The buffer is finalized; the registry entry is erased. |

The `valid_transition` table governs what is legal; the direct assignments above are only used where the path is
initializing or concluding a buffer, and each is a legal move under the table.

---

## Reference counting and finalization

Every `BufferHandle` (and every active `BufferLease`, `BufferMap`, and `BufferView`) holds a reference to the buffer's
`Control` (`Control::refs`). The buffer is finalized **exactly once**, when the last reference is dropped.

- `Runtime::allocate` / `wrap_external` / `import` / `migrate` produce a handle that retains (`refs++`).
- `BufferHandle::share()` adds another handle (another `refs++`).
- `BufferHandle::release()` and the destructor drop the handle reference (`refs--`).
- `BufferLease`, `BufferMap`, and `BufferView` release call `drop_ref` when destroyed or released.

When `refs` transitions to zero (`drop_ref` sees `prev == 1`), `RuntimeImpl::finalize` runs. It:

1. Locks the control mutex and exchanges the `finalized` atomic flag (so it runs at most once).
2. If a backing is present and ownership is not `BORROWED`:
   - If the buffer is pool-eligible and pooling is enabled, returns the backing to the pool (`pool_put`). If the pool
     rejects it (over `max_objects` or `max_idle_bytes`), it is evicted (freed) directly.
   - Otherwise, free the backing via the domain `free` (after converting `ADOPTED` to `RUNTIME`). `BORROWED` backings are
     never freed; only the descriptor is cleared.
3. Sets `state = RELEASED`, releases the namespace allocation count, and erases the registry entry by `BufferId`.
4. Increments `total_frees` and decrements `active_buffers` / `outstanding_allocations`.

Because finalization is guarded by the `finalized` flag and clears the backing, a buffer is never finalized twice and a
backing is never freed twice.

---

## Deterministic finalization

Finalization is deterministic in the sense that it is driven entirely by explicit reference counts, never by a GC pass or
a background thread. The sequence is always:

1. The last handle/lease/map/view goes away (RAII or explicit `release()`).
2. `refs` reaches zero.
3. `finalize` runs synchronously in the calling thread and the backing is returned to the pool or freed.

Buffer handles are move-only and RAII, so ownership of the reference is unambiguous. There is no implicit leak and no
finalizer thread.

---

## Shutdown behaviour

`Runtime::shutdown()` (also called by the `Runtime` destructor and `RuntimeImpl::~RuntimeImpl`) is idempotent:

1. It atomically exchanges the `closed_` flag to `true`; if already closed, it returns immediately.
2. It snapshots every registered control block under the registry lock and calls `finalize(ctl, true)` for each
   (returning eligible buffers to the pool).
3. It trims the pool to zero (`pool_trim_to_zero`), freeing any remaining pooled backing.
4. It resets each backend (`cuda_device_`, `cuda_pinned_`, `shared_`, `file_`, `host_`).

After `shutdown()`, `Runtime::closed()` returns `true`, and any subsequent allocate/import/migrate returns
`ErrorCode::closed`. Existing `BufferHandle` objects that were already created are still valid (they hold a shared_ptr to
the implementation, which stays alive), but live buffers at shutdown are finalized.

Note: `shutdown()` returns to the pool any buffers that have references still outstanding; the finalization only frees or
returns the backing, and the handles' own destructors will later drop their reference without re-freeing (the `finalized`
flag is already set).

---

## Stale-handle and stale-generation rules

Every handle, lease, map, and view carries the `BufferId` and the `BufferGeneration` it was created with. The runtime
rejects a stale reference rather than dereferencing freed memory.

### `stale_handle`

Returned when:

- The handle is not alive (`!valid()` or no control block).
- The buffer was already finalized (`ctl->finalized` is true).
- The buffer is in a terminal state: `RELEASED`, `QUARANTINED`, or `INVALID`.
- The `BufferId` is not in the registry (it was finalized and erased).

### `stale_generation`

Returned when the handle's stored `buffer_generation` differs from the buffer's current `Record::generation`. The only
operation in 1.0.0 that advances a generation is **migration**. After a migration, any handle that still holds the old
generation is rejected with `stale_generation`.

### Where this is enforced

- `BufferHandle::validate_alive()` runs the checks above and is called by every real operation (`view`, `map`, `copy`,
  `checksum`, `verify`, `share`, `device_pointer`, and so on).
- `RuntimeImpl::lookup(id, expected_generation)` checks both the registry membership (`stale_handle`) and the generation
  (`stale_generation`); it is used by import of same-runtime descriptors.
- `Runtime::migrate` returns a **new** handle (with the new generation) rather than mutating the old one; the old handle is
  left as a stale-generation ledger entry.

Both rejection paths increment the corresponding counter (`stale_handle_rejections` or `stale_generation_rejections`) in
`RuntimeStats`.

---

## Invariants summary (lifecycle-specific)

- A buffer is finalized exactly once, when the last reference is dropped.
- No buffer transitions to a disallowed state; an illegal move returns `lifecycle_violation`.
- `INVALID` and adjacent terminal states are never re-entered.
- Shutdown is idempotent and safe to call from the destructor.
- A stale handle or generation is always rejected, never dereferenced.

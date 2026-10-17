#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>
#include <winternl.h> // NTSTATUS

#include "driver_types.hpp"

class c_memory {
public:
  c_memory() = default;
  ~c_memory() { detach(); }

  bool attach(uint32_t process_id);
  bool attach(std::wstring process_name);
  void detach();

  bool read(uint64_t src, void *dest, uint64_t size);
  bool write(void *src, uint64_t dest, uint64_t size);

  // Batched read — sends N entries in one driver call. Each entry's
  // status is written into the entry itself; the return value is true
  // iff the BATCH ENVELOPE round-tripped, regardless of how many
  // individual entries failed (check entries[i].status per entry).
  // count must be in (0, kMaxBatchEntries]; entries must point to
  // count valid mem_read_batch_entry structs in our address space.
  bool read_batch(mem_read_batch_entry *entries, uint32_t count);

  template <typename T> T read(uint64_t src) {
    T buf{};
    read(src, &buf, sizeof(T));
    return buf;
  }

  template <typename T> bool write(uint64_t dest, T val) {
    return write(&val, dest, sizeof(T));
  }

  uint64_t get_module_base(std::wstring module_name);

  // Allocate RWX in the target process. The driver picks the base
  // address (returned). `protect` is accepted for source compat with
  // the legacy stub but ignored — matches comms + kernel behavior
  // (driver always RWX, skips a second protect call on the hot path).
  // Returns 0 if the driver rejects the request or the round trip
  // fails after retries.
  uint64_t alloc(uint64_t size, uint32_t protect = PAGE_READWRITE);
  bool free(uint64_t address);
  bool protect(uint64_t address, uint64_t size, uint32_t prot);
  bool hide_process(uint32_t pid);
  bool unhide_process(uint32_t pid);
  bool is_hidden(uint32_t pid);

  // Runtime override for the auth token. Delegates to astrum_cmd::set_token
  // so the global state (used by every c_request constructor by default)
  // stays the single source of truth. main.cpp's --cmd-token CLI path uses
  // astrum_cmd::set_token directly; this is here so external code that has
  // a c_memory instance can rotate the token without reaching into the
  // namespace. Mirrors comms/memory.hpp's API.
  void set_auth_token(uint32_t token) { astrum_cmd::set_token(token); }

  bool scatter_write(SCATTER_WRITE_ENTRY *entries, size_t count);
  bool scatter_read(SCATTER_READ_ENTRY *entries, size_t count);

  uint32_t get_pid() const { return m_pid; }
  uint32_t get_process_id(std::wstring process_name) const;

  void *get_peb() const { return m_proc_data.peb; }

  bool is_driver_connected() const { return m_connected; }

  bool try_connect();

  bool check_alive();

private:
  NTSTATUS dispatch(c_request *request);
  bool resolve_transport();
  bool test_proc_query(uint32_t pid, proc_info_request *out, const char *label);

  uint32_t m_pid = 0;
  bool m_connected = false;
  proc_info_request m_proc_data = {};

  using nt_call_t = NTSTATUS(__stdcall *)(void *, void *, void *, void *);
  nt_call_t m_transport_fn = nullptr;
  int m_transport_idx = -1;

  std::mutex m_dispatch_mtx;
};

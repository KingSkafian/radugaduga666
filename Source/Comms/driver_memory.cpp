#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h> // NTSTATUS

#include "driver_memory.hpp"

#ifndef STATUS_PROCEDURE_NOT_FOUND
#define STATUS_PROCEDURE_NOT_FOUND ((NTSTATUS)0xC000007AL)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#endif

// why: silenced; was writing transport diagnostics to driver_transport.log in cwd, leaked loader presence. Call sites kept intact for one-line revert.
static void drv_log(const char * /*fmt*/, ...) {
}

bool c_memory::resolve_transport() {
  if (m_transport_fn)
    return true;

  auto *ntdll = GetModuleHandleA("ntdll.dll");
  if (!ntdll) {
    drv_log("resolve_transport: ntdll.dll not found");
    return false;
  }

  // why: try multiple NT functions that hit different HAL dispatch table entries; driver hooks HalPrivateDispatchTable[116] and the NT->HAL index mapping is Windows-version dependent
  static const char *k_candidates[] = {
      "NtQueryAuxiliaryCounterFrequency",
      "NtConvertBetweenAuxiliaryCounterAndPerformanceCounter",
  };

  for (int i = 0; i < _countof(k_candidates); ++i) {
    auto fn =
        reinterpret_cast<nt_call_t>(GetProcAddress(ntdll, k_candidates[i]));
    drv_log("resolve_transport: %s -> %p", k_candidates[i],
            reinterpret_cast<void *>(fn));
    if (fn) {
      m_transport_fn = fn;
      m_transport_idx = i;
      return true;
    }
  }

  drv_log("resolve_transport: no NT function found");
  return false;
}

// why: SEH only, no objects requiring unwinding (MSVC C2712: can't mix __try with destructor-bearing locals). Called with m_dispatch_mtx held.
// Astrum protocol: c_request* in RDX. The struct's first 8 bytes are
// kAstrumRequestMagic; the kernel hook SEH-reads those to discriminate
// our calls from legit NtQueryAuxiliaryCounterFrequency callers.
using _dispatch_fn_t = NTSTATUS(__stdcall *)(void *, void *, void *, void *);
static NTSTATUS dispatch_inner(_dispatch_fn_t fn, int idx, c_request *request) {
  uint64_t dummy1 = 0, dummy2 = 0;
  NTSTATUS status = STATUS_UNSUCCESSFUL;
  __try {
    if (idx == 0) {
      // NtQueryAuxiliaryCounterFrequency: RCX=&dummy1, RDX=request
      status = fn(&dummy1, request, nullptr, nullptr);
    } else {
      // NtConvertBetweenAuxiliaryCounter...: RCX=nullptr, RDX=request
      status = fn(nullptr, request, &dummy1, &dummy2);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    status = (NTSTATUS)0xC0000005L;
  }
  return status;
}

NTSTATUS c_memory::dispatch(c_request *request) {
  if (!request)
    return STATUS_ACCESS_VIOLATION;

  if (!m_transport_fn && !resolve_transport())
    return STATUS_PROCEDURE_NOT_FOUND;

  // why: three threads (Worker, FrameData, AuraWrite) hit driver concurrently; concurrent syscalls collide in driver hook and reads return zeros. SEH lives in dispatch_inner so this function can legally hold the lock_guard.
  std::lock_guard<std::mutex> _lk(m_dispatch_mtx);
  return dispatch_inner(m_transport_fn, m_transport_idx, request);
}

bool c_memory::test_proc_query(uint32_t pid, proc_info_request *out,
                               const char *label) {
  *out = {};
  out->process_id = pid;

  // why: sentinel values detect whether driver modified fields
  out->peb = reinterpret_cast<void *>(0xDEAD);
  out->base_address = reinterpret_cast<void *>(0xDEAD);
  out->cr3 = 0xDEADDEADDEADDEADULL;

  auto request = c_request((e_command)astrum_cmd::query, out, sizeof(*out));
  NTSTATUS status = dispatch(&request);

  bool cr3_changed = (out->cr3 != 0xDEADDEADDEADDEADULL);
  bool any_changed = cr3_changed ||
                     out->peb != reinterpret_cast<void *>(0xDEAD) ||
                     out->base_address != reinterpret_cast<void *>(0xDEAD);

  drv_log("  %s pid=%u status=0x%08X peb=%p base=%p cr3=0x%llX changed=%d",
          label, pid, (unsigned)status, out->peb, out->base_address,
          (unsigned long long)out->cr3, any_changed);

  return any_changed && out->cr3 != 0 && out->cr3 != 0xDEADDEADDEADDEADULL;
}

bool c_memory::try_connect() {
  drv_log("=== try_connect ===");

  if (!resolve_transport()) {
    drv_log("try_connect: resolve_transport failed");
    return false;
  }

  // why: PID 4 (System) has valid CR3 but NULL peb/base_address
  proc_info_request info = {};
  bool ok = test_proc_query(4, &info, "System(4)");

  if (!ok) {
    DWORD self_pid = GetCurrentProcessId();
    ok = test_proc_query(self_pid, &info, "Self");
  }

  if (!ok && m_transport_idx == 0) {
    drv_log("try_connect: first transport failed, trying alternate...");
    auto *ntdll = GetModuleHandleA("ntdll.dll");
    auto fn =
        ntdll ? reinterpret_cast<nt_call_t>(GetProcAddress(
                    ntdll,
                    "NtConvertBetweenAuxiliaryCounterAndPerformanceCounter"))
              : nullptr;
    if (fn) {
      m_transport_fn = fn;
      m_transport_idx = 1;
      drv_log("try_connect: switched to NtConvertBetween... (%p)",
              reinterpret_cast<void *>(fn));
      ok = test_proc_query(4, &info, "System(4)-alt");
      if (!ok) {
        DWORD self_pid = GetCurrentProcessId();
        ok = test_proc_query(self_pid, &info, "Self-alt");
      }
    }
  }

  if (ok) {
    drv_log("try_connect: SUCCESS (transport_idx=%d)", m_transport_idx);
    m_connected = true;
    return true;
  }

  drv_log("try_connect: FAILED, driver did not modify proc_info fields");
  m_connected = false;
  return false;
}

bool c_memory::check_alive() {
  if (!m_transport_fn)
    return false;

  proc_info_request info = {};
  info.process_id = 4;
  info.cr3 = 0xDEADDEADDEADDEADULL;

  auto request = c_request((e_command)astrum_cmd::query, &info, sizeof(info));
  dispatch(&request);

  m_connected = (info.cr3 != 0 && info.cr3 != 0xDEADDEADDEADDEADULL);
  return m_connected;
}

bool c_memory::attach(uint32_t process_id) {
  if (!process_id)
    return false;

  if (!m_connected && !try_connect())
    return false;

  proc_info_request info = {};
  info.process_id = process_id;

  auto request = c_request((e_command)astrum_cmd::query, &info, sizeof(info));
  NTSTATUS status = dispatch(&request);

  // Gate on peb, not base_address: get_module_base walks PEB→Ldr manually,
  // so peb is the only field we actually consume. EAC-protected processes
  // (RustClient.exe) sometimes return null base_address even when peb is
  // valid; requiring base_address here loops the bootstrap forever.
  if (status < 0 || info.peb == nullptr)
    return false;

  m_pid = process_id;
  m_proc_data = info;
  return true;
}

bool c_memory::attach(std::wstring process_name) {
  return attach(get_process_id(process_name));
}

void c_memory::detach() {
  m_pid = 0;
  m_proc_data = {};
  // Reset cached transport so the next try_connect re-probes both NT
  // entries from scratch. Without this, a previous attempt that
  // fell through to the alternate transport (NtConvertBetween...)
  // leaves m_transport_idx = 1, and the next probe goes through that
  // entry — which the kernel driver doesn't hook. The Init-fallback
  // path was hitting this: second probe at default cmd values went
  // through the wrong NT call and returned failure even though the
  // driver was alive on the primary entry.
  m_transport_fn  = nullptr;
  m_transport_idx = 0;
  m_connected     = false;
}

bool c_memory::read(uint64_t src, void *dest, uint64_t size) {
  if (!src || !dest || !size || !m_pid)
    return false;

  // why: skip non-canonical addresses (neither valid usermode < 0x00007FFF'FFFFFFFF nor kernel > 0xFFFF8000'00000000); scan loops generate garbage pointers
  if (src > 0x00007FFFFFFFFFFFULL && src < 0xFFFF800000000000ULL)
    return false;

  mem_copy_request input = {};
  input.process_id = m_pid;
  input.source = src;
  input.dest = dest;
  input.size = size;

  auto request = c_request((e_command)astrum_cmd::read, &input, sizeof(input));

  // why: retry transient failures; kernel occasionally drops reply when another usermode thread (anti-cheat probe, GDI) collides
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (dispatch(&request) >= 0)
      return true;
  }
  return false;
}

bool c_memory::write(void *src, uint64_t dest, uint64_t size) {
  if (!src || !dest || !size || !m_pid)
    return false;

  // skip non-canonical so a garbage ptr never lands at the driver
  if (dest > 0x00007FFFFFFFFFFFULL && dest < 0xFFFF800000000000ULL)
    return false;

  mem_copy_request input = {};
  input.process_id = m_pid;
  input.source = reinterpret_cast<uint64_t>(src);
  input.dest = reinterpret_cast<void *>(dest);
  input.size = size;

  auto request = c_request((e_command)astrum_cmd::write, &input, sizeof(input));

  // why: same retry policy as read; writes run every tick
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (dispatch(&request) >= 0)
      return true;
  }
  return false;
}

uint64_t c_memory::get_module_base(std::wstring module_name) {
  if (!m_pid || !m_proc_data.peb)
    return 0;

  PEB peb = {};
  if (!read(reinterpret_cast<uint64_t>(m_proc_data.peb), &peb, sizeof(peb)))
    return 0;

  if (!peb.Ldr)
    return 0;

  PEB_LDR_DATA ldr = {};
  if (!read(reinterpret_cast<uint64_t>(peb.Ldr), &ldr, sizeof(ldr)))
    return 0;

  const uint64_t list_head = reinterpret_cast<uint64_t>(peb.Ldr) +
                             offsetof(PEB_LDR_DATA, InMemoryOrderModuleList);
  uint64_t list_entry =
      reinterpret_cast<uint64_t>(ldr.InMemoryOrderModuleList.Flink);

  int max_iter = 512;
  while (list_entry && list_entry != list_head && --max_iter > 0) {
    const uint64_t entry_va =
        list_entry - offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);

    LDR_DATA_TABLE_ENTRY entry = {};
    if (!read(entry_va, &entry, sizeof(entry))) {
      list_entry = 0;
      break;
    }

    UNICODE_STRING name_str = {};
    if (!read(entry_va + offsetof(LDR_DATA_TABLE_ENTRY, FullDllName), &name_str,
              sizeof(name_str))) {
      list_entry = read<uint64_t>(list_entry);
      continue;
    }

    if (name_str.Length > 0 && name_str.Length < 520 && name_str.Buffer) {
      std::wstring dll_name;
      dll_name.resize(name_str.Length / sizeof(wchar_t));

      if (read(reinterpret_cast<uint64_t>(name_str.Buffer), dll_name.data(),
               name_str.Length)) {
        auto it =
            std::search(dll_name.begin(), dll_name.end(), module_name.begin(),
                        module_name.end(), [](wchar_t a, wchar_t b) {
                          return towlower(a) == towlower(b);
                        });

        if (it != dll_name.end())
          return reinterpret_cast<uint64_t>(entry.DllBase);
      }
    }

    list_entry = read<uint64_t>(list_entry);
  }

  // EAC unlinks RustClient.exe from PEB→Ldr but can't hide
  // EPROCESS->SectionBaseAddress (kernel-returned in m_proc_data.base_address).
  // ONLY fall back for the main EXE — DLL queries (GameAssembly.dll etc.) that
  // miss the walk must return 0 so callers (game.cpp's get_module_hidden chain,
  // memory::get_module fallbacks) can try other resolution paths. If we
  // returned the EXE base for a DLL miss, every downstream offset read
  // (game_assembly + typeinfo etc.) would land at wrong memory and the cheat
  // would silently see "no entities / no players / garbage everywhere."
  if (m_proc_data.base_address && module_name.size() >= 4) {
    const wchar_t* tail = module_name.c_str() + module_name.size() - 4;
    const bool is_exe =
        tail[0] == L'.' &&
        (tail[1] == L'e' || tail[1] == L'E') &&
        (tail[2] == L'x' || tail[2] == L'X') &&
        (tail[3] == L'e' || tail[3] == L'E');
    if (is_exe)
      return reinterpret_cast<uint64_t>(m_proc_data.base_address);
  }

  return 0;
}

// Driver allocates RWX in the target process and writes the chosen base
// address back into the request's `address` field. `protect` is accepted
// for source compat but ignored — the kernel always RWX. Same 3-attempt
// retry as read/write since this travels through the same HAL hook and
// can collide with concurrent dispatches.
uint64_t c_memory::alloc(uint64_t size, uint32_t /*protect*/) {
  if (!size || !m_pid)
    return 0;

  mem_alloc_request input = {};
  input.process_id = m_pid;
  input.size = size;
  input.address = nullptr;  // OUT: driver writes the chosen base here

  auto request = c_request((e_command)astrum_cmd::alloc, &input, sizeof(input));

  for (int attempt = 0; attempt < 3; ++attempt) {
    if (dispatch(&request) >= 0)
      return reinterpret_cast<uint64_t>(input.address);
  }
  return 0;
}

bool c_memory::free(uint64_t) { return false; }
bool c_memory::protect(uint64_t, uint64_t, uint32_t) { return false; }
bool c_memory::hide_process(uint32_t) { return false; }
bool c_memory::unhide_process(uint32_t) { return false; }
bool c_memory::is_hidden(uint32_t) { return false; }

bool c_memory::scatter_write(SCATTER_WRITE_ENTRY *entries, size_t count) {
  if (!entries || !count || !m_pid)
    return false;

  bool any_ok = false;
  for (size_t i = 0; i < count; ++i) {
    if (entries[i].address && entries[i].value) {
      if (write(&entries[i].value, entries[i].address, sizeof(uint32_t)))
        any_ok = true;
    }
  }
  return any_ok;
}

bool c_memory::scatter_read(SCATTER_READ_ENTRY *entries, size_t count) {
  if (!entries || !count || !m_pid)
    return false;

  bool any_ok = false;
  for (size_t i = 0; i < count; ++i) {
    entries[i].status = 0;
    if (!entries[i].address || !entries[i].size || entries[i].size > 16)
      continue;

    memset(entries[i].data, 0, 16);
    if (read(entries[i].address, entries[i].data, entries[i].size)) {
      entries[i].status = 1;
      any_ok = true;
    }
  }
  return any_ok;
}

uint32_t c_memory::get_process_id(std::wstring process_name) const {
  auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return 0;

  PROCESSENTRY32W entry = {sizeof(entry)};
  if (!Process32FirstW(snapshot, &entry)) {
    CloseHandle(snapshot);
    return 0;
  }

  do {
    if (std::wstring(entry.szExeFile) == process_name) {
      CloseHandle(snapshot);
      return entry.th32ProcessID;
    }
  } while (Process32NextW(snapshot, &entry));

  CloseHandle(snapshot);
  return 0;
}

// Batched read. Sends one driver call containing N read requests.
// Userland-side caps mirror the kernel's so we never even round-trip
// a request the driver would reject. Per-entry failures are captured
// in each entry's status field — caller iterates and checks.
bool c_memory::read_batch(mem_read_batch_entry *entries, uint32_t count) {
  if (!entries || count == 0 || count > kMaxBatchEntries || !m_pid)
    return false;

  // Stamp the process_id into every entry so callers don't have to.
  // Also pre-validate per-entry caps locally — saves a kernel round
  // trip when the batch is malformed.
  uint64_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (entries[i].size > (16ull * 1024 * 1024)) {
      return false;
    }
    total += entries[i].size;
    if (total > kMaxBatchTotalBytes) {
      return false;
    }
    entries[i].process_id = m_pid;
    entries[i].status     = 0;
    entries[i]._pad       = 0;
  }

  mem_read_batch_request req{};
  req.count   = count;
  req._pad    = 0;
  req.entries = entries;

  auto request = c_request((e_command)astrum_cmd::batch(), &req, sizeof(req));

  // Same retry pattern as single read — the kernel call can drop on
  // collision with another thread's HAL traffic.
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (dispatch(&request) >= 0)
      return true;
  }
  return false;
}

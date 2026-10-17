#pragma once
/*
 * driver_comm.h
 *
 * DriverComm — driver communication interface.
 *
 * Wraps c_memory (NtQueryAuxiliaryCounterFrequency / HAL dispatch hook).
 * The Loader is responsible for mapping the kernel driver before launching
 * this process. Init() verifies the driver is responding.
 *
 * All call sites (g_Driver.ReadMemory / WriteMemory / etc.) are unchanged.
 */

#include "crash_log.h"
#include "driver_memory.hpp"
#include <cmath>
#include <cstdio>

class DriverComm {
  c_memory m_mem;

  // Ensure PID is set before each operation (attach only sets m_pid; the
  // hook is already resolved from Init(), so it's a cheap assignment).
  void set_pid(DWORD pid) {
    if (m_mem.get_pid() != static_cast<uint32_t>(pid))
      m_mem.attach(static_cast<uint32_t>(pid));
  }

public:
  /* ── Connection management  */

  bool Init() {
    // Cmd values are static defaults — kernel and userland always
    // agree on which switch case is which. Token is per-HWID, stable
    // across sessions. Either probe succeeds or the driver isn't
    // mapped at all; no fallback chase needed.
    if (m_mem.try_connect()) {
      CLOG("DriverComm::Init: connected");
      return true;
    }
    CLOG("DriverComm::Init: driver not responding — was it mapped by "
         "the Loader?");
    return false;
  }

  bool IsConnected() const { return m_mem.is_driver_connected(); }

  bool Cleanup() {
    m_mem.detach();
    CLOG("DriverComm::Cleanup: disconnected");
    return true;
  }

  void Reconnect() {
    CLOG("DriverComm::Reconnect: attempting...");
    m_mem.detach();
    if (m_mem.try_connect()) {
      CLOG("DriverComm::Reconnect: connected");
    } else {
      CLOG("DriverComm::Reconnect: failed — driver not responding");
    }
  }

  bool CheckDriverStatus() {
    bool ok = m_mem.check_alive();
    if (!ok)
      CLOG("DriverComm::CheckDriverStatus: ping failed");
    return ok;
  }

  /* ── Memory R/W  */

  bool ReadMemory(DWORD pid, uintptr_t address, void *buffer, size_t size) {
    if (!IsConnected() || !buffer || !size)
      return false;
    set_pid(pid);
    return m_mem.read(static_cast<uint64_t>(address), buffer,
                      static_cast<uint64_t>(size));
  }

  bool WriteMemory(DWORD pid, uintptr_t address, void *buffer, size_t size) {
    if (!IsConnected() || !buffer || !size)
      return false;
    set_pid(pid);
    return m_mem.write(buffer, static_cast<uint64_t>(address),
                       static_cast<uint64_t>(size));
  }

  /* ── PEB of attached process  */

  uintptr_t GetPeb(DWORD pid) {
    if (!IsConnected())
      return 0;
    set_pid(pid);
    return reinterpret_cast<uintptr_t>(m_mem.get_peb());
  }

  /* ── Module base  */

  uintptr_t GetModuleBase(DWORD pid, const wchar_t *moduleName) {
    if (!IsConnected() || !moduleName)
      return 0;
    set_pid(pid);
    uint64_t base = m_mem.get_module_base(moduleName);
    CLOG("GetModuleBase: pid=%u mod=%S -> 0x%llx", pid, moduleName,
         (unsigned long long)base);
    return static_cast<uintptr_t>(base);
  }

  /* ── Alloc / Free / Protect  */

  uintptr_t AllocMemory(DWORD pid, size_t size, DWORD protect_) {
    if (!IsConnected() || !size)
      return 0;
    set_pid(pid);
    return static_cast<uintptr_t>(
        m_mem.alloc(static_cast<uint64_t>(size), protect_));
  }

  void FreeMemory(DWORD pid, uintptr_t address) {
    if (!IsConnected() || !address)
      return;
    set_pid(pid);
    m_mem.free(static_cast<uint64_t>(address));
  }

  void ProtectMemory(DWORD pid, uintptr_t address, size_t size,
                     DWORD protect_) {
    if (!IsConnected() || !address || !size)
      return;
    set_pid(pid);
    m_mem.protect(static_cast<uint64_t>(address), static_cast<uint64_t>(size),
                  protect_);
  }

  /* ── Scatter I/O  */

  bool WriteScatter(DWORD pid, SCATTER_WRITE_ENTRY *entries, size_t count) {
    if (!IsConnected() || !entries || !count)
      return false;
    set_pid(pid);
    return m_mem.scatter_write(entries, count);
  }

  bool ReadScatter(DWORD pid, SCATTER_READ_ENTRY *entries, size_t count) {
    if (!IsConnected() || !entries || !count)
      return false;
    set_pid(pid);
    return m_mem.scatter_read(entries, count);
  }

  /* ── Process hiding  */

  bool HideProcess(DWORD pid) {
    if (!IsConnected())
      return false;
    return m_mem.hide_process(static_cast<uint32_t>(pid));
  }

  bool UnhideProcess(DWORD pid) {
    if (!IsConnected())
      return false;
    return m_mem.unhide_process(static_cast<uint32_t>(pid));
  }

  bool IsProcessHidden(DWORD pid) {
    if (!IsConnected())
      return false;
    return m_mem.is_hidden(static_cast<uint32_t>(pid));
  }

  /* ── Write sanity check  */

  bool TestWriteCapability(DWORD pid, uintptr_t testAddr) {
    if (!IsConnected() || !testAddr)
      return false;
    float original = 0;
    if (!ReadMemory(pid, testAddr, &original, sizeof(float))) {
      printf("[WRITE-TEST] FAIL: cannot read 0x%llx\n",
             (unsigned long long)testAddr);
      return false;
    }
    float testVal = (original == 0.0f) ? 0.001f : original + 0.001f;
    if (!WriteMemory(pid, testAddr, &testVal, sizeof(float))) {
      printf("[WRITE-TEST] FAIL: WriteMemory returned false\n");
      return false;
    }
    float readback = 0;
    ReadMemory(pid, testAddr, &readback, sizeof(float));
    WriteMemory(pid, testAddr, &original, sizeof(float));
    if (fabsf(readback - testVal) < 0.0001f) {
      printf("[WRITE-TEST] OK (addr=0x%llx)\n", (unsigned long long)testAddr);
      return true;
    }
    printf("[WRITE-TEST] FAIL: write had no effect (readback=%.4f)\n",
           readback);
    return false;
  }

  /* ── Templated helpers  */

  template <typename T> T Read(DWORD pid, uintptr_t address) {
    T value{};
    ReadMemory(pid, address, &value, sizeof(T));
    return value;
  }

  template <typename T>
  bool Write(DWORD pid, uintptr_t address, const T &value) {
    return WriteMemory(pid, address, (void *)&value, sizeof(T));
  }
};

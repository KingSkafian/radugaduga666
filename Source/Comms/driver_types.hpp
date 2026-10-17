#pragma once
/*
 * driver_types.hpp
 *
 * Transport types for kernel driver communication via
 * NtQueryAuxiliaryCounterFrequency (HAL dispatch hook).
 *
 * Per-customer builds: the build pipeline drops an astrum_build_config.hpp
 * next to this file (or onto the include path) with the rotated command
 * IDs and the HMAC-derived auth token. __has_include picks it up
 * automatically; the defaults below match the kernel's hardcoded fallbacks
 * so a build without ever picking the file up still talks to a kernel
 * that did.
 */

#include <cstdint>

#if defined(__has_include)
#  if __has_include("astrum_build_config.hpp")
#    include "astrum_build_config.hpp"
#  endif
#endif

/* ── Command IDs  */

#ifndef ASTRUM_CMD_NULL
#define ASTRUM_CMD_NULL  0u
#endif
#ifndef ASTRUM_CMD_READ
#define ASTRUM_CMD_READ  1u
#endif
#ifndef ASTRUM_CMD_WRITE
#define ASTRUM_CMD_WRITE 2u
#endif
#ifndef ASTRUM_CMD_QUERY
#define ASTRUM_CMD_QUERY 3u
#endif
#ifndef ASTRUM_CMD_ALLOC
#define ASTRUM_CMD_ALLOC 4u
#endif

/* Derived batched-read id — XOR'd from READ so kernel + userland agree
   without another /D flag. Driver-side static_asserts guard against
   collisions; mirror them here so a bad override fails build. */
#define ASTRUM_CMD_READ_BATCH ((ASTRUM_CMD_READ) ^ 0xC0DEBA5Eu)
static_assert(ASTRUM_CMD_READ_BATCH != ASTRUM_CMD_NULL,  "batch cmd collides with NULL — re-roll cmd values");
static_assert(ASTRUM_CMD_READ_BATCH != ASTRUM_CMD_READ,  "batch cmd collides with READ — re-roll cmd values");
static_assert(ASTRUM_CMD_READ_BATCH != ASTRUM_CMD_WRITE, "batch cmd collides with WRITE — re-roll cmd values");
static_assert(ASTRUM_CMD_READ_BATCH != ASTRUM_CMD_QUERY, "batch cmd collides with QUERY — re-roll cmd values");
static_assert(ASTRUM_CMD_READ_BATCH != ASTRUM_CMD_ALLOC, "batch cmd collides with ALLOC — re-roll cmd values");

/* Per-HWID auth token. driver_builder derives it via HMAC over the
   customer's HWID and bakes it in with /D ASTRUM_AUTH_TOKEN=. Userland
   sends the same token in every c_request; the driver compares with
   constant time and returns STATUS_ACCESS_DENIED on mismatch. 0 is
   "no auth" — driver's if-constexpr elides the check entirely. */
#ifndef ASTRUM_AUTH_TOKEN
#define ASTRUM_AUTH_TOKEN 0u
#endif

// Static command identifiers — same for every customer's build, same
// as the kernel source's defaults. Userland and kernel always agree
// on which switch case is which without anything being transported.
// The auth token (below) is the per-HWID secret that gates dispatch.
// main.cpp's --cmd-token CLI path calls set_token() at boot to override
// the build-config bake; we keep that runtime path so a single binary
// can talk to a kernel built with a different token than this one was.
namespace astrum_cmd {
  inline uint32_t null  = ASTRUM_CMD_NULL;
  inline uint32_t read  = ASTRUM_CMD_READ;
  inline uint32_t write = ASTRUM_CMD_WRITE;
  inline uint32_t query = ASTRUM_CMD_QUERY;
  inline uint32_t alloc = ASTRUM_CMD_ALLOC;
  // Per-HWID auth token. Initialized from the build-config bake;
  // overridden at boot from the loader's --cmd-token arg (forwarded
  // out of the PWJBCMS1 trailer) via set_token().
  inline uint32_t token = ASTRUM_AUTH_TOKEN;

  inline void set_token(uint32_t t) { token = t; }
}

enum class e_command : uint32_t {
  null = ASTRUM_CMD_NULL,
  mem_read = ASTRUM_CMD_READ,
  mem_write = ASTRUM_CMD_WRITE,
  proc_query = ASTRUM_CMD_QUERY,
  mem_alloc = ASTRUM_CMD_ALLOC,
};

// MUST match kAstrumRequestMagic in the kernel driver. Kernel-side HAL
// hijack uses this as the in-register discriminator (RDX) — any caller
// of NtQueryAuxiliaryCounterFrequency without the magic in RDX falls
// through to the original handler, no user memory dereferenced.
constexpr uint64_t kAstrumRequestMagic = 0xA5724DC0DEA11Eull;

/* ── Request wrapper passed to the HAL dispatch hook  */

class c_request {
public:
  // Default: token pulled from the runtime astrum_cmd::token global, which
  // is initialized from the build-config bake and overridable via the
  // --cmd-token CLI path. Existing call sites keep this form.
  c_request(e_command command, void *data, uint64_t length)
      : m_magic(kAstrumRequestMagic), m_data(data), m_length(length),
        m_command(command), m_token(astrum_cmd::token) {}

  // Explicit-token overload: used by c_memory::set_auth_token() so a
  // single binary can talk to multiple per-customer kernels in one run
  // without stomping the global. Mirrors comms/driver_types.hpp's API.
  c_request(e_command command, void *data, uint64_t length, uint32_t token)
      : m_magic(kAstrumRequestMagic), m_data(data), m_length(length),
        m_command(command), m_token(token) {}

  ~c_request() = default;

  template <typename T> T *get() const {
    if (!m_data || !m_length)
      return nullptr;
    if (sizeof(T) != m_length)
      return nullptr;
    return (T *)m_data;
  }

  e_command get_command() const { return m_command; }
  uint32_t  get_token()   const { return m_token; }
  uint64_t  get_magic()   const { return m_magic; }

private:
  uint64_t m_magic = 0;   // MUST be first — mirrors kernel layout
  void *m_data = nullptr;
  uint64_t m_length = 0;
  e_command m_command = e_command::null;
  uint32_t m_token = 0;
};

/* ── Memory copy request (read / write)  */

typedef struct _mem_copy_request {
  uint32_t process_id = 0;
  uint64_t source = 0;
  void *dest = nullptr;
  uint64_t size = 0;
} mem_copy_request, *pmem_copy_request;

/* ── Process info request (proc_query)  */

typedef struct _proc_info_request {
  uint32_t process_id = 0;
  void *peb = nullptr;
  void *base_address = nullptr;
  uint64_t cr3 = 0;
} proc_info_request, *pproc_info_request;

/* ── Memory alloc request (mem_alloc) ───────────────────────────── */
/* Driver allocates RWX in the target process and writes the chosen
   base address back into `address`. Used by the injector for DLL +
   shellcode. Mirrors comms/driver_types.hpp byte-for-byte. */

typedef struct _mem_alloc_request {
  uint32_t process_id = 0;
  uint64_t size = 0;
  void    *address = nullptr;   // OUT: chosen base address
} mem_alloc_request, *pmem_alloc_request;

// Batched read entry. Kernel loops over an array of these and writes
// each entry's `status` field independently — one bad entry doesn't
// fail the whole batch.
typedef struct _mem_read_batch_entry {
  uint32_t process_id;
  uint64_t source;
  void    *dest;
  uint64_t size;
  int32_t  status;          // OUT: 0 on success, NTSTATUS on failure
  uint32_t _pad;
} mem_read_batch_entry, *pmem_read_batch_entry;

typedef struct _mem_read_batch_request {
  uint32_t count;
  uint32_t _pad;
  mem_read_batch_entry *entries;
} mem_read_batch_request, *pmem_read_batch_request;

// Userland-side mirrors of the kernel caps (Loader / pawjob check
// these so we never even send a request the driver would reject).
constexpr uint32_t kMaxBatchEntries    = 256u;
constexpr uint64_t kMaxBatchTotalBytes = 64ull * 1024 * 1024;

// Same derivation the kernel uses (ASTRUM_CMD_READ_BATCH). Use the
// macro so a per-customer build-config rotation lands in one place.
namespace astrum_cmd {
  inline uint32_t batch() { return ASTRUM_CMD_READ_BATCH; }
}

/* ── Legacy scatter types (emulated via individual reads/writes) ─── */
/*
 * Duplicate definitions live in shared.h (DBD's legacy transport header).
 * A shared sentinel guards both so whichever header loads first wins.
 * The byte layout matches exactly in both places.
 */
#ifndef SOLARAX_SCATTER_ENTRIES_DEFINED
#define SOLARAX_SCATTER_ENTRIES_DEFINED

typedef struct _SCATTER_WRITE_ENTRY {
  uint64_t address;
  unsigned int value;
  unsigned int _pad;
} SCATTER_WRITE_ENTRY, *PSCATTER_WRITE_ENTRY;

typedef struct _SCATTER_READ_ENTRY {
  uint64_t address;
  unsigned int size;
  unsigned int status;
  unsigned char data[16];
} SCATTER_READ_ENTRY, *PSCATTER_READ_ENTRY;

#endif /* SOLARAX_SCATTER_ENTRIES_DEFINED */

#pragma once

// write_buffer helper - uses the backend driver's write_memory
inline bool write_buffer(uint64_t address, const void* buffer, size_t size)
{
    if (!buffer || size == 0) return true;
    return g_backend->write_memory(address, const_cast<void*>(buffer), size);
}
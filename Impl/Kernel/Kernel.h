#pragma once

// write_buffer helper - uses the New Driver's write_memory
inline bool write_buffer(uint64_t address, const void* buffer, size_t size)
{
    if (!buffer || size == 0) return true;
    return g_driver->write_memory(address, const_cast<void*>(buffer), size);
}
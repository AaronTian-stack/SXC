#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SXC
{
// .slang_blob container layout:
// [ShaderBlobHeader]
// [Entry0][Define0_0\0][Define0_1\0]...[EntryN][DefineN_*\0]
// [Shader0Bytecode][Shader1Bytecode]...[ShaderNBytecode]

struct ShaderView
{
    void* data;
    size_t size;
};

struct ShaderBlobHeader
{
    uint32_t magic;
    uint32_t version;
    uint64_t shader_count;
};

struct ShaderBlobEntry
{
    uint64_t offset;
    uint64_t size;
    uint32_t define_count;
};

constexpr uint32_t SHADER_BLOB_MAGIC = 0x53584342; // 'SXCB'
constexpr uint32_t SHADER_BLOB_VERSION = 3;

class ShaderBlob
{
public:
    /**
     * Finds native shader bytecode matching the requested defines.
     * Returns the first shader when no defines are provided.
     * @param blob Loaded .slang_blob container.
     * @param out_shader Receives a non-owning view of the selected shader bytecode.
     * @param defines Array of null terminated define strings to match.
     * @param define_count Number of strings in the defines array.
     * @return Whether matching shader bytecode was found successfully.
     */
    static bool find_shader(const ShaderView& blob,
                            ShaderView* const out_shader,
                            const char* const* defines = nullptr,
                            const uint32_t define_count = 0)
    {
        if (!out_shader || !blob.data || blob.size < sizeof(ShaderBlobHeader) || (!defines && define_count != 0))
        {
            return false;
        }

        for (uint32_t i = 0; i < define_count; i++)
        {
            if (!defines[i])
            {
                return false;
            }
        }

        auto* const base = static_cast<std::byte*>(blob.data);
        const auto* const end = base + blob.size;
        ShaderBlobHeader header{};
        std::memcpy(&header, base, sizeof(header));
        if (header.magic != SHADER_BLOB_MAGIC || header.version != SHADER_BLOB_VERSION)
        {
            return false;
        }

        const std::byte* cursor = base + sizeof(ShaderBlobHeader);
        for (uint64_t i = 0; i < header.shader_count; i++)
        {
            ShaderBlobEntry entry{};
            const char* define_list = nullptr;
            if (!read_entry(&cursor, end, &entry, &define_list))
            {
                return false;
            }

            bool match = true;
            for (uint32_t requested = 0; requested < define_count; requested++)
            {
                const auto requested_size = std::strlen(defines[requested]);
                bool found = false;
                const char* define_cursor = define_list;
                for (uint32_t d = 0; d < entry.define_count; d++)
                {
                    const auto size = std::strlen(define_cursor);
                    if (size == requested_size && std::memcmp(define_cursor, defines[requested], size) == 0)
                    {
                        found = true;
                        break;
                    }
                    define_cursor += size + 1;
                }
                if (!found)
                {
                    match = false;
                    break;
                }
            }

            if (match)
            {
                return set_shader_view(blob, entry, out_shader);
            }
        }

        return false;
    }

private:
    static bool read_entry(const std::byte** cursor,
                           const std::byte* const end,
                           ShaderBlobEntry* const out_entry,
                           const char** const out_defines = nullptr)
    {
        auto& cursor_ref = *cursor;
        if (cursor_ref > end || static_cast<size_t>(end - cursor_ref) < sizeof(ShaderBlobEntry))
        {
            return false;
        }

        std::memcpy(out_entry, cursor_ref, sizeof(ShaderBlobEntry));
        cursor_ref += sizeof(*out_entry);
        if (out_defines)
        {
            *out_defines = reinterpret_cast<const char*>(cursor_ref);
        }

        for (uint32_t i = 0; i < out_entry->define_count; i++)
        {
            const auto remaining = static_cast<size_t>(end - cursor_ref);
            const auto* const terminator = static_cast<const std::byte*>(std::memchr(cursor_ref, '\0', remaining));
            if (!terminator)
            {
                return false;
            }
            cursor_ref = terminator + 1;
        }
        return true;
    }

    static bool set_shader_view(const ShaderView& blob, const ShaderBlobEntry& entry, ShaderView* const out_shader)
    {
        const auto shader_end = entry.offset + entry.size;
        if (entry.offset > blob.size || shader_end < entry.offset || shader_end > blob.size)
        {
            return false;
        }

        *out_shader = {
            .data = static_cast<std::byte*>(blob.data) + static_cast<size_t>(entry.offset),
            .size = static_cast<size_t>(entry.size),
        };
        return true;
    }
};
} // namespace SXC

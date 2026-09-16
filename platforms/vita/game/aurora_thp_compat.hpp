#pragma once

/* The portable Aurora THP decoder only needs the byte-reading portion of
 * Aurora's internal support header.  Keep the native Vita build independent
 * of Aurora's desktop logging/fmt dependency. */
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

template <typename T>
  requires(std::is_integral_v<T>)
inline T read_bits(const void* source) noexcept
{
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    using U = std::make_unsigned_t<T>;
    U value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value = static_cast<U>((value << 8) | bytes[i]);
    return static_cast<T>(value);
}

namespace aurora {
class ByteReader {
public:
    static ByteReader unbounded(const void* data) noexcept
    {
        return ByteReader(static_cast<const std::uint8_t*>(data));
    }

    template <typename T>
      requires(std::is_integral_v<T>)
    bool try_read(T& value) noexcept
    {
        value = read_bits<T>(mData + mOffset);
        mOffset += sizeof(T);
        return true;
    }

    bool try_take(std::size_t count,
                  std::span<const std::uint8_t>& output) noexcept
    {
        output = std::span<const std::uint8_t>(mData + mOffset, count);
        mOffset += count;
        return true;
    }

    std::size_t offset() const noexcept { return mOffset; }

private:
    explicit ByteReader(const std::uint8_t* data) noexcept : mData(data) {}

    const std::uint8_t* mData;
    std::size_t mOffset = 0;
};
} // namespace aurora

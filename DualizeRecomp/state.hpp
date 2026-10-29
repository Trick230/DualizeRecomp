#pragma once

// Savestate stream: raw little-endian PODs in declaration order, one buffer for
// the whole machine. A section marker after each component catches layout
// drift at load instead of letting it corrupt what follows.

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace dualize
{
class StateIO
{
public:
  // Writing appends to buffer; reading consumes it from the start.
  StateIO(std::vector<uint8_t>& buffer, bool reading) : m_buffer(buffer), m_reading(reading) {}

  bool Reading() const { return m_reading; }
  // False once a read ran past the end or hit a bad marker.
  bool Ok() const { return m_ok; }

  void Bytes(void* data, size_t size)
  {
    if (m_reading)
    {
      if (!m_ok || size > m_buffer.size() - m_cursor)
      {
        m_ok = false;
        std::memset(data, 0, size);
        return;
      }
      std::memcpy(data, m_buffer.data() + m_cursor, size);
      m_cursor += size;
    }
    else
    {
      const uint8_t* bytes = static_cast<const uint8_t*>(data);
      m_buffer.insert(m_buffer.end(), bytes, bytes + size);
    }
  }
  template <typename T>
  void Do(T& value)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    Bytes(&value, sizeof(T));
  }
  void Do(bool& value)
  {
    uint8_t v = value;
    Do(v);
    value = v != 0;
  }
  void Do(std::string& value)
  {
    uint32_t size = static_cast<uint32_t>(value.size());
    Do(size);
    if (m_reading)
      value.resize(m_ok ? size : 0);
    if (!value.empty())
      Bytes(value.data(), value.size());
  }
  template <typename T>
  void Do(std::vector<T>& values)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    uint32_t size = static_cast<uint32_t>(values.size());
    Do(size);
    if (m_reading)
      values.resize(m_ok && size <= m_buffer.size() - m_cursor ? size : 0);
    if (!values.empty())
      Bytes(values.data(), values.size() * sizeof(T));
  }
  void Marker(uint32_t id)
  {
    uint32_t value = id;
    Do(value);
    if (m_reading && value != id)
      m_ok = false;
  }

private:
  std::vector<uint8_t>& m_buffer;
  size_t m_cursor = 0;
  bool m_reading;
  bool m_ok = true;
};
}  // namespace dualize

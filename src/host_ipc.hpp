#pragma once

#include "winusb_device.hpp"

#include <array>

namespace u3s::host {

constexpr size_t ts_chunk_size = 256 * 188;
constexpr size_t ts_queue_capacity = 256;
using Ticket = std::array<uint8_t, 16>;

enum class Op : uint32_t {
    hello = 1, readers, open_tuner, close_tuner, tune, signal, purge, attach, read_ts,
    open_card, card_status, transmit, disconnect_card,
};

struct Frame {
    Frame() = default;
    explicit Frame(uint32_t code_, uint32_t a_ = 0, uint32_t b_ = 0,
                   std::span<const uint8_t> data_ = {})
        : code(code_), a(a_), b(b_), data(data_.begin(), data_.end()) {}
    explicit Frame(Op op, uint32_t a_ = 0, uint32_t b_ = 0, std::span<const uint8_t> data_ = {})
        : Frame(static_cast<uint32_t>(op), a_, b_, data_) {}

    uint32_t code = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    std::vector<uint8_t> data;
};

struct Caller {
    std::wstring sid;
    ULONG pid;
    bool operator==(const Caller&) const = default;
};

std::wstring current_sid();
std::wstring target_sid();
std::wstring pipe_name(const std::wstring& sid, bool ts);
WinHandle create_listener(const std::wstring& sid, bool ts, bool first);
Caller authenticate(HANDLE pipe, const std::wstring& host_sid);
void write_frame(HANDLE pipe, const Frame& frame, DWORD timeout = INFINITE);
Frame read_frame(HANDLE pipe, size_t limit, DWORD timeout = INFINITE);
void trace(const char* operation, const char* detail) noexcept;

class Connection {
public:
    explicit Connection(const std::wstring& sid, bool ts = false, DWORD timeout = 3000);
    Frame call(const Frame& request, size_t limit = 0, DWORD timeout = 0);
    bool alive() const noexcept;

private:
    WinHandle pipe_;
};

}

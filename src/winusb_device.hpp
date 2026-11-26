#pragma once

#include "u3s.hpp"

#include <windows.h>
#include <winusb.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace u3s {

[[noreturn]] void win_error(const char* operation, DWORD code = GetLastError());

struct CloseWinHandle {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using WinHandle = std::unique_ptr<void, CloseWinHandle>;

std::vector<std::wstring> list_winusb();

class WinUsb final : public Control {
public:
    explicit WinUsb(const std::wstring& path);
    void transfer(uint8_t type, uint8_t request, uint16_t index, std::span<uint8_t> data) override;
    void alt(uint8_t setting) override;

private:
    friend class IsoStream;
    WinHandle file_;
    std::unique_ptr<void, decltype(&WinUsb_Free)> interface_{nullptr, WinUsb_Free};
};

struct IsoBlock {
    std::vector<uint8_t> data;
    std::vector<size_t> gaps;
    bool missing_before = false;
};

struct IsoStats {
    uint64_t failed_packets = 0;
    uint64_t restarts = 0;
};

class IsoStream {
public:
    explicit IsoStream(WinUsb& usb);
    ~IsoStream();

    void read(std::chrono::seconds duration, const std::atomic_bool& stop,
              const std::function<void(IsoBlock)>& sink);
    const IsoStats& stats() const { return stats_; }

private:
    struct Slot;
    DWORD prime();
    void cancel_and_drain() noexcept;
    IsoBlock collect(const Slot& slot);

    WinUsb& usb_;
    std::vector<std::unique_ptr<Slot>> slots_;
    ULONG bytes_per_interval_ = 0;
    IsoStats stats_;
};

}

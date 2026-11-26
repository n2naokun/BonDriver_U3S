#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <span>
#include <vector>

namespace u3s {

inline constexpr size_t ts_packet_size = 188;

struct TsStats {
    uint64_t packets = 0;
    uint64_t resyncs = 0;
    uint64_t skipped_bytes = 0;
    uint64_t startup_dropped = 0;
    bool startup_limit = false;
};

class TsPipeline {
public:
    TsPipeline();
    ~TsPipeline();
    TsPipeline(const TsPipeline&) = delete;
    TsPipeline& operator=(const TsPipeline&) = delete;

    void feed(std::span<const uint8_t> data, std::vector<uint8_t>& output);
    void gap();
    const TsStats& stats() const { return stats_; }

private:
    void packet(std::span<uint8_t, ts_packet_size> packet, std::vector<uint8_t>& output);

    BCRYPT_KEY_HANDLE key_ = nullptr;
    BCRYPT_ALG_HANDLE aes_ = nullptr;
    std::vector<uint8_t> input_;
    std::vector<uint8_t> startup_;
    bool synced_ = false;
    bool ever_synced_ = false;
    bool settled_ = false;
    TsStats stats_;
};

}

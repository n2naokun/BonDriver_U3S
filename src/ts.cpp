// SPDX-License-Identifier: MIT
// Copyright (c) 2026 waya0125
// recfriio の mtvu3s/stream.go と settle.go を基準に実装。AES は Windows CNG を使用。
#include "ts.hpp"

#include <stdexcept>
#include <string>

namespace u3s {
namespace {
constexpr size_t clean_run = 5000;
constexpr size_t settle_limit = 200000;

void cng_check(NTSTATUS status, const char* operation) {
    if (status < 0) {
        throw std::runtime_error(std::string(operation) + " (NTSTATUS " + std::to_string(status) + ")");
    }
}
}

TsPipeline::TsPipeline() {
    cng_check(BCryptOpenAlgorithmProvider(&aes_, BCRYPT_AES_ALGORITHM, nullptr, 0), "AES プロバイダーの取得");
    try {
        wchar_t mode[] = BCRYPT_CHAIN_MODE_ECB;
        cng_check(BCryptSetProperty(aes_, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode), sizeof(mode), 0),
            "AES-ECB の設定");
        UCHAR zero_key[16]{};
        cng_check(BCryptGenerateSymmetricKey(aes_, &key_, nullptr, 0, zero_key, sizeof(zero_key), 0), "AES 鍵の生成");
    } catch (...) {
        BCryptCloseAlgorithmProvider(aes_, 0);
        throw;
    }
}

TsPipeline::~TsPipeline() {
    BCryptDestroyKey(key_);
    BCryptCloseAlgorithmProvider(aes_, 0);
}

void TsPipeline::gap() {
    synced_ = false;
    stats_.skipped_bytes += input_.size();
    input_.clear();
    stats_.startup_dropped += startup_.size() / ts_packet_size;
    startup_.clear();
}

void TsPipeline::packet(std::span<uint8_t, ts_packet_size> packet, std::vector<uint8_t>& output) {
    // EM28178 の暗号は 12 バイト目の次から。TS ヘッダーの放送側スクランブル状態を保持する。
    constexpr ULONG encrypted_length = ts_packet_size - 12;
    auto* tail = packet.data() + 12;
    ULONG decrypted = 0;
    cng_check(BCryptDecrypt(key_, tail, encrypted_length, nullptr, nullptr, 0, tail, encrypted_length, &decrypted, 0),
        "機器側 AES の解除");
    if (decrypted != encrypted_length) throw std::runtime_error("AES 解除後の長さが一致しません");

    if (settled_) {
        output.insert(output.end(), packet.begin(), packet.end());
        ++stats_.packets;
        return;
    }
    if (packet[1] & 0x80) {
        stats_.startup_dropped += startup_.size() / ts_packet_size + 1;
        startup_.clear();
    } else {
        startup_.insert(startup_.end(), packet.begin(), packet.end());
    }
    const auto clean = startup_.size() / ts_packet_size;
    stats_.startup_limit = stats_.startup_dropped + clean >= settle_limit && clean < clean_run;
    if (clean >= clean_run || stats_.startup_limit) {
        settled_ = true;
        output.insert(output.end(), startup_.begin(), startup_.end());
        stats_.packets += clean;
        startup_.clear();
    }
}

void TsPipeline::feed(std::span<const uint8_t> data, std::vector<uint8_t>& output) {
    input_.insert(input_.end(), data.begin(), data.end());
    size_t offset = 0;
    while (offset + ts_packet_size <= input_.size()) {
        if (!synced_) {
            // 次の受信で 3 個の同期バイトを照合できるように、末尾は最大 376 バイト残す。
            while (offset + 2 * ts_packet_size < input_.size()
                && (input_[offset] != 0x47 || input_[offset + ts_packet_size] != 0x47
                    || input_[offset + 2 * ts_packet_size] != 0x47)) {
                ++offset;
                ++stats_.skipped_bytes;
            }
            if (offset + 2 * ts_packet_size >= input_.size()) break;
            if (ever_synced_) ++stats_.resyncs;
            synced_ = ever_synced_ = true;
        }
        if (input_[offset] != 0x47) {
            synced_ = false;
            stats_.startup_dropped += startup_.size() / ts_packet_size;
            startup_.clear();
            continue;
        }
        packet(std::span<uint8_t, ts_packet_size>(input_.data() + offset, ts_packet_size), output);
        offset += ts_packet_size;
    }
    input_.erase(input_.begin(), input_.begin() + static_cast<ptrdiff_t>(offset));
}

}

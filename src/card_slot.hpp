#pragma once

#include "winusb_device.hpp"

#include <winscard.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <numeric>
#include <thread>

namespace u3s {

constexpr std::array<BYTE, 13> verified_atr{0x3b, 0xf0, 0x12, 0, 0xff, 0x91, 0x81, 0xb1, 0x7c, 0x45, 0x1f, 3, 0x99};
constexpr size_t aes_offset = 9;
constexpr ULONG aes_block_size = 16;
constexpr size_t aes_end = aes_offset + aes_block_size;
using Bytes = std::span<BYTE>;
using View = std::span<const BYTE>;
using namespace std::chrono_literals;

inline BYTE lrc(View data) {
    return std::accumulate(data.begin(), data.end(), BYTE{}, std::bit_xor<BYTE>{});
}

inline bool bcas_aes_reply(View apdu, View frame) {
    if (apdu.size() < 5 || apdu[0] != 0x90 || apdu[2] != 0 || apdu[3] != 0
        || frame.size() < 12 || frame[3] != 0 || frame[4] != frame.size() - 8) return false;

    // shortcut: 実機でAESを確認した応答形式に限定し、追加の命令は検証後に対応する。
    switch (apdu[1]) {
    case 0x30: return frame.size() == 65;
    case 0x32: return frame.size() >= 23 && (frame.size() - 13) % 10 == 0;
    case 0x34: return frame.size() == 29;
    case 0x3a: return apdu.size() == 10 && apdu[4] == 4 && frame.size() == 19;
    case 0x3c: return frame.size() == 24;
    case 0x50: return apdu.size() == 11 && apdu[4] == 5 && frame.size() == 12;
    case 0x80: return apdu.size() == 7 && apdu[4] == 1 && frame.size() == 24;
    default: return false;
    }
}

inline bool valid_bcas_reply(View apdu, View frame) {
    if (lrc(frame) != 0 || frame[frame.size() - 3] != 0x90 || frame[frame.size() - 2] != 0) return false;
    const unsigned code = (static_cast<unsigned>(frame[7]) << 8) | frame[8];
    switch (apdu[1]) {
    case 0x30: return code == 0x2100 && frame[9] == 0 && frame[10] == 5;
    case 0x32: return code == 0x2100 && frame[9] == (frame.size() - 13) / 10;
    case 0x3a: return (code == 0x2100 || code == 0xa101) && frame[14] == 0 && frame[15] == 0;
    case 0x50: return code == 0x2100;
    case 0x80: return (code == 0x2100 || code == 0xa101) && frame[9] == apdu[5];
    default: return true;
    }
}

class CardSlot {
public:
    explicit CardSlot(Control& usb) : usb_(usb) {}

    LONG reset() {
        ready_ = false;
        next_pcb_ = 0;
        ifsd_set_ = false;
        write(0x73, 0);
        std::this_thread::sleep_for(700ms);
        if ((read(0x70) & 5) != 5) return SCARD_E_NO_SMARTCARD;
        const auto length = read(0x71);
        if (!length) return SCARD_E_NO_SMARTCARD;
        if (length != atr.size()) return SCARD_W_UNSUPPORTED_CARD;
        usb_.transfer(0xc0, 0x14, 0, atr);
        // shortcut: 実機で読めたT=1/LRCのATRに限定。他のATRを扱うときにパラメーターを解析する。
        if (atr != verified_atr) return SCARD_W_UNSUPPORTED_CARD;
        ready_ = true;
        return SCARD_S_SUCCESS;
    }

    LONG command(View apdu, LPBYTE output, LPDWORD length) {
        if (!ready_) return SCARD_W_RESET_CARD;
        ready_ = false;
        std::array<BYTE, 258> response{};
        size_t size = 0;
        if (!ifsd_set_) {
            // 受信上限を254バイトに広げ、カードが受け入れたことを確認する。
            std::array<BYTE, 1> ifsd{0xfe};
            const auto result = exchange(0xc1, ifsd, 0xe1, response, size);
            if (result == SCARD_E_NOT_TRANSACTED) return SCARD_E_PROTO_MISMATCH;
            if (result != SCARD_S_SUCCESS) return result;
            if (size != 1 || response[3] != ifsd[0]) return SCARD_E_PROTO_MISMATCH;
            ifsd_set_ = true;
        }
        // shortcut: 確認したATRのIFSC(124)までの単一Iブロック。長い転送が必要ならチェーンを実装する。
        const auto result = exchange(next_pcb_, apdu, next_pcb_, response, size);
        if (result != SCARD_S_SUCCESS) return result;
        if (size < 2) return SCARD_E_NOT_TRANSACTED;
        next_pcb_ ^= 0x40;
        ready_ = true;
        const auto capacity = *length;
        *length = static_cast<DWORD>(size);
        if (capacity < size) return SCARD_E_INSUFFICIENT_BUFFER;
        std::copy_n(response.begin() + 3, size, output);
        return SCARD_S_SUCCESS;
    }

    bool ready() const { return ready_; }
    std::array<BYTE, verified_atr.size()> atr{};

private:
    BYTE read(uint16_t reg) {
        BYTE value{};
        usb_.transfer(0xc0, 0, reg, Bytes(&value, 1));
        return value;
    }

    void write(uint16_t reg, BYTE value) {
        usb_.transfer(0x40, 0, reg, Bytes(&value, 1));
    }

    void buffer(uint8_t type, uint16_t start, Bytes data) {
        for (size_t offset = 0; offset < data.size(); offset += 64) {
            usb_.transfer(type, 0x14, static_cast<uint16_t>(start + offset),
                data.subspan(offset, std::min<size_t>(64, data.size() - offset)));
        }
    }

    bool restore_bcas_reply(View apdu, Bytes response, size_t count) {
        // 短い応答ではLRCも暗号化ブロックに含まれ、ブロック末尾は受信長の外にある。
        if (count < aes_end) buffer(0xc0, static_cast<uint16_t>(count), response.subspan(count, aes_end - count));
        const auto block = response.subspan(aes_offset, aes_block_size);
        std::array<BYTE, aes_block_size> zero_key{};
        BCRYPT_KEY_HANDLE handle = nullptr;
        if (BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &handle, nullptr, 0,
                zero_key.data(), aes_block_size, 0) < 0) return false;
        const std::unique_ptr<void, decltype(&BCryptDestroyKey)> key(handle, BCryptDestroyKey);
        ULONG length = 0;
        const auto result = BCryptDecrypt(key.get(), block.data(), aes_block_size, nullptr, nullptr, 0,
            block.data(), aes_block_size, &length, 0);
        return result >= 0 && length == aes_block_size && valid_bcas_reply(apdu, response.first(count));
    }

    LONG exchange(BYTE pcb, View payload, BYTE expected_pcb, Bytes response, size_t& size) {
        std::array<BYTE, 128> tx{};
        tx[1] = pcb;
        tx[2] = static_cast<BYTE>(payload.size());
        std::copy(payload.begin(), payload.end(), tx.begin() + 3);
        const auto block_size = payload.size() + 4;
        tx[block_size - 1] = lrc(View(tx).first(block_size - 1));
        buffer(0x40, 0x0200, Bytes(tx).first(block_size));
        write(0x75, static_cast<BYTE>((read(0x75) & 0xfb) | 1));
        write(0x70, 0xff);
        write(0x71, static_cast<BYTE>(block_size));

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        // 純正ドライバーと同じ待ち時間を取り、受信長レジスタの早すぎる読み出しを避ける。
        std::this_thread::sleep_for(80ms);
        for (; std::chrono::steady_clock::now() < deadline; std::this_thread::sleep_for(5ms)) {
            if ((read(0x70) & 5) != 5) continue;
            const unsigned low = read(0x71);
            const unsigned count = low | (static_cast<unsigned>(read(0x72)) << 8);
            if (count < 4) continue;
            if (count > response.size()) return SCARD_E_NOT_TRANSACTED;
            buffer(0xc0, 0, response.first(count));
            size = response[2];
            // 完了ビットは受信途中でも立つので、ブロック全体が届くまで待つ。
            if (count < size + 4) continue;
            if (count != size + 4 || response[0] != 0 || response[1] != expected_pcb) return SCARD_E_NOT_TRANSACTED;
            const auto frame = response.first(count);
            if (bcas_aes_reply(payload, frame)) {
                // 暗号文のLRCが偶然一致しても、復号後の検証を通った応答だけを返す。
                return restore_bcas_reply(payload, response, count) ? SCARD_S_SUCCESS : SCARD_E_NOT_TRANSACTED;
            }
            return lrc(frame) == 0 ? SCARD_S_SUCCESS : SCARD_E_NOT_TRANSACTED;
        }
        return SCARD_E_TIMEOUT;
    }

    Control& usb_;
    BYTE next_pcb_ = 0;
    bool ifsd_set_ = false;
    bool ready_ = false;
};

}

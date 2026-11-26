// SPDX-License-Identifier: MIT
// Copyright (c) 2026 waya0125
// recfriio の mtvu3s、em28178、tc90522、fc0013、qm1d1c0042 から移植。
#include "u3s.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <stdexcept>
#include <thread>

namespace u3s {
namespace {
using namespace std::chrono_literals;
constexpr uint8_t terrestrial = 0x20;
constexpr uint8_t satellite = 0x22;
constexpr uint8_t fc0013 = 0xc6;
constexpr uint8_t qm1d1c0042 = 0xc0;
constexpr uint8_t request_out = 0x40;
constexpr uint8_t request_in = 0xc0;
constexpr uint8_t i2c_stop = 0x02;
constexpr uint8_t i2c_continue = 0x03;
constexpr uint16_t cs_tsids[]{0x6020, 0x7040, 0x7060, 0x6080, 0x60a0, 0x70c0,
    0x70e0, 0x7100, 0x7120, 0x7140, 0x7160, 0x7180};
constexpr uint8_t qm_defaults[]{0x68, 0x1c, 0xb0, 0x10, 0xbc, 0xc1, 0x11, 0x35,
    0x09, 0x06, 0x00, 0x00, 0xc3, 0x47, 0xb5, 0x30, 0x00, 0xff, 0xf3, 0x20, 0x3f, 0x25, 0x5c,
    0xd6, 0x55, 0xcf, 0x95, 0xf6, 0x36, 0xf2, 0x09, 0x00};

// この CLI のチャンネル名に含まれる各数値は最大 62。
int channel_number(std::wstring_view text) {
    if (text.empty()) return -1;
    int value = 0;
    for (wchar_t digit : text) {
        if (digit < L'0' || digit > L'9') return -1;
        value = value * 10 + (digit - L'0');
        if (value > 62) return -1;
    }
    return value;
}

bool valid_tsid(uint16_t id) { return id != 0 && id <= max_satellite_tsid; }

uint16_t match_tsid(uint16_t wanted, std::span<const uint16_t> ids) {
    if (std::find(ids.begin(), ids.end(), wanted) != ids.end()) return wanted;
    // BS の登録年ビットか、110度CS のネットワークビットだけを無視する。
    uint16_t mask = 0;
    if ((wanted & 0xf000) == 0x4000) mask = 0x0e00;
    if ((wanted & 0xe000) == 0x6000) mask = 0x1000;
    for (const auto id : ids) {
        if (valid_tsid(id) && (id & ~mask) == (wanted & ~mask)) return id;
    }
    return 0;
}

template<class Predicate>
bool wait_satellite(Predicate ready) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::this_thread::sleep_for(25ms);
        if (ready()) return true;
    }
    return false;
}

struct Pll { uint8_t am, pm; uint16_t k; };

Pll pll(unsigned vco, unsigned reference) {
    const auto divider = (vco + reference / 2) / reference;
    auto am = divider % 8;
    auto pm = divider / 8;
    if (am < 2) {
        am += 8;
        --pm;
    }
    auto k = ((vco % reference) << 15) / reference;
    if (k >= 0x4000) {
        k += 0x8000;
    }
    return {static_cast<uint8_t>(am), static_cast<uint8_t>(pm), static_cast<uint16_t>(k)};
}
}

Channel parse_channel(std::wstring_view text) {
    if (text.size() >= 2 && (text[1] == L'S' || text[1] == L's')) {
        const auto band = text.front();
        text.remove_prefix(2);
        if (band == L'B' || band == L'b') {
            const auto separator = text.find(L'_');
            const auto node = channel_number(text.substr(0, separator));
            if (separator == std::wstring_view::npos || text.size() - separator != 2
                || node < 1 || node > 23 || node % 2 == 0 || text.back() < L'0' || text.back() > L'7') {
                throw std::invalid_argument("BS は 1〜23 の奇数とスロット 0〜7 を指定してください（例 BS15_0）");
            }
            // BS15 のスロット番号は TSID 上で 1 つずれる。
            const auto slot = text.back() - L'0' + (node == 15);
            const auto frequency = satellite_frequency((node - 1) / 2);
            return {frequency, 0, static_cast<uint16_t>(0x4000 | (node << 4) | slot)};
        }
        if (band == L'C' || band == L'c') {
            const auto node = channel_number(text);
            if (node < 2 || node > 24 || node % 2 != 0) {
                throw std::invalid_argument("110度CS は 2〜24 の偶数を指定してください（例 CS2）");
            }
            const auto index = static_cast<unsigned>(node / 2 - 1);
            return {satellite_frequency(index + 12), 0, cs_tsids[index]};
        }
        throw std::invalid_argument("チャンネルは地デジ 13〜62、BS15_0、CS2 などを指定してください");
    }
    const auto channel = channel_number(text);
    if (channel < 13 || channel > 62) {
        throw std::invalid_argument("チャンネルは地デジ 13〜62、BS15_0、CS2 などを指定してください");
    }
    const auto number = static_cast<unsigned>(channel);
    return {terrestrial_frequency(number), number, 0};
}

void Device::bridge_write(uint8_t address, uint8_t value) {
    usb_.transfer(request_out, 0, address, {&value, 1});
}

void Device::demod_write(uint8_t chip, std::initializer_list<Reg> regs) {
    for (const auto [address, value] : regs) {
        uint8_t data[]{address, value};
        usb_.transfer(request_out, i2c_stop, chip, data);
    }
}

void Device::demod_read(uint8_t chip, uint8_t address, std::span<uint8_t> data) {
    usb_.transfer(request_out, i2c_continue, chip, {&address, 1});
    usb_.transfer(request_in, i2c_stop, chip, data);
}

uint8_t Device::demod_read(uint8_t chip, uint8_t address) {
    uint8_t value{};
    demod_read(chip, address, {&value, 1});
    return value;
}

void Device::tuner_write(uint8_t demod, uint8_t tuner, uint8_t address, uint8_t value) {
    uint8_t data[]{0xfe, tuner, address, value};
    usb_.transfer(request_out, i2c_stop, demod, data);
}

void Device::fc_write(std::initializer_list<Reg> regs) {
    for (const auto [address, value] : regs) {
        tuner_write(terrestrial, fc0013, address, value);
    }
}

uint8_t Device::fc_read(uint8_t address) {
    uint8_t select[]{0xfe, fc0013, address};
    uint8_t read[]{0xfe, fc0013 | 1};
    uint8_t value{};
    usb_.transfer(request_out, i2c_stop, terrestrial, select);
    usb_.transfer(request_out, i2c_continue, terrestrial, read);
    usb_.transfer(request_in, i2c_stop, terrestrial, {&value, 1});
    return value;
}

void Device::initialize() {
    locked_demod_ = 0;
    usb_.alt(0);
    bridge_write(0x0c, 0x10);
    bridge_write(0x06, 0x44);
    // GPIO1 は Low で電源 ON。GPIO7 のパルスで復調器のリセットを解く。
    bridge_write(0x80, 0x7c);
    std::this_thread::sleep_for(50ms);
    bridge_write(0x80, 0xfd);
    bridge_write(0x73, 0xfd);
    std::this_thread::sleep_for(50ms);

    demod_write(terrestrial, {{0x0e, 0x01}, {0x0f, 0x77}, {0x01, 0x40}, {0x20, 0x00},
        {0x21, 0xff}, {0x22, 0x8f}, {0x23, 0xfc}, {0x24, 0x4c}, {0x25, 0x80}, {0xf4, 0x80}, {0x1d, 0xa8}});
    demod_write(satellite, {{0x07, 0x01}, {0x08, 0x10}, {0x15, 0x00}, {0x1d, 0x00},
        {0x0a, 0xff}, {0x10, 0xb1}, {0x11, 0x40}, {0x85, 0x7a}, {0x87, 0x04},
        {0x1c, 0x80}, {0x1f, 0x22}, {0x03, 0x01}});
    for (uint8_t address = 1; address < std::size(qm_defaults); ++address) {
        tuner_write(satellite, qm1d1c0042, address, qm_defaults[address]);
    }
    constexpr uint8_t terrestrial_defaults[]{0x09, 0x16, 0x00, 0x00, 0x17, 0x02, 0x2a, 0xff,
        0x6f, 0xb8, 0x82, 0xfe, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x01};
    uint8_t address = 1;
    for (auto value : terrestrial_defaults) {
        tuner_write(terrestrial, fc0013, address++, value);
    }
    std::this_thread::sleep_for(50ms);
    for (uint8_t key_reg = 0x60; key_reg <= 0x6f; ++key_reg) {
        bridge_write(key_reg, 0);
    }
    bridge_write(0x5d, 0xef);
}

void Device::fc_tune(unsigned frequency) {
    fc_write({{0x1d, static_cast<uint8_t>(fc_read(0x1d) | 0x1c)}});
    fc_write({{0x07, static_cast<uint8_t>(fc_read(0x07) & 0xef)}});
    fc_write({{0x14, static_cast<uint8_t>((fc_read(0x14) & 0x1f) | 0x40)}});

    // 地デジ 13〜62ch で使う VCO 分周だけを、倍率の高い順に試す。
    constexpr struct { unsigned multiplier; uint8_t r05, r06; } divisors[]{
        {6, 0x0a, 0x00}, {4, 0x12, 0x02},
    };
    const auto div = frequency * divisors[0].multiplier < 3560000 ? divisors[0] : divisors[1];
    const auto vco = frequency * div.multiplier;
    auto p = pll(vco, 28800);
    uint8_t r09 = 0x4f;
    if (p.pm < 11) {
        p = pll(vco, 14400);
        r09 = 0x6f;
    }
    const auto r06 = static_cast<uint8_t>(div.r06 | 0x88); // 6 MHz と VCO 校正。
    fc_write({{0x01, p.am}, {0x02, p.pm}, {0x03, static_cast<uint8_t>(p.k >> 8)},
        {0x04, static_cast<uint8_t>(p.k)}, {0x05, static_cast<uint8_t>(div.r05 | 0x07)}, {0x06, r06}, {0x09, r09}});
    fc_write({{0x11, static_cast<uint8_t>(fc_read(0x11) & 0xfb)}});

    fc_write({{0x0e, 0x80}, {0x0e, 0x00}, {0x0e, 0x00}});
    if ((fc_read(0x0e) & 0x3f) > 0x3c) {
        fc_write({{0x06, static_cast<uint8_t>(r06 & 0xf7)}, {0x0e, 0x80}, {0x0e, 0x00}});
    }
    fc_write({{0x08, 0x1f}, {0x0c, 0xf8}, {0x14, 0x50}});
}

void Device::fc_adjust_lna() {
    const auto current = fc_read(0x14);
    fc_write({{0x12, 0x00}, {0x13, 0x00}});
    const auto r12 = fc_read(0x12);
    const auto r13 = fc_read(0x13);
    constexpr int lna_gain[]{10, 8, 4, 6, 0, 0, 0, 2};
    constexpr int pga_gain[]{0, 2, 6, 4};
    const int total = lna_gain[r13 >> 5] + pga_gain[(r12 >> 2) & 3] + pga_gain[r12 & 3] + (r13 & 0x1f) * 2;
    auto next = current;
    switch (current & 0x1f) {
    case 0x00:
        if (total > 0x1e) next = (current & 0xe0) | 0x08;
        break;
    case 0x08:
        if (total < 0x06) next = current & 0xe0;
        if (total > 0x32) next = (current & 0xf0) | 0x10;
        break;
    case 0x10:
        if (total < 0x1c) next = (current & 0xe0) | 0x08;
        break;
    default:
        next = (current & 0xf0) | 0x10;
    }
    if (next != current) {
        fc_write({{0x14, next}});
    }
}

bool Device::tune_terrestrial(unsigned channel) {
    if (channel < 13 || channel > 62) {
        throw std::invalid_argument("地デジの物理チャンネルは 13〜62 を指定してください");
    }
    locked_demod_ = 0;
    bridge_write(0x50, 0x01);
    // 2 基の復調器が TS バスを共有するので、衛星側は浮かせる。
    demod_write(satellite, {{0x1c, 0x80}, {0x1f, 0x22}});
    demod_write(terrestrial, {{0x0e, 0x01}, {0x0f, 0x77}, {0x1d, 0x00}, {0x1f, 0x00}});
    demod_write(terrestrial, {{0x25, 0x00}});
    demod_write(terrestrial, {{0x23, static_cast<uint8_t>(demod_read(terrestrial, 0x23) | 1)}});
    fc_tune(terrestrial_frequency(channel));
    demod_write(terrestrial, {{0x23, static_cast<uint8_t>(demod_read(terrestrial, 0x23) & 0xfe)}, {0x25, 0x80}});
    std::this_thread::sleep_for(50ms);
    demod_write(terrestrial, {{0x01, 0x40}});
    for (int attempt = 0; attempt < 15; ++attempt) {
        std::this_thread::sleep_for(40ms);
        fc_adjust_lna();
        if ((demod_read(terrestrial, 0x80) & 0x28) == 0) {
            locked_demod_ = terrestrial;
            return true;
        }
    }
    return false;
}

void Device::qm_tune(unsigned frequency) {
    constexpr struct { unsigned lower_khz; uint8_t bits; } bands[]{
        {1950000, 0xf0}, {1800000, 0xe0}, {1600000, 0xd0}, {1450000, 0xc0},
        {1250000, 0xb0}, {1200000, 0xa0}, {975000, 0x70}, {950000, 0x60},
    };
    const auto band = std::find_if(std::begin(bands), std::end(bands),
        [frequency](const auto& entry) { return frequency >= entry.lower_khz; });
    constexpr auto xtal_khz = 16000u;
    const auto divider = (frequency + xtal_khz / 2) / xtal_khz;
    const auto prescaler = (divider - 12) / 4;
    const auto counter = (divider - 4 * (prescaler + 1) - 5) & 0x0f;
    // 小数分周の負値は、3 レジスタの下位 22 ビットに格納する。
    const auto fraction = static_cast<uint32_t>(static_cast<int64_t>((uint64_t{frequency} << 20) / xtal_khz)
        - (static_cast<int64_t>(divider) << 20));
    const auto write = [this](uint8_t address, uint8_t value) { tuner_write(satellite, qm1d1c0042, address, value); };
    const auto r13 = static_cast<uint8_t>((qm_defaults[0x13] & 0x9f) | 0x20);
    write(0x02, static_cast<uint8_t>((qm_defaults[0x02] & 0x0f) | band->bits));
    write(0x06, static_cast<uint8_t>((qm_defaults[0x06] & 0x40) | prescaler));
    write(0x07, static_cast<uint8_t>((qm_defaults[0x07] & 0xf0) | counter));
    write(0x08, static_cast<uint8_t>((qm_defaults[0x08] & 0xf0) | 0x02));
    write(0x09, static_cast<uint8_t>((qm_defaults[0x09] & 0xc0) | ((fraction >> 16) & 0x3f)));
    write(0x0a, static_cast<uint8_t>(fraction >> 8));
    write(0x0b, static_cast<uint8_t>(fraction));
    write(0x13, r13);
    write(0x0c, qm_defaults[0x0c] & 0x3f);
    std::this_thread::sleep_for(3ms);
    write(0x0c, qm_defaults[0x0c] | 0xc0);
    std::this_thread::sleep_for(50ms);
    write(0x08, 0x09);
    write(0x13, r13);
}

uint16_t Device::tune_satellite(unsigned frequency_khz, uint16_t wanted_tsid) {
    if (frequency_khz < satellite_frequency(0) || frequency_khz > satellite_frequency(23) || !valid_tsid(wanted_tsid)) {
        throw std::invalid_argument("衛星の周波数または TSID が範囲外です");
    }
    locked_demod_ = 0;
    demod_write(terrestrial, {{0x0e, 0x77}, {0x1d, 0xa8}});
    demod_write(satellite, {{0x1f, 0x00}, {0x1c, 0x05}});
    qm_tune(frequency_khz);
    demod_write(satellite, {{0x03, 0x01}});
    if (!wait_satellite([this] { return (demod_read(satellite, 0xc3) & 0x10) == 0; })) {
        throw std::runtime_error("衛星の TMCC をロックできませんでした");
    }
    std::array<uint16_t, 12> ids{};
    // TMCC ロックの直後は 0xfff8 などの空き値しか読めないことがある。
    if (!wait_satellite([&] {
        std::array<uint8_t, 24> table{};
        demod_read(satellite, 0xce, table);
        for (size_t i = 0; i < ids.size(); ++i) {
            ids[i] = static_cast<uint16_t>((table[2 * i] << 8) | table[2 * i + 1]);
        }
        return std::any_of(ids.begin(), ids.end(), valid_tsid);
    })) {
        throw std::runtime_error("衛星の TMCC に有効な TSID がありませんでした");
    }
    const auto selected = match_tsid(wanted_tsid, ids);
    if (!selected) {
        std::string available;
        for (const auto id : ids) {
            if (valid_tsid(id)) available += std::format(" 0x{:04X}", id);
        }
        throw std::runtime_error(std::format("TMCC に要求した TSID 0x{:04X} がありません。受信した TSID{}", wanted_tsid, available));
    }
    demod_write(satellite, {{0x8f, static_cast<uint8_t>(selected >> 8)},
        {0x90, static_cast<uint8_t>(selected)}, {0x03, 0x01}});
    if (!wait_satellite([this] { return (demod_read(satellite, 0xc5) & 0x03) != 0; })) {
        throw std::runtime_error(std::format("衛星の TSID 0x{:04X} をロックできませんでした", selected));
    }
    locked_demod_ = satellite;
    return selected;
}

float Device::signal_level() {
    // 現行 recfriio と同じ TC90522 の C/N 変換式を使う。
    if (locked_demod_ == satellite) {
        std::array<uint8_t, 2> data{};
        demod_read(satellite, 0xbc, data);
        const auto raw = (data[0] << 8) | data[1];
        if (raw < 3000) return 0;
        const double p = std::sqrt(raw - 3000.0) / 64;
        const double cn = (((((-1634.6 * p + 14341.0) * p - 50259.0) * p + 88977.0) * p - 89565.0) * p + 58857.0) / 1000;
        return static_cast<float>(std::max(cn, 0.0));
    }
    if (locked_demod_ == terrestrial) {
        std::array<uint8_t, 3> data{};
        demod_read(terrestrial, 0x8b, data);
        const auto raw = (data[0] << 16) | (data[1] << 8) | data[2];
        if (raw == 0) return 0;
        const double p = 10 * std::log10(5505024.0 / raw);
        const double cn = (((0.000024 * p - 0.0016) * p + 0.0398) * p + 0.5491) * p + 3.0965;
        return static_cast<float>(std::max(cn, 0.0));
    }
    return 0;
}

void Device::start() {
    usb_.alt(1);
    bridge_write(0x5d, 0xef);
    bridge_write(0x5f, 0x89);
}

void Device::stop() {
    locked_demod_ = 0;
    try {
        bridge_write(0x5f, 0x08);
    } catch (...) {
        try { usb_.alt(0); } catch (...) {}
        throw;
    }
    usb_.alt(0);
}
}

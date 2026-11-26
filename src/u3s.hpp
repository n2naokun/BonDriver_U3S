#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace u3s {

struct Control {
    virtual void transfer(uint8_t type, uint8_t request, uint16_t index, std::span<uint8_t> data) = 0;
    virtual void alt(uint8_t setting) = 0;
    virtual ~Control() = default;
};

constexpr unsigned terrestrial_frequency(unsigned channel) {
    return 473143 + (channel - 13) * 6000;
}

// 設定表の 0〜11 は BS、12〜23 は 110 度 CS。
constexpr unsigned satellite_frequency(unsigned index) {
    return index < 12 ? 1049480u + index * 38360 : 1613000u + (index - 12) * 40000;
}

struct Channel {
    unsigned frequency_khz;
    unsigned terrestrial_channel;
    uint16_t tsid; // 地デジでは 0。
};

// 0xFFF0〜0xFFFF は TMCC の空きスロット値。
constexpr uint16_t max_satellite_tsid = 0xffef;

Channel parse_channel(std::wstring_view text);

class Device {
public:
    explicit Device(Control& usb) : usb_(usb) {}

    void initialize();
    bool tune_terrestrial(unsigned channel);
    uint16_t tune_satellite(unsigned frequency_khz, uint16_t wanted_tsid);
    float signal_level(); // C/N dB。未選局時は 0。
    void start();
    void stop();

private:
    struct Reg { uint8_t address, value; };

    void bridge_write(uint8_t address, uint8_t value);
    void demod_write(uint8_t chip, std::initializer_list<Reg> regs);
    void demod_read(uint8_t chip, uint8_t address, std::span<uint8_t> data);
    uint8_t demod_read(uint8_t chip, uint8_t address);
    void tuner_write(uint8_t demod, uint8_t tuner, uint8_t address, uint8_t value);
    void fc_write(std::initializer_list<Reg> regs);
    uint8_t fc_read(uint8_t address);
    void fc_tune(unsigned frequency);
    void fc_adjust_lna();
    void qm_tune(unsigned frequency);

    Control& usb_;
    uint8_t locked_demod_ = 0;
};

}

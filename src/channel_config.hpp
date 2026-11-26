#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace u3s {

struct ChannelEntry {
    std::wstring name;
    unsigned frequency_number;
    uint16_t tsid;
};

struct ChannelSpace {
    std::wstring name;
    std::vector<ChannelEntry> channels;
};

std::vector<ChannelSpace> load_channels(const std::filesystem::path& path);

}

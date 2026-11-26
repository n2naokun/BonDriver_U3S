#pragma once

#include "ts.hpp"
#include "winusb_device.hpp"

namespace u3s {

inline void feed_iso_block(TsPipeline& ts, const IsoBlock& block, std::vector<uint8_t>& output) {
    output.clear();
    if (block.missing_before) ts.gap();
    const auto data = std::span<const uint8_t>(block.data);
    size_t offset = 0;
    for (const auto gap : block.gaps) {
        ts.feed(data.subspan(offset, gap - offset), output);
        ts.gap();
        offset = gap;
    }
    ts.feed(data.subspan(offset), output);
}

}

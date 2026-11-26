#include "channel_config.hpp"
#include "u3s.hpp"

#include <windows.h>

#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace u3s {
namespace {

std::wstring decode(std::string_view bytes, UINT code_page) {
    if (bytes.empty()) return {};
    const auto length = static_cast<int>(bytes.size());
    const auto count = MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, bytes.data(), length, nullptr, 0);
    if (count == 0) return {};
    std::wstring value(count, L'\0');
    if (!MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, bytes.data(), length, value.data(), count)) return {};
    return value;
}

std::vector<std::wstring_view> split_tabs(std::wstring_view line) {
    std::vector<std::wstring_view> fields;
    for (auto tab = line.find(L'\t'); tab != line.npos; tab = line.find(L'\t')) {
        fields.push_back(line.substr(0, tab));
        line.remove_prefix(tab + 1);
    }
    fields.push_back(line);
    return fields;
}

}

std::vector<ChannelSpace> load_channels(const std::filesystem::path& path) {
    size_t line_number = 0;
    const auto fail = [&](const char* message) -> void {
        const auto name = path.u8string();
        throw std::runtime_error(std::string(name.begin(), name.end())
            + (line_number ? "(" + std::to_string(line_number) + ")" : "") + ": " + message);
    };
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) fail("ファイルを開けません");
    const auto size = file.tellg();
    if (size < 0 || !file.seekg(0)) fail("ファイルを読み込めません");
    if (size > std::numeric_limits<int>::max()) fail("ファイルが大きすぎます");
    std::string raw(static_cast<size_t>(size), '\0');
    if (!file.read(raw.data(), static_cast<std::streamsize>(size))) fail("ファイルを読み込めません");
    std::string_view bytes(raw);
    const bool bom = bytes.starts_with("\xEF\xBB\xBF");
    if (bom) bytes.remove_prefix(3);
    auto decoded = decode(bytes, CP_UTF8);
    if (decoded.empty() && !bytes.empty()) {
        if (!bom) decoded = decode(bytes, 932);
        if (decoded.empty()) fail("文字コードを判定できません。UTF-8 または CP932 を指定してください");
    }

    struct Entry { ChannelEntry channel; size_t line; };
    std::array<std::wstring, 3> names;
    std::array<std::map<unsigned, Entry>, 3> channels;
    const auto number = [&](std::wstring_view field, unsigned min, unsigned max, const char* error) {
        unsigned value = 0;
        if (field.empty()) fail(error);
        for (const auto c : field) {
            if (c < L'0' || c > L'9') fail(error);
            const auto digit = static_cast<unsigned>(c - L'0');
            if (digit > max || value > (max - digit) / 10) fail(error);
            value = value * 10 + digit;
        }
        if (value < min) fail(error);
        return value;
    };

    bool in_channels = false;
    bool found_section = false;
    size_t section_line = 0;
    std::wistringstream lines(std::move(decoded));
    for (std::wstring line; std::getline(lines, line);) {
        ++line_number;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const auto first = line.find_first_not_of(L" \t");
        if (first == line.npos || line[first] == L';') continue;
        const auto trimmed = std::wstring_view(line).substr(first, line.find_last_not_of(L" \t") - first + 1);
        if (trimmed.front() == L'[' && trimmed.back() == L']' && trimmed.find(L'\t') == trimmed.npos) {
            in_channels = trimmed == L"[CHANNEL]";
            if (in_channels && !found_section) {
                found_section = true;
                section_line = line_number;
            }
            continue;
        }
        if (!in_channels) continue;
        if (line.find(L'\0') != line.npos) fail("行に NUL 文字が含まれています");

        const bool declaration = line.front() == L'$';
        auto fields = split_tabs(line);
        if (fields.size() != (declaration ? 2u : 5u)) fail("列数が不正です。半角タブで区切ってください");
        if (declaration) fields[0].remove_prefix(1);
        if (fields[0].find_first_not_of(L' ') == fields[0].npos) fail("表示名が空です");
        const auto space = number(fields[1], 0, 2, "空間番号は 0〜2 の十進数を指定してください");
        if (declaration) {
            if (!names[space].empty()) fail("空間番号が重複しています");
            names[space] = fields[0];
            continue;
        }
        const auto channel = number(fields[2], 0, std::numeric_limits<unsigned>::max(), "チャンネル番号は十進数を指定してください");
        constexpr unsigned lower[] = {0, 12, 13};
        constexpr unsigned upper[] = {11, 23, 62};
        const auto frequency = number(fields[3], lower[space], upper[space], "周波数番号が範囲外か、十進数ではありません");
        const auto tsid = number(fields[4], space == 2 ? 0 : 1, space == 2 ? 0 : max_satellite_tsid, "TSID が範囲外か、十進数ではありません");
        const Entry entry{{std::wstring(fields[0]), frequency, static_cast<uint16_t>(tsid)}, line_number};
        if (!channels[space].emplace(channel, entry).second) fail("チャンネル番号が重複しています");
    }
    if (!found_section) {
        line_number = 0;
        fail("[CHANNEL] セクションがありません");
    }

    std::vector<ChannelSpace> result;
    for (size_t space = 0; space < names.size(); ++space) {
        if (names[space].empty()) {
            if (!channels[space].empty()) {
                line_number = channels[space].begin()->second.line;
                fail("空間番号が宣言されていません");
            }
            if (space < 2) {
                line_number = section_line;
                fail(space == 0 ? "空間番号 0 の宣言がありません" : "空間番号 1 の宣言がありません");
            }
            break;
        }
        ChannelSpace output{std::move(names[space]), {}};
        for (auto& [index, entry] : channels[space]) {
            if (index != output.channels.size()) {
                line_number = entry.line;
                fail("チャンネル番号は 0 からの連番を指定してください");
            }
            output.channels.push_back(std::move(entry.channel));
        }
        result.push_back(std::move(output));
    }
    // 旧 INI の空間 2 は読み込めるが、地デジのスキャン範囲は固定する。
    result.resize(2);
    auto& terrestrial = result.emplace_back(ChannelSpace{L"地デジ", {}});
    for (unsigned channel = 13; channel <= 52; ++channel) {
        terrestrial.channels.push_back({std::to_wstring(channel) + L"ch", channel, 0});
    }
    return result;
}

}

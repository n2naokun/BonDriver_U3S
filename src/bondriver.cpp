#include "bondriver_abi.hpp"
#include "channel_config.hpp"
#include "host_ipc.hpp"
#include "ts.hpp"

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace u3s;
using namespace u3s::host;

HMODULE module;
constexpr DWORD invalid_channel = 0xffffffff;

std::filesystem::path ini_path() {
    std::wstring name(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
        if (length == 0) win_error("BonDriver DLL のパスの取得");
        if (length < name.size()) {
            name.resize(length);
            return std::filesystem::path(name).replace_extension(L".ini");
        }
        if (name.size() >= 32768) throw std::runtime_error("BonDriver DLL のパスが長すぎます");
        name.resize(std::min<size_t>(name.size() * 2, 32768));
    }
}

class BonDriver final : public IBonDriver2 {
public:
    explicit BonDriver(std::filesystem::path path) : path_(std::move(path)) {
        // ホストが開く前にも列挙できるよう読み、正式な検証と診断は OpenTuner で行う。
        try { spaces_ = load_channels(path_); } catch (const std::exception&) {}
    }

    const BOOL OpenTuner() override {
        std::lock_guard lock(control_);
        if (connection_ && !host_lost_ && connection_->alive()) return TRUE;
        close_tuner();
        try {
            auto spaces = load_channels(path_);
            sid_ = target_sid();
            connection_ = std::make_unique<Connection>(sid_);
            const auto reply = connection_->call(Frame(Op::open_tuner), ticket_.size());
            if (reply.code || reply.data.size() != ticket_.size())
                throw std::runtime_error("U3SHost が空いているチューナーを確保できませんでした");
            std::copy(reply.data.begin(), reply.data.end(), ticket_.begin());
            host_lost_ = false;
            spaces_ = std::move(spaces);
            return TRUE;
        } catch (const std::exception& error) {
            trace("チューナーを開く", error.what());
            close_tuner();
            return FALSE;
        }
    }

    void CloseTuner() override {
        std::lock_guard lock(control_);
        close_tuner();
    }

    const BOOL SetChannel(const BYTE physical_channel) override {
        std::lock_guard lock(control_);
        if (spaces_.size() <= 2) return FALSE;
        const auto& channels = spaces_[2].channels;
        const auto channel = std::find_if(channels.begin(), channels.end(), [physical_channel](const ChannelEntry& entry) {
            return entry.frequency_number == physical_channel;
        });
        if (channel == channels.end()) return FALSE;
        return set_channel(2, static_cast<DWORD>(channel - channels.begin()));
    }

    const BOOL SetChannel(const DWORD space, const DWORD channel) override {
        std::lock_guard lock(control_);
        return set_channel(space, channel);
    }

    const float GetSignalLevel() override {
        std::lock_guard lock(control_);
        if (!connection_ || GetCurSpace() == invalid_channel) return 0.0f;
        try {
            const auto reply = connection_->call(Frame(Op::signal));
            return reply.code ? 0.0f : std::bit_cast<float>(reply.a);
        } catch (const std::exception& error) {
            trace("C/N の取得", error.what());
            return 0.0f;
        }
    }

    const DWORD WaitTsStream(const DWORD timeout) override {
        std::unique_lock lock(state_);
        if (space_ == invalid_channel) return WAIT_ABANDONED;
        const auto generation = stream_generation_;
        const auto ready = [this, generation] { return stream_generation_ != generation || !queue_.empty(); };
        ++waiters_;
        bool signaled = true;
        if (timeout == 0 || timeout == INFINITE) ready_.wait(lock, ready);
        else signaled = ready_.wait_for(lock, std::chrono::milliseconds(timeout), ready);
        --waiters_;
        if (waiters_ == 0) ready_.notify_all();
        if (stream_generation_ != generation) return WAIT_ABANDONED;
        return signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }

    const DWORD GetReadyCount() override {
        std::lock_guard lock(state_);
        return static_cast<DWORD>(queue_.size());
    }

    const BOOL GetTsStream(BYTE* data, DWORD* size, DWORD* remain) override {
        if (!size || !remain) return FALSE;
        std::lock_guard lock(state_);
        const auto capacity = *size;
        *size = 0;
        *remain = static_cast<DWORD>(queue_.size());
        if (!data || space_ == invalid_channel) return FALSE;
        if (queue_.empty()) return TRUE;
        const auto& chunk = queue_.front();
        if (capacity < chunk.size()) return FALSE;
        std::memcpy(data, chunk.data(), chunk.size());
        *size = static_cast<DWORD>(chunk.size());
        queue_.pop_front();
        --*remain;
        return TRUE;
    }

    const BOOL GetTsStream(BYTE** data, DWORD* size, DWORD* remain) override {
        if (!data || !size || !remain) return FALSE;
        std::lock_guard lock(state_);
        *data = nullptr;
        *size = *remain = 0;
        if (space_ == invalid_channel) return FALSE;
        if (queue_.empty()) return TRUE;
        // 返した領域は受信側のキューから分離し、破棄や選局変更でも参照を壊さない。
        last_chunk_ = std::move(queue_.front());
        queue_.pop_front();
        *data = last_chunk_.data();
        *size = static_cast<DWORD>(last_chunk_.size());
        *remain = static_cast<DWORD>(queue_.size());
        return TRUE;
    }

    void PurgeTsStream() override {
        std::lock_guard lock(control_);
        const auto space = GetCurSpace();
        const auto channel = GetCurChannel();
        if (space == invalid_channel) return;
        stop_stream(true);
        try {
            if (connection_->call(Frame(Op::purge)).code) throw std::runtime_error("Host 側の TS 破棄に失敗しました");
            start_reader(space, channel);
            host_lost_ = false;
        } catch (const std::exception& error) {
            trace("TS の破棄", error.what());
            end_stream();
        }
    }

    const wchar_t* GetTunerName() override { return L"SK-MTVU3S"; }

    const BOOL IsTunerOpening() override {
        std::lock_guard lock(control_);
        return connection_ && !host_lost_ && connection_->alive();
    }

    const wchar_t* const EnumTuningSpace(const DWORD space) override {
        std::lock_guard lock(control_);
        return space < spaces_.size() ? spaces_[space].name.c_str() : nullptr;
    }

    const wchar_t* const EnumChannelName(const DWORD space, const DWORD channel) override {
        std::lock_guard lock(control_);
        if (space >= spaces_.size() || channel >= spaces_[space].channels.size()) return nullptr;
        return spaces_[space].channels[channel].name.c_str();
    }

    const DWORD GetCurSpace() override {
        std::lock_guard lock(state_);
        return space_;
    }

    const DWORD GetCurChannel() override {
        std::lock_guard lock(state_);
        return channel_;
    }

    void Release() override {
        CloseTuner();
        {
            std::unique_lock lock(state_);
            ready_.wait(lock, [this] { return waiters_ == 0; });
        }
        delete this;
    }

private:
    void close_tuner() {
        stop_stream();
        if (connection_ && connection_->alive()) {
            try { connection_->call(Frame(Op::close_tuner)); }
            catch (const std::exception& error) { trace("チューナーを閉じる", error.what()); }
        }
        connection_.reset();
    }

    bool set_channel(DWORD space, DWORD channel) {
        if (!connection_ || host_lost_ || space >= spaces_.size() || channel >= spaces_[space].channels.size()) return false;
        stop_stream();
        const auto& entry = spaces_[space].channels[channel];
        try {
            const auto reply = connection_->call(Frame(Op::tune, entry.frequency_number, entry.tsid));
            if (reply.code) throw std::runtime_error("U3SHost の選局に失敗しました");
            start_reader(space, channel);
            return true;
        } catch (const std::exception& error) {
            trace("選局", error.what());
            return false;
        }
    }

    void end_stream() {
        {
            std::lock_guard lock(state_);
            queue_.clear();
            space_ = channel_ = invalid_channel;
            ++stream_generation_;
        }
        ready_.notify_all();
    }

    void stop_stream(bool preserve_channel = false) {
        stop_requested_ = true;
        if (!preserve_channel) end_stream();
        if (worker_.joinable()) worker_.join();
        if (preserve_channel) {
            std::lock_guard lock(state_);
            queue_.clear();
        }
    }

    void start_reader(DWORD space, DWORD channel) {
        try {
            Connection ts(sid_, true);
            if (ts.call(Frame(Op::attach, 0, 0, ticket_)).code)
                throw std::runtime_error("U3SHost の TS セッションに接続できません");
            {
                std::lock_guard lock(state_);
                space_ = space;
                channel_ = channel;
            }
            stop_requested_ = false;
            worker_ = std::thread([this, ts = std::move(ts)]() mutable {
                uint64_t dropped = 0;
                try {
                    while (!stop_requested_) {
                        auto reply = ts.call(Frame(Op::read_ts), ts_chunk_size);
                        if (reply.code) break;
                        if (reply.data.empty()) continue;
                        if (reply.data.size() % ts_packet_size) throw std::runtime_error("不正な TS サイズです");
                        {
                            std::lock_guard lock(state_);
                            if (stop_requested_) break;
                            if (queue_.size() == ts_queue_capacity) {
                                queue_.pop_front();
                                ++dropped;
                            }
                            queue_.push_back(std::move(reply.data));
                        }
                        ready_.notify_all();
                    }
                } catch (const std::exception& error) {
                    if (!stop_requested_) host_lost_ = true;
                    trace("TS の受信", error.what());
                }
                if (!stop_requested_) end_stream();
                if (dropped) {
                    wchar_t message[128]{};
                    std::swprintf(message, std::size(message), L"BonDriver_U3S: queue_dropped_blocks=%llu\n",
                        static_cast<unsigned long long>(dropped));
                    OutputDebugStringW(message);
                }
            });
        } catch (...) {
            end_stream();
            throw;
        }
    }

    std::mutex control_;
    std::mutex state_;
    std::condition_variable ready_;
    std::filesystem::path path_;
    std::vector<ChannelSpace> spaces_;
    std::wstring sid_;
    Ticket ticket_{};
    std::unique_ptr<Connection> connection_;
    std::deque<std::vector<uint8_t>> queue_;
    std::vector<uint8_t> last_chunk_;
    DWORD space_ = invalid_channel;
    DWORD channel_ = invalid_channel;
    uint64_t stream_generation_ = 0;
    unsigned waiters_ = 0;
    std::atomic_bool host_lost_ = false;
    std::atomic_bool stop_requested_ = false;
    std::thread worker_;
};

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) module = instance;
    return TRUE;
}

extern "C" __declspec(dllexport) IBonDriver* CreateBonDriver() {
    try {
        return new BonDriver(ini_path());
    } catch (const std::exception& error) {
        trace("インスタンスを作成", error.what());
        return nullptr;
    }
}

#include "card_slot.hpp"
#include "host_ipc.hpp"
#include "iso_ts.hpp"

#include <bcrypt.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>

namespace {
using namespace u3s;
using namespace u3s::host;
using namespace std::chrono_literals;

Frame error(LONG code) {
    return Frame(static_cast<uint32_t>(code));
}

struct Physical {
    explicit Physical(const std::wstring& path) : usb(path), tuner(usb), card(usb) {
        // 初回確保時にだけ 0x73 を含む初期化を済ませ、カード通信開始後は再実行しない。
        tuner.initialize();
    }

    WinUsb usb;
    Device tuner;
    CardSlot card;
    std::mutex hardware;
    std::mutex card_mutex;
    bool receiving = false;
    std::atomic_uint cards = 0;
    bool exclusive = false;
};

struct TsChannel {
    explicit TsChannel(const Caller& caller_) : caller(caller_) {}

    Frame read() {
        std::unique_lock lock(mutex);
        ready.wait_for(lock, 100ms, [&] { return !active || !queue.empty(); });
        if (!active) return Frame(ERROR_NOT_READY);
        Frame reply;
        if (!queue.empty()) {
            reply.data = std::move(queue.front());
            queue.pop_front();
        }
        return reply;
    }

    void end() {
        {
            std::lock_guard lock(mutex);
            active = false;
            queue.clear();
        }
        ready.notify_all();
    }

    const Caller caller;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::vector<uint8_t>> queue;
    bool active = false;
    uint64_t purge_generation = 0;
};

class Reception {
public:
    Reception(std::shared_ptr<Physical> device_, const Caller& caller)
        : device(std::move(device_)), ts(std::make_shared<TsChannel>(caller)) {
        if (BCryptGenRandom(nullptr, ticket.data(), static_cast<ULONG>(ticket.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("受信セッション ID の生成に失敗しました");
    }

    ~Reception() { stop(); }

    bool tune(unsigned number, unsigned tsid) {
        if ((!tsid && (number < 13 || number > 62)) || (tsid && (number > 23 || tsid > max_satellite_tsid)))
            return false;
        if (!stop()) return false;
        try {
            {
                std::lock_guard lock(device->hardware);
                idle_ = false;
                if (!tsid) {
                    if (!device->tuner.tune_terrestrial(number)) throw std::runtime_error("地デジをロックできませんでした");
                } else {
                    device->tuner.tune_satellite(satellite_frequency(number), static_cast<uint16_t>(tsid));
                }
                device->tuner.start();
            }
            std::promise<void> registration;
            auto registered = registration.get_future();
            stop_requested_ = false;
            worker_ = std::thread([this, registration = std::move(registration)]() mutable {
                std::unique_ptr<IsoStream> iso;
                std::unique_ptr<TsPipeline> pipeline;
                try {
                    // バッファは登録したスレッドで受信を続ける必要がある。
                    const std::lock_guard lock(device->hardware);
                    iso = std::make_unique<IsoStream>(device->usb);
                    pipeline = std::make_unique<TsPipeline>();
                    const std::lock_guard queue_lock(ts->mutex);
                    ts->active = true;
                    registration.set_value();
                } catch (...) {
                    registration.set_exception(std::current_exception());
                    return;
                }
                receive(*iso, *pipeline);
                ts->end();
                // ISO 転送を回収してから代替設定を変更する。
                iso.reset();
                stop_device();
            });
            registered.get();
            return true;
        } catch (const std::exception& e) {
            trace("選局", e.what());
            stop();
            return false;
        }
    }

    float signal() {
        const std::lock_guard lock(device->hardware);
        return device->tuner.signal_level();
    }

    void purge() {
        const std::lock_guard lock(ts->mutex);
        ts->queue.clear();
        ++ts->purge_generation;
    }

    const std::shared_ptr<Physical> device;
    const std::shared_ptr<TsChannel> ts;
    Ticket ticket{};

private:
    bool stop_device() noexcept {
        const std::lock_guard lock(device->hardware);
        if (idle_) return true;
        try {
            device->tuner.stop();
            idle_ = true;
        } catch (const std::exception& e) {
            trace("チューナー停止", e.what());
        }
        return idle_;
    }

    bool stop() {
        stop_requested_ = true;
        ts->end();
        if (worker_.joinable()) worker_.join();
        return stop_device();
    }

    void receive(IsoStream& iso, TsPipeline& pipeline) {
        uint64_t dropped = 0;
        try {
            std::vector<uint8_t> output;
            uint64_t generation = 0;
            iso.read(std::chrono::seconds(INT_MAX), stop_requested_, [&](IsoBlock block) {
                uint64_t current = 0;
                {
                    const std::lock_guard lock(ts->mutex);
                    current = ts->purge_generation;
                }
                if (generation != current) pipeline.gap();
                generation = current;
                feed_iso_block(pipeline, block, output);
                if (output.empty()) return;
                {
                    const std::lock_guard lock(ts->mutex);
                    if (!ts->active || current != ts->purge_generation) return;
                    for (size_t start = 0; start < output.size(); start += ts_chunk_size) {
                        const auto end = std::min(start + ts_chunk_size, output.size());
                        if (ts->queue.size() == ts_queue_capacity) {
                            ts->queue.pop_front();
                            ++dropped;
                        }
                        ts->queue.emplace_back(output.data() + start, output.data() + end);
                    }
                }
                ts->ready.notify_one();
            });
        } catch (const std::exception& e) {
            trace("TS の受信", e.what());
        }
        const auto stats = iso.stats();
        wchar_t message[192]{};
        std::swprintf(message, std::size(message), L"U3SHost: usb_failed_packets=%llu usb_restarts=%llu queue_dropped_blocks=%llu\n",
            static_cast<unsigned long long>(stats.failed_packets), static_cast<unsigned long long>(stats.restarts),
            static_cast<unsigned long long>(dropped));
        OutputDebugStringW(message);
    }

    bool idle_ = true;
    std::atomic_bool stop_requested_ = false;
    std::thread worker_;
};

enum class CardMode { single, fill, balance };

constexpr bool prefer_card(CardMode mode, unsigned candidate, unsigned current) {
    return mode == CardMode::balance ? candidate < current : candidate > current;
}

static_assert(prefer_card(CardMode::fill, 1, 0));
static_assert(prefer_card(CardMode::balance, 0, 1));

class Host {
public:
    Host(CardMode card_mode, unsigned card_limit) : card_mode_(card_mode), card_limit_(card_limit) {}

    std::unique_ptr<Reception> open_tuner(const Caller& caller) {
        const auto entries = list_winusb();
        const auto deadline = std::chrono::steady_clock::now() + 25s;
        const auto reserve = [&](const std::shared_ptr<Physical>& device) -> std::unique_ptr<Reception> {
            if (!device || device->receiving) return nullptr;
            auto session = std::make_unique<Reception>(device, caller);
            if (!streams_.emplace(session->ticket, session->ts).second)
                throw std::runtime_error("受信セッション ID が重複しました");
            device->receiving = true;
            return session;
        };
        std::vector<std::shared_future<std::shared_ptr<Physical>>> deferred;
        for (const auto& entry : entries) {
            if (std::chrono::steady_clock::now() >= deadline) return nullptr;
            std::shared_future<std::shared_ptr<Physical>> pending;
            auto device = get(entry, &pending);
            if (pending.valid()) deferred.push_back(std::move(pending));
            const std::lock_guard lock(allocation_);
            if (auto session = reserve(device)) return session;
        }

        // 他の要求が始めた初期化の結果だけを待つ。失敗した個体は開き直さない。
        const auto ready = [](const auto& pending) { return pending.wait_for(0s) == std::future_status::ready; };
        std::unique_lock lock(allocation_);
        while (!deferred.empty() && std::chrono::steady_clock::now() < deadline) {
            if (!initialization_changed_.wait_until(lock, deadline, [&] {
                    return std::any_of(deferred.begin(), deferred.end(), ready);
                })) break;
            for (auto item = deferred.begin(); item != deferred.end();) {
                if (!ready(*item)) {
                    ++item;
                    continue;
                }
                if (auto session = reserve(item->get())) return session;
                item = deferred.erase(item);
            }
        }
        return nullptr;
    }

    void close_tuner(std::unique_ptr<Reception> session) {
        if (!session) return;
        const auto device = session->device;
        const auto ticket = session->ticket;
        session.reset();
        const std::lock_guard lock(allocation_);
        streams_.erase(ticket);
        device->receiving = false;
    }

    std::shared_ptr<TsChannel> attach(const Ticket& ticket, const Caller& caller) {
        const std::lock_guard lock(allocation_);
        const auto it = streams_.find(ticket);
        if (it == streams_.end() || it->second->caller != caller) return nullptr;
        return it->second;
    }

    Frame open_card(std::shared_ptr<Physical>& card, bool exclusive) {
        // Singleでは最初のカードが使えると確認するまで、次の接続を待たせる。
        std::unique_lock single_open(single_open_, std::defer_lock);
        if (card_mode_ == CardMode::single) single_open.lock();
        const auto paths = list_winusb();
        if (paths.empty()) return error(SCARD_E_NO_READERS_AVAILABLE);
        std::vector<std::shared_ptr<Physical>> devices;
        std::vector<std::wstring> unopened;
        {
            const std::lock_guard lock(allocation_);
            for (const auto& path : paths) {
                const auto found = devices_.find(path);
                auto device = found == devices_.end() ? nullptr : found->second.lock();
                if (device) devices.push_back(std::move(device));
                else unopened.push_back(path);
            }
        }
        auto next = unopened.begin();
        LONG failure = SCARD_E_READER_UNAVAILABLE;
        bool sharing_violation = false;
        std::vector<std::shared_ptr<Physical>> tried;
        for (;;) {
            if (card_mode_ == CardMode::balance) {
                for (; next != unopened.end(); ++next)
                    if (auto device = get(*next)) devices.push_back(std::move(device));
            }
            std::shared_ptr<Physical> selected;
            {
                const std::scoped_lock lock(allocation_, card_selection_);
                sharing_violation = false;
                for (const auto& path : paths) {
                    const auto found = devices_.find(path);
                    if (found == devices_.end()) continue;
                    auto device = found->second.lock();
                    if (device && std::find(devices.begin(), devices.end(), device) == devices.end())
                        devices.push_back(std::move(device));
                }
                const auto shared = single_card_.lock();
                const bool pinned = shared && shared->cards != 0;
                if (pinned && std::find(devices.begin(), devices.end(), shared) == devices.end())
                    devices.push_back(shared);
                for (const auto& device : devices) {
                    if ((pinned && device != shared) || std::find(tried.begin(), tried.end(), device) != tried.end())
                        continue;
                    if (device->exclusive || (exclusive && device->cards)
                        || (card_limit_ && device->cards >= card_limit_)) {
                        sharing_violation = true;
                        continue;
                    }
                    if (!selected || prefer_card(card_mode_, device->cards, selected->cards)) selected = device;
                }
                if (selected) {
                    ++selected->cards;
                    selected->exclusive = exclusive;
                    if (card_mode_ == CardMode::single) single_card_ = selected;
                } else if (pinned) {
                    return error(SCARD_E_SHARING_VIOLATION);
                }
            }
            if (!selected) {
                if (next == unopened.end()) break;
                if (auto device = get(*next)) devices.push_back(std::move(device));
                ++next;
                continue;
            }
            tried.push_back(selected);
            const auto reply = initialize_card(selected);
            if (!reply.code) { card = selected; return reply; }
            {
                const std::lock_guard lock(card_selection_);
                if (--selected->cards == 0) selected->exclusive = false;
                else if (card_mode_ == CardMode::single) return reply;
            }
            failure = static_cast<LONG>(reply.code);
        }
        return error(sharing_violation ? SCARD_E_SHARING_VIOLATION : failure);
    }

    Frame close_card(std::shared_ptr<Physical> device, bool reset) {
        if (!device) return error(SCARD_E_INVALID_HANDLE);
        {
            const std::lock_guard lock(card_selection_);
            if (device->cards > 1) {
                --device->cards;
                return reset ? error(SCARD_E_SHARING_VIOLATION) : Frame{};
            }
            if (!reset) {
                --device->cards;
                device->exclusive = false;
                return {};
            }
            // リセットが終わるまで最終接続の予約を維持する。
            device->exclusive = true;
        }
        LONG result = SCARD_E_READER_UNAVAILABLE;
        try {
            const std::lock_guard hardware_lock(device->hardware);
            result = device->card.reset();
        } catch (const std::exception& e) {
            trace("カードをリセット", e.what());
        }
        {
            const std::lock_guard lock(card_selection_);
            --device->cards;
            device->exclusive = false;
        }
        return error(result);
    }

private:
    std::shared_ptr<Physical> get(const std::wstring& path,
                                  std::shared_future<std::shared_ptr<Physical>>* deferred = nullptr) {
        std::shared_future<std::shared_ptr<Physical>> pending;
        std::shared_ptr<std::promise<std::shared_ptr<Physical>>> promise;
        {
            const std::lock_guard lock(allocation_);
            if (auto device = devices_[path].lock()) return device;
            const auto found = initializing_.find(path);
            if (found != initializing_.end()) {
                pending = found->second;
                if (deferred) {
                    *deferred = pending;
                    return nullptr;
                }
            }
            else {
                promise = std::make_shared<std::promise<std::shared_ptr<Physical>>>();
                pending = promise->get_future().share();
                initializing_.emplace(path, pending);
            }
        }
        if (!promise) return pending.get();
        std::shared_ptr<Physical> device;
        try { device = std::make_shared<Physical>(path); }
        catch (const std::exception& e) { trace("USB デバイスを確保", e.what()); }
        {
            const std::lock_guard lock(allocation_);
            devices_[path] = device;
            promise->set_value(device);
            initializing_.erase(path);
        }
        initialization_changed_.notify_all();
        return device;
    }

    static Frame initialize_card(const std::shared_ptr<Physical>& device) {
        const std::lock_guard lock(device->hardware);
        try {
            if (!device->card.ready()) {
                const auto result = device->card.reset();
                if (result != SCARD_S_SUCCESS) return error(result);
            }
            return Frame(0, 0, 0, device->card.atr);
        } catch (const std::exception& e) {
            trace("カードを初期化", e.what());
            return error(SCARD_E_READER_UNAVAILABLE);
        }
    }

    const CardMode card_mode_;
    const unsigned card_limit_;
    std::mutex allocation_;
    std::mutex card_selection_;
    std::mutex single_open_;
    std::weak_ptr<Physical> single_card_;
    std::condition_variable initialization_changed_;
    std::map<std::wstring, std::weak_ptr<Physical>> devices_;
    std::map<std::wstring, std::shared_future<std::shared_ptr<Physical>>> initializing_;
    std::map<Ticket, std::shared_ptr<TsChannel>> streams_;
};

struct Client {
    Host& host;
    Caller caller;
    std::unique_ptr<Reception> tuner;
    std::shared_ptr<Physical> card;

    ~Client() {
        host.close_tuner(std::move(tuner));
        if (card) host.close_card(std::move(card), false);
    }

    Frame request(const Frame& frame) {
        const auto op = static_cast<Op>(frame.code);
        if (op != Op::transmit && !frame.data.empty()) return Frame(ERROR_INVALID_PARAMETER);
        switch (op) {
        case Op::readers:
            return list_winusb().empty() ? error(SCARD_E_NO_READERS_AVAILABLE) : Frame{};
        case Op::open_tuner:
            if (tuner) return Frame(ERROR_BUSY);
            tuner = host.open_tuner(caller);
            return tuner ? Frame(0, 0, 0, tuner->ticket) : Frame(ERROR_BUSY);
        case Op::close_tuner:
            host.close_tuner(std::move(tuner));
            return {};
        case Op::tune:
            return Frame(tuner && tuner->tune(frame.a, frame.b) ? ERROR_SUCCESS : ERROR_NOT_READY);
        case Op::signal:
            return tuner ? Frame(0, std::bit_cast<uint32_t>(tuner->signal())) : Frame(ERROR_INVALID_HANDLE);
        case Op::purge:
            if (!tuner) return Frame(ERROR_INVALID_HANDLE);
            tuner->purge();
            return {};
        case Op::open_card:
            if (card) return error(SCARD_E_SHARING_VIOLATION);
            if (frame.a != SCARD_SHARE_SHARED && frame.a != SCARD_SHARE_EXCLUSIVE) return error(SCARD_E_INVALID_VALUE);
            return host.open_card(card, frame.a == SCARD_SHARE_EXCLUSIVE);
        case Op::card_status:
            if (!card) return error(SCARD_E_INVALID_HANDLE);
            {
                const std::lock_guard lock(card->hardware);
                return card->card.ready() ? Frame(0, 0, 0, card->card.atr) : error(SCARD_W_RESET_CARD);
            }
        case Op::transmit:
            if (!card) return error(SCARD_E_INVALID_HANDLE);
            if (frame.data.size() < 4 || frame.data.size() > 124) return error(SCARD_E_INVALID_PARAMETER);
            {
                const std::lock_guard lock(card->hardware);
                std::array<BYTE, 254> response{};
                DWORD size = static_cast<DWORD>(response.size());
                const auto result = card->card.command(frame.data, response.data(), &size);
                return result ? error(result) : Frame(0, 0, 0, std::span(response).first(size));
            }
        case Op::disconnect_card:
            if (frame.a != SCARD_LEAVE_CARD && frame.a != SCARD_RESET_CARD) return error(SCARD_E_INVALID_VALUE);
            return host.close_card(std::move(card), frame.a == SCARD_RESET_CARD);
        default: return Frame(ERROR_INVALID_FUNCTION);
        }
    }
};

void control_client(Host& host, HANDLE pipe, Caller caller) {
    Client client{host, std::move(caller), {}, {}};
    for (;;) {
        const auto request = read_frame(pipe, 124);
        Frame reply;
        try {
            reply = client.request(request);
        } catch (const std::exception& e) {
            trace("Host の制御要求", e.what());
            reply = error(SCARD_E_READER_UNAVAILABLE);
        }
        write_frame(pipe, reply);
    }
}

void ts_client(Host& host, HANDLE pipe, const Caller& caller) {
    const auto request = read_frame(pipe, Ticket{}.size());
    Ticket ticket{};
    if (request.code != static_cast<uint32_t>(Op::attach) || request.data.size() != ticket.size())
        throw std::runtime_error("TS セッション ID が不正です");
    std::copy(request.data.begin(), request.data.end(), ticket.begin());
    const auto ts = host.attach(ticket, caller);
    write_frame(pipe, Frame(ts ? ERROR_SUCCESS : ERROR_ACCESS_DENIED));
    if (!ts) return;
    for (;;) {
        if (read_frame(pipe, 0).code != static_cast<uint32_t>(Op::read_ts)) return;
        write_frame(pipe, ts->read());
    }
}

void listen(Host& host, const std::wstring& sid, bool ts, WinHandle next) {
    const auto active = std::make_shared<std::atomic_uint>(0);
    for (;;) {
        if (!ConnectNamedPipe(next.get(), nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            if (!DisconnectNamedPipe(next.get()) && GetLastError() != ERROR_PIPE_NOT_CONNECTED)
                win_error("Host パイプを再接続");
            continue;
        }
        auto pipe = std::move(next);
        // 常に待ち受けを一つ残し、接続中の利用者を別スレッドに渡す。
        next = create_listener(sid, ts, false);
        if (++*active > 64) {
            --*active;
            continue;
        }
        std::thread([&host, sid, ts, active, pipe = std::move(pipe)] {
            try {
                auto caller = authenticate(pipe.get(), sid);
                if (ts) ts_client(host, pipe.get(), caller);
                else control_client(host, pipe.get(), std::move(caller));
            } catch (const std::exception& e) {
                // 通常の切断でも待機中の ReadFile はエラーで終了する。
                if (PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, nullptr, nullptr)) trace("Host クライアント", e.what());
            }
            --*active;
        }).detach();
    }
}

int start_and_wait(const std::wstring& sid, const std::filesystem::path& executable) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    const auto ready = [&] {
        try {
            const auto remaining = [&] {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                return static_cast<DWORD>((std::max)(int64_t{1}, ms));
            };
            if (std::chrono::steady_clock::now() >= deadline) return false;
            const Connection ts(sid, true, remaining());
            if (std::chrono::steady_clock::now() >= deadline) return false;
            const Connection control(sid, false, remaining());
            return std::chrono::steady_clock::now() < deadline;
        } catch (const std::exception&) {
            return false;
        }
    };
    if (ready()) return 0;
    if (std::chrono::steady_clock::now() >= deadline) return 1;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &process))
        win_error("U3SHost を起動");
    const WinHandle thread(process.hThread), handle(process.hProcess);
    do {
        if (ready()) return 0;
        const auto now = std::chrono::steady_clock::now();
        if (now < deadline) std::this_thread::sleep_until((std::min)(deadline, now + 100ms));
    } while (std::chrono::steady_clock::now() < deadline);
    return 1;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    try {
        std::wstring path(32768, L'\0');
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size || size >= path.size()) win_error("U3SHost.exe のパス取得");
        path.resize(size);
        const std::filesystem::path executable(path);
        const auto sid = current_sid();
        if (std::wstring_view(arguments) == L"--start") return start_and_wait(sid, executable);
        auto control = create_listener(sid, false, true);
        auto ts = create_listener(sid, true, true);
        const auto ini = executable.parent_path() / L"U3SHost.ini";
        // スレッドはプロセス終了まで待ち受ける。
        const auto legacy_mode = GetPrivateProfileIntW(L"Card", L"Distribute", 0, ini.c_str()) == 1
            ? L"Balance" : L"Single";
        wchar_t mode[32]{};
        GetPrivateProfileStringW(L"Card", L"CardSharingMode", legacy_mode, mode, 32, ini.c_str());
        CardMode card_mode;
        if (_wcsicmp(mode, L"Single") == 0) card_mode = CardMode::single;
        else if (_wcsicmp(mode, L"Fill") == 0) card_mode = CardMode::fill;
        else if (_wcsicmp(mode, L"Balance") == 0) card_mode = CardMode::balance;
        else throw std::runtime_error("CardSharingMode は Single、Fill、Balance のいずれかを指定してください");

        wchar_t limit[32]{};
        GetPrivateProfileStringW(L"Card", L"MaxConnectionsPerCard", L"0", limit, 32, ini.c_str());
        const std::wstring_view digits(limit);
        if (digits.empty() || digits.size() == 31 || digits.find_first_not_of(L"0123456789") != digits.npos)
            throw std::runtime_error("MaxConnectionsPerCard は 0 以上の整数を指定してください");
        unsigned card_limit = 0;
        for (const auto digit : digits) {
            if (card_limit > (UINT_MAX - (digit - L'0')) / 10)
                throw std::runtime_error("MaxConnectionsPerCard が大きすぎます");
            card_limit = card_limit * 10 + (digit - L'0');
        }
        static Host host(card_mode, card_limit);
        std::thread([&] {
            try {
                listen(host, sid, true, std::move(ts));
            } catch (const std::exception& e) {
                trace("TS パイプ待ち受け", e.what());
                ExitProcess(1);
            }
        }).detach();
        listen(host, sid, false, std::move(control));
    } catch (const std::exception& e) {
        trace("U3SHost 起動", e.what());
        ExitProcess(1);
    }
    return 0;
}

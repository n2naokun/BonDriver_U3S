#include "host_ipc.hpp"

#define WINSCARDDATA // DLL内でPCIデータを定義し、.defからエクスポートする。
#include <winscard.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string_view>

namespace {

using namespace u3s::host;
using View = std::span<const BYTE>;
constexpr char reader_a[] = "SK-MTVU3S B-CAS\0";
constexpr wchar_t reader_w[] = L"SK-MTVU3S B-CAS\0";
struct Context {
    std::shared_ptr<std::mutex> connections = std::make_shared<std::mutex>();
    std::map<const void*, std::unique_ptr<BYTE[]>> allocations;
};

struct Card {
    Card(SCARDCONTEXT context_, Connection&& host_) : context(context_), host(std::move(host_)) {}
    SCARDCONTEXT context;
    Connection host;
    std::mutex mutex;
    bool closed = false;
    bool closing = false; // access で保護する。
};

std::mutex access;
std::map<SCARDCONTEXT, Context> contexts;
std::map<SCARDHANDLE, std::shared_ptr<Card>> cards;
ULONG_PTR next_handle = 1;

template<class Operation>
LONG api(Operation operation) noexcept {
    try {
        const std::lock_guard lock(access);
        return operation();
    } catch (const std::bad_alloc&) {
        return SCARD_E_NO_MEMORY;
    } catch (...) {
        return SCARD_E_READER_UNAVAILABLE;
    }
}

template<class Operation>
LONG unlocked_api(Operation operation) noexcept {
    try { return operation(); }
    catch (const std::bad_alloc&) { return SCARD_E_NO_MEMORY; }
    catch (...) { return SCARD_E_READER_UNAVAILABLE; }
}

template<class Char>
LONG copy_output(Context& context, std::span<const Char> value, Char* output, LPDWORD length) {
    if (!length) return output ? SCARD_E_INVALID_PARAMETER : SCARD_S_SUCCESS;
    const auto capacity = output ? *length : 0;
    *length = static_cast<DWORD>(value.size());
    if (!output) return SCARD_S_SUCCESS;
    if (capacity == SCARD_AUTOALLOCATE) {
        auto memory = std::make_unique<BYTE[]>(value.size_bytes());
        std::memcpy(memory.get(), value.data(), value.size_bytes());
        auto* const pointer = memory.get();
        context.allocations.emplace(pointer, std::move(memory));
        std::memcpy(output, &pointer, sizeof(pointer));
        return SCARD_S_SUCCESS;
    }
    if (capacity < value.size()) return SCARD_E_INSUFFICIENT_BUFFER;
    std::copy(value.begin(), value.end(), output);
    return SCARD_S_SUCCESS;
}

template<class Char, size_t N>
LONG list_readers(SCARDCONTEXT context, const Char* groups, Char* readers, LPDWORD length,
                  const Char (&name)[N]) {
    return unlocked_api([&]() -> LONG {
        if (!length) return SCARD_E_INVALID_PARAMETER;
        { const std::lock_guard lock(access); if (!contexts.contains(context)) return SCARD_E_INVALID_HANDLE; }
        if (groups) return SCARD_E_UNSUPPORTED_FEATURE;
        Connection host(target_sid());
        const auto reply = host.call(Frame(Op::readers));
        if (reply.code) {
            *length = 0;
            return static_cast<LONG>(reply.code);
        }
        const std::lock_guard lock(access);
        const auto found = contexts.find(context);
        if (found == contexts.end()) return SCARD_E_INVALID_HANDLE;
        return copy_output(found->second, std::span<const Char>(name), readers, length);
    });
}

template<class Char>
LONG connect(SCARDCONTEXT context, const Char* reader, DWORD share, DWORD protocols,
             LPSCARDHANDLE handle, LPDWORD protocol, const Char* name) {
    return unlocked_api([&]() -> LONG {
        if (!reader || !handle || !protocol) return SCARD_E_INVALID_PARAMETER;
        std::shared_ptr<std::mutex> connections;
        {
            const std::lock_guard lock(access);
            const auto found = contexts.find(context);
            if (found == contexts.end()) return SCARD_E_INVALID_HANDLE;
            connections = found->second.connections;
        }
        const std::lock_guard connection_lock(*connections);
        { const std::lock_guard lock(access); if (!contexts.contains(context)) return SCARD_E_INVALID_HANDLE; }
        if (std::basic_string_view<Char>(reader) != name) return SCARD_E_UNKNOWN_READER;
        if (share != SCARD_SHARE_SHARED && share != SCARD_SHARE_EXCLUSIVE) return SCARD_E_INVALID_VALUE;
        if (!(protocols & SCARD_PROTOCOL_T1)) return SCARD_E_PROTO_MISMATCH;
        Connection host(target_sid());
        const auto reply = host.call(Frame(Op::open_card, share), 33);
        if (reply.code) return static_cast<LONG>(reply.code);
        const auto card = std::make_shared<Card>(context, std::move(host));
        bool registered = false;
        {
            const std::lock_guard lock(access);
            if (contexts.contains(context)) {
                const auto assigned = next_handle++;
                cards.emplace(assigned, card);
                *handle = assigned;
                *protocol = SCARD_PROTOCOL_T1;
                registered = true;
            }
        }
        if (!registered) {
            try { card->host.call(Frame(Op::disconnect_card, SCARD_LEAVE_CARD)); } catch (...) {}
            return SCARD_E_INVALID_HANDLE;
        }
        return SCARD_S_SUCCESS;
    });
}

template<class Char, size_t N>
LONG status(SCARDHANDLE handle, Char* readers, LPDWORD name_len, LPDWORD state,
            LPDWORD protocol, LPBYTE atr, LPDWORD atr_len, const Char (&name)[N]) {
    return unlocked_api([&]() -> LONG {
        std::shared_ptr<Card> card;
        {
            const std::lock_guard lock(access);
            const auto found = cards.find(handle);
            if (found == cards.end()) return SCARD_E_INVALID_HANDLE;
            card = found->second;
        }
        const std::lock_guard card_lock(card->mutex);
        if (card->closed) return SCARD_E_INVALID_HANDLE;
        const auto reply = card->host.call(Frame(Op::card_status), 33);
        if (reply.code) return static_cast<LONG>(reply.code);
        if (state) *state = SCARD_SPECIFIC;
        if (protocol) *protocol = SCARD_PROTOCOL_T1;

        // 両方の出力が成功してから、自動確保したポインターを呼び出し元へ渡す。
        Context staged;
        BYTE* allocated_atr = nullptr;
        Char* allocated_name = nullptr;
        const bool auto_atr = atr && atr_len && *atr_len == SCARD_AUTOALLOCATE;
        const bool auto_name = readers && name_len && *name_len == SCARD_AUTOALLOCATE;
        DWORD allocated_atr_len = SCARD_AUTOALLOCATE;
        DWORD allocated_name_len = SCARD_AUTOALLOCATE;
        const auto atr_result = copy_output(staged, View(reply.data),
            auto_atr ? reinterpret_cast<LPBYTE>(&allocated_atr) : atr, auto_atr ? &allocated_atr_len : atr_len);
        if (atr_result != SCARD_S_SUCCESS) return atr_result;
        const auto name_result = copy_output(staged, std::span<const Char>(name),
            auto_name ? reinterpret_cast<Char*>(&allocated_name) : readers,
            auto_name ? &allocated_name_len : name_len);
        if (name_result != SCARD_S_SUCCESS) return name_result;
        const std::lock_guard lock(access);
        const auto context = contexts.find(card->context);
        if (context == contexts.end()) return SCARD_E_INVALID_HANDLE;
        context->second.allocations.merge(staged.allocations);
        if (auto_atr) {
            std::memcpy(atr, &allocated_atr, sizeof(allocated_atr));
            *atr_len = allocated_atr_len;
        }
        if (auto_name) {
            std::memcpy(readers, &allocated_name, sizeof(allocated_name));
            *name_len = allocated_name_len;
        }
        return SCARD_S_SUCCESS;
    });
}

}

extern const SCARD_IO_REQUEST g_rgSCardT1Pci{SCARD_PROTOCOL_T1, sizeof(SCARD_IO_REQUEST)};

LONG WINAPI SCardEstablishContext(DWORD scope, LPCVOID reserved1, LPCVOID reserved2, LPSCARDCONTEXT context) {
    return api([&]() -> LONG {
        if (!context || reserved1 || reserved2) return SCARD_E_INVALID_PARAMETER;
        if (scope != SCARD_SCOPE_USER && scope != SCARD_SCOPE_SYSTEM) return SCARD_E_INVALID_VALUE;
        const auto handle = next_handle++;
        contexts.try_emplace(handle);
        *context = handle;
        return SCARD_S_SUCCESS;
    });
}

LONG WINAPI SCardReleaseContext(SCARDCONTEXT context) {
    return unlocked_api([&]() -> LONG {
        std::vector<std::shared_ptr<Card>> closing;
        std::shared_ptr<std::mutex> connections;
        {
            const std::lock_guard lock(access);
            const auto found = contexts.find(context);
            if (found == contexts.end()) return SCARD_E_INVALID_HANDLE;
            connections = found->second.connections;
        }
        const std::lock_guard connection_lock(*connections);
        {
            const std::lock_guard lock(access);
            if (!contexts.contains(context)) return SCARD_E_INVALID_HANDLE;
            for (const auto& [handle, card] : cards) if (card->context == context) closing.push_back(card);
            contexts.erase(context);
            for (auto it = cards.begin(); it != cards.end();) {
                if (it->second->context == context) it = cards.erase(it); else ++it;
            }
        }
        for (const auto& card : closing) {
            const std::lock_guard card_lock(card->mutex);
            if (card->closed) continue;
            card->closed = true;
            try { card->host.call(Frame(Op::disconnect_card, SCARD_LEAVE_CARD)); }
            catch (...) {} // Host が終了していてもローカルのハンドルは解放する。
        }
        return SCARD_S_SUCCESS;
    });
}

LONG WINAPI SCardIsValidContext(SCARDCONTEXT context) {
    return api([&]() -> LONG { return contexts.contains(context) ? SCARD_S_SUCCESS : SCARD_E_INVALID_HANDLE; });
}

LONG WINAPI SCardListReadersA(SCARDCONTEXT context, LPCSTR groups, LPSTR readers, LPDWORD length) {
    return list_readers(context, groups, readers, length, reader_a);
}

LONG WINAPI SCardListReadersW(SCARDCONTEXT context, LPCWSTR groups, LPWSTR readers, LPDWORD length) {
    return list_readers(context, groups, readers, length, reader_w);
}

LONG WINAPI SCardFreeMemory(SCARDCONTEXT context, LPCVOID memory) {
    return api([&]() -> LONG {
        const auto found = contexts.find(context);
        if (found == contexts.end()) return SCARD_E_INVALID_HANDLE;
        return found->second.allocations.erase(memory) ? SCARD_S_SUCCESS : SCARD_E_INVALID_PARAMETER;
    });
}

LONG WINAPI SCardConnectA(SCARDCONTEXT context, LPCSTR reader, DWORD share, DWORD protocols,
                        LPSCARDHANDLE handle, LPDWORD protocol) {
    return connect(context, reader, share, protocols, handle, protocol, reader_a);
}

LONG WINAPI SCardConnectW(SCARDCONTEXT context, LPCWSTR reader, DWORD share, DWORD protocols,
                        LPSCARDHANDLE handle, LPDWORD protocol) {
    return connect(context, reader, share, protocols, handle, protocol, reader_w);
}

LONG WINAPI SCardDisconnect(SCARDHANDLE handle, DWORD disposition) {
    return unlocked_api([&]() -> LONG {
        std::shared_ptr<Card> card;
        {
            const std::lock_guard lock(access);
            const auto found = cards.find(handle);
            if (found == cards.end() || found->second->closing) return SCARD_E_INVALID_HANDLE;
            if (disposition != SCARD_LEAVE_CARD && disposition != SCARD_RESET_CARD) return SCARD_E_INVALID_VALUE;
            card = found->second;
            card->closing = true;
        }
        LONG result = SCARD_E_READER_UNAVAILABLE;
        {
            const std::lock_guard card_lock(card->mutex);
            if (!card->closed) {
                card->closed = true;
                try { result = static_cast<LONG>(card->host.call(Frame(Op::disconnect_card, disposition)).code); }
                catch (...) {}
            } else result = SCARD_E_INVALID_HANDLE;
        }
        {
            const std::lock_guard lock(access);
            cards.erase(handle);
        }
        return result;
    });
}

LONG WINAPI SCardStatusA(SCARDHANDLE handle, LPSTR readers, LPDWORD name_len, LPDWORD state,
                       LPDWORD protocol, LPBYTE atr, LPDWORD atr_len) {
    return status(handle, readers, name_len, state, protocol, atr, atr_len, reader_a);
}

LONG WINAPI SCardStatusW(SCARDHANDLE handle, LPWSTR readers, LPDWORD name_len, LPDWORD state,
                       LPDWORD protocol, LPBYTE atr, LPDWORD atr_len) {
    return status(handle, readers, name_len, state, protocol, atr, atr_len, reader_w);
}

LONG WINAPI SCardTransmit(SCARDHANDLE handle, LPCSCARD_IO_REQUEST send_pci, LPCBYTE send, DWORD send_len,
                         LPSCARD_IO_REQUEST recv_pci, LPBYTE recv, LPDWORD recv_len) {
    try {
        std::shared_ptr<Card> card;
        {
            const std::lock_guard lock(access);
            const auto found = cards.find(handle);
            if (found == cards.end()) return SCARD_E_INVALID_HANDLE;
            card = found->second;
        }
        if (!send_pci || !send || !recv || !recv_len || *recv_len == SCARD_AUTOALLOCATE
            || send_pci->cbPciLength != sizeof(SCARD_IO_REQUEST) || send_len < 4 || send_len > 124)
            return SCARD_E_INVALID_PARAMETER;
        if (send_pci->dwProtocol != SCARD_PROTOCOL_T1) return SCARD_E_PROTO_MISMATCH;
        const std::lock_guard lock(card->mutex);
        if (card->closed) return SCARD_E_INVALID_HANDLE;
        const auto reply = card->host.call(Frame(Op::transmit, 0, 0, View(send, send_len)), 254);
        if (reply.code) return static_cast<LONG>(reply.code);
        const auto capacity = *recv_len;
        *recv_len = static_cast<DWORD>(reply.data.size());
        if (capacity < *recv_len) return SCARD_E_INSUFFICIENT_BUFFER;
        std::copy(reply.data.begin(), reply.data.end(), recv);
        if (recv_pci) *recv_pci = g_rgSCardT1Pci;
        return SCARD_S_SUCCESS;
    } catch (const std::bad_alloc&) {
        return SCARD_E_NO_MEMORY;
    } catch (...) {
        return SCARD_E_READER_UNAVAILABLE;
    }
}

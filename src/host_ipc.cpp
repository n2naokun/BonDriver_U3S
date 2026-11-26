#include "host_ipc.hpp"

#include <aclapi.h>
#include <sddl.h>
#include <wtsapi32.h>

#include <stdexcept>
#include <climits>

namespace u3s::host {
namespace {

constexpr uint32_t version = 1;
constexpr wchar_t system_sid[] = L"S-1-5-18";
constexpr wchar_t local_service_sid[] = L"S-1-5-19";
constexpr wchar_t network_service_sid[] = L"S-1-5-20";
constexpr DWORD client_access = READ_CONTROL | SYNCHRONIZE | FILE_READ_DATA | FILE_WRITE_DATA;
using Local = std::unique_ptr<void, decltype(&LocalFree)>;

constexpr bool is_service_sid(std::wstring_view sid) {
    return sid == system_sid || sid == local_service_sid || sid == network_service_sid;
}
static_assert(is_service_sid(system_sid) && is_service_sid(local_service_sid)
    && is_service_sid(network_service_sid) && !is_service_sid(L"S-1-5-6"));

std::wstring token_sid(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (!size) win_error("トークンのユーザー情報サイズ取得");
    std::vector<uint8_t> data(size);
    if (!GetTokenInformation(token, TokenUser, data.data(), size, &size)) win_error("トークンのユーザー情報取得");
    const auto user = reinterpret_cast<const TOKEN_USER*>(data.data());
    LPWSTR raw = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &raw)) win_error("ユーザー SID の文字列化");
    const Local sid(raw, LocalFree);
    return raw;
}

DWORD transfer(HANDLE pipe, void* buffer, DWORD size, bool write, DWORD timeout) {
    DWORD count = 0;
    if (timeout == INFINITE) {
        const BOOL ok = write ? WriteFile(pipe, buffer, size, &count, nullptr)
                              : ReadFile(pipe, buffer, size, &count, nullptr);
        if (!ok) win_error("Host パイプの転送");
        return count;
    }
    WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) win_error("Host パイプのイベント作成");
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const BOOL ok = write ? WriteFile(pipe, buffer, size, &count, &overlapped)
                          : ReadFile(pipe, buffer, size, &count, &overlapped);
    if (!ok && GetLastError() != ERROR_IO_PENDING) win_error("Host パイプの転送");
    if (!ok) {
        if (WaitForSingleObject(event.get(), timeout) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, &count, TRUE);
            throw std::runtime_error("Host パイプが応答しません");
        }
        if (!GetOverlappedResult(pipe, &overlapped, &count, FALSE)) win_error("Host パイプの転送完了");
    }
    return count;
}

DWORD remaining_timeout(ULONGLONG deadline) {
    if (deadline == ULLONG_MAX) return INFINITE;
    const auto now = GetTickCount64();
    return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
}

ULONGLONG make_deadline(DWORD timeout) {
    return timeout == INFINITE ? ULLONG_MAX : GetTickCount64() + timeout;
}

void read_exact(HANDLE pipe, void* buffer, DWORD size, ULONGLONG deadline) {
    auto* at = static_cast<uint8_t*>(buffer);
    while (size) {
        const auto count = transfer(pipe, at, size, false, remaining_timeout(deadline));
        if (!count) throw std::runtime_error("Host パイプが切断されました");
        at += count;
        size -= count;
    }
}

}

void trace(const char* operation, const char* detail) noexcept {
    try {
        const auto message = std::string("U3S: ") + operation + ": " + detail + "\n";
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.c_str(), -1, nullptr, 0);
        if (count > 0) {
            std::wstring wide(count, L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.c_str(), -1, wide.data(), count)) {
                OutputDebugStringW(wide.c_str());
                return;
            }
        }
        OutputDebugStringA(message.c_str());
    } catch (...) {
        OutputDebugStringW(L"U3S: 診断を出力できませんでした\n");
    }
}

std::wstring current_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) win_error("実行ユーザーのトークン取得");
    const WinHandle token(raw);
    return token_sid(token.get());
}

std::wstring target_sid() {
    auto sid = current_sid();
    if (!is_service_sid(sid)) return sid;

    // サービスの明示的な接続先は、管理者のみ変更できる 64 ビットの HKLM から取得する。
    std::array<wchar_t, 192> setting{};
    DWORD size = sizeof(setting);
    const auto result = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\U3SHost", L"HostSid",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, setting.data(), &size);
    if (result == ERROR_FILE_NOT_FOUND && sid == system_sid) {
        const auto session = WTSGetActiveConsoleSessionId();
        if (session == 0xffffffff) throw std::runtime_error("Host の接続先となるコンソールセッションがありません");
        HANDLE raw = nullptr;
        if (!WTSQueryUserToken(session, &raw)) win_error("コンソールユーザーのトークン取得");
        const WinHandle token(raw);
        return token_sid(token.get());
    }
    if (result != ERROR_SUCCESS) win_error("HKLM\\SOFTWARE\\U3SHost の HostSid を読み取る", result);
    PSID raw = nullptr;
    if (!ConvertStringSidToSidW(setting.data(), &raw)) win_error("HostSid の検証");
    const Local parsed(raw, LocalFree);
    LPWSTR canonical = nullptr;
    if (!ConvertSidToStringSidW(raw, &canonical)) win_error("HostSid の文字列化");
    const Local formatted(canonical, LocalFree);
    return canonical;
}

std::wstring pipe_name(const std::wstring& sid, bool ts) {
    return L"\\\\.\\pipe\\U3SHost." + sid + (ts ? L".ts" : L".control");
}

WinHandle create_listener(const std::wstring& sid, bool ts, bool first) {
    // CreateFile が要求する FILE_READ_ATTRIBUTES を含め、FILE_CREATE_PIPE_INSTANCE は与えない。
    const auto acl = L"O:" + sid + L"D:P(A;;GA;;;" + sid + L")"
        L"(A;;0x120083;;;SY)(A;;0x120083;;;LS)(A;;0x120083;;;NS)"
        L"S:(ML;;NW;;;ME)";
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &raw, nullptr))
        win_error("Host パイプのアクセス権設定");
    const Local descriptor(raw, LocalFree);
    SECURITY_ATTRIBUTES security{sizeof(security), raw, FALSE};
    const auto pipe = CreateNamedPipeW(pipe_name(sid, ts).c_str(),
        PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, ts ? static_cast<DWORD>(ts_chunk_size + 16) : 512, 512, 0, &security);
    if (pipe == INVALID_HANDLE_VALUE) win_error("Host パイプの作成");
    return WinHandle(pipe);
}

Caller authenticate(HANDLE pipe, const std::wstring& host_sid) {
    const auto hello = read_frame(pipe, 0);
    if (hello.code != static_cast<uint32_t>(Op::hello) || hello.a != version || hello.b)
        throw std::runtime_error("Host IPC のプロトコル版が一致しません");
    if (!ImpersonateNamedPipeClient(pipe)) win_error("Host クライアントの認証");
    std::wstring sid;
    try {
        sid = token_sid(GetCurrentThreadToken());
    } catch (...) {
        if (!RevertToSelf()) std::terminate();
        throw;
    }
    if (!RevertToSelf()) std::terminate();
    if (sid != host_sid && !is_service_sid(sid)) throw std::runtime_error("Host クライアントの SID が一致しません");
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) win_error("Host クライアントの PID 取得");
    write_frame(pipe, Frame(0, version));
    return {std::move(sid), pid};
}

void write_frame(HANDLE pipe, const Frame& frame, DWORD timeout) {
    const auto deadline = make_deadline(timeout);
    const std::array<uint32_t, 4> header{frame.code, frame.a, frame.b, static_cast<uint32_t>(frame.data.size())};
    const auto write = [&](const void* data, DWORD size) {
        const auto written = transfer(pipe, const_cast<void*>(data), size, true, remaining_timeout(deadline));
        if (written != size) throw std::runtime_error("Host パイプへの書き込みが途中で止まりました");
    };
    write(header.data(), sizeof(header));
    if (!frame.data.empty()) write(frame.data.data(), header.back());
}

Frame read_frame(HANDLE pipe, size_t limit, DWORD timeout) {
    const auto deadline = make_deadline(timeout);
    std::array<uint32_t, 4> header{};
    read_exact(pipe, header.data(), sizeof(header), deadline);
    if (header.back() > limit) throw std::runtime_error("Host IPC のメッセージ長が範囲外です");
    Frame frame(header[0], header[1], header[2]);
    frame.data.resize(header.back());
    if (!frame.data.empty()) read_exact(pipe, frame.data.data(), header.back(), deadline);
    return frame;
}

Connection::Connection(const std::wstring& sid, bool ts, DWORD timeout) {
    const auto deadline = GetTickCount64() + timeout;
    const auto remaining = [&] { const auto now = GetTickCount64(); return now < deadline ? static_cast<DWORD>(deadline - now) : 0; };
    const auto name = pipe_name(sid, ts);
    auto pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // SYSTEM のトークンを通常ユーザーの Host に委任しない。
        pipe = CreateFileW(name.c_str(), client_access, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) win_error("U3SHost に接続");
        const auto wait = remaining();
        if (!wait || !WaitNamedPipeW(name.c_str(), (std::min)(wait, DWORD{1000}))) win_error("U3SHost に接続");
    }
    if (pipe == INVALID_HANDLE_VALUE) win_error("U3SHost に接続");
    pipe_.reset(pipe);

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    const auto result = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
        &owner, nullptr, nullptr, nullptr, &raw);
    const Local descriptor(raw, LocalFree);
    if (result != ERROR_SUCCESS) win_error("Host パイプ所有者の取得", result);
    PSID expected_raw = nullptr;
    if (!ConvertStringSidToSidW(sid.c_str(), &expected_raw)) win_error("Host SID の検証");
    const Local expected(expected_raw, LocalFree);
    if (!owner || !EqualSid(owner, expected.get())) throw std::runtime_error("Host パイプの所有者が一致しません");

    const auto wait = remaining();
    if (!wait) throw std::runtime_error("Host 接続がタイムアウトしました");
    const auto hello = call(Frame(Op::hello, version), 0, wait);
    if (hello.code || hello.a != version || hello.b) throw std::runtime_error("Host IPC のプロトコル版が一致しません");
}

Frame Connection::call(const Frame& request, size_t limit, DWORD timeout) {
    if (!timeout) {
        const auto op = static_cast<Op>(request.code);
        timeout = op == Op::hello || op == Op::read_ts ? 3000 : 30000;
    }
    if (!pipe_) throw std::runtime_error("U3SHost に未接続です");
    try {
        const auto deadline = make_deadline(timeout);
        write_frame(pipe_.get(), request, remaining_timeout(deadline));
        return read_frame(pipe_.get(), limit, remaining_timeout(deadline));
    } catch (...) {
        pipe_.reset();
        throw;
    }
}

bool Connection::alive() const noexcept {
    return pipe_ && PeekNamedPipe(pipe_.get(), nullptr, 0, nullptr, nullptr, nullptr);
}

}

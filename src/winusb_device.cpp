#include "winusb_device.hpp"

#include <cfgmgr32.h>
#include <objbase.h>
#include <setupapi.h>
#include <usb.h>

#include <algorithm>
#include <cwchar>
#include <stdexcept>

namespace u3s {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr UCHAR endpoint = 0x84;
constexpr size_t transfer_count = 32;

struct DestroyDeviceList {
    void operator()(HDEVINFO list) const noexcept { SetupDiDestroyDeviceInfoList(list); }
};
using DeviceList = std::unique_ptr<void, DestroyDeviceList>;

void append_interfaces(const GUID& guid, const wchar_t* instance, std::vector<std::wstring>& devices) {
    HDEVINFO raw = SetupDiGetClassDevsW(&guid, instance, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (raw == INVALID_HANDLE_VALUE) {
        win_error("WinUSB インターフェースの取得");
    }
    DeviceList list(raw);
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA entry{};
        entry.cbSize = sizeof(entry);
        if (!SetupDiEnumDeviceInterfaces(list.get(), nullptr, &guid, i, &entry)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) return;
            win_error("WinUSB インターフェースの列挙");
        }
        DWORD size = 0;
        if (!SetupDiGetDeviceInterfaceDetailW(list.get(), &entry, nullptr, 0, &size, nullptr)
            && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            win_error("WinUSB パスのサイズ取得");
        }
        std::vector<uint8_t> storage(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(list.get(), &entry, detail, size, nullptr, nullptr)) {
            win_error("WinUSB パスの取得");
        }
        if (std::ranges::find(devices, detail->DevicePath) == devices.end()) {
            devices.emplace_back(detail->DevicePath);
        }
    }
}

}

[[noreturn]] void win_error(const char* operation, DWORD code) {
    throw std::runtime_error(std::string(operation) + " (Win32 " + std::to_string(code) + ")");
}

std::vector<std::wstring> list_winusb() {
    const HDEVINFO raw = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (raw == INVALID_HANDLE_VALUE) {
        win_error("USB デバイス一覧の取得");
    }
    DeviceList devices(raw);
    std::vector<std::wstring> interfaces;
    constexpr wchar_t prefix[] = L"USB\\VID_3275&PID_9015\\";
    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices.get(), i, &device)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            win_error("USB デバイスの列挙");
        }
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(devices.get(), &device, instance, MAX_DEVICE_ID_LEN, nullptr)
            || _wcsnicmp(instance, prefix, std::size(prefix) - 1) != 0) {
            continue;
        }
        wchar_t service[32]{};
        if (!SetupDiGetDeviceRegistryPropertyW(devices.get(), &device, SPDRP_SERVICE, nullptr,
                reinterpret_cast<BYTE*>(service), sizeof(service), nullptr)
            || _wcsicmp(service, L"WinUSB") != 0) {
            continue;
        }
        const auto key = SetupDiOpenDevRegKey(devices.get(), &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE) {
            win_error("WinUSB のデバイス設定を開く");
        }
        const std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> registered(key, RegCloseKey);
        // WinUSB の割り当て時に登録された GUID を使う。機器固有の INF に固定しない。
        for (const wchar_t* name : {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"}) {
            constexpr auto flags = RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ;
            DWORD bytes = 0;
            const auto result = RegGetValueW(key, nullptr, name, flags, nullptr, nullptr, &bytes);
            if (result == ERROR_FILE_NOT_FOUND) continue;
            if (result != ERROR_SUCCESS) win_error("WinUSB のインターフェース GUID の取得", result);
            std::vector<wchar_t> values(bytes / sizeof(wchar_t) + 2);
            bytes = static_cast<DWORD>(values.size() * sizeof(wchar_t));
            if (const auto read = RegGetValueW(key, nullptr, name, flags, nullptr, values.data(), &bytes);
                read != ERROR_SUCCESS) {
                win_error("WinUSB のインターフェース GUID の読み込み", read);
            }
            for (auto* value = values.data(); *value; value += std::wcslen(value) + 1) {
                GUID guid{};
                if (SUCCEEDED(CLSIDFromString(value, &guid))) {
                    append_interfaces(guid, instance, interfaces);
                }
            }
        }
    }
    return interfaces;
}

WinUsb::WinUsb(const std::wstring& path) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) win_error("SK-MTVU3S を開く");
    file_.reset(file);
    WINUSB_INTERFACE_HANDLE handle = nullptr;
    if (!WinUsb_Initialize(file_.get(), &handle)) win_error("WinUsb_Initialize");
    interface_.reset(handle);

    USB_DEVICE_DESCRIPTOR descriptor{};
    ULONG count = 0;
    if (!WinUsb_GetDescriptor(handle, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
            reinterpret_cast<PUCHAR>(&descriptor), sizeof(descriptor), &count)) {
        win_error("USB デバイス記述子の取得");
    }
    if (count != sizeof(descriptor) || descriptor.idVendor != 0x3275 || descriptor.idProduct != 0x9015) {
        throw std::runtime_error("USB デバイス記述子が SK-MTVU3S (3275:9015) と一致しません");
    }
    USB_INTERFACE_DESCRIPTOR setting{};
    if (!WinUsb_QueryInterfaceSettings(handle, 0, &setting)) win_error("USB インターフェース記述子の取得");
    if (setting.bInterfaceNumber != 0) throw std::runtime_error("USB インターフェース 0 を開けません");

    ULONG timeout = 1000;
    if (!WinUsb_SetPipePolicy(handle, 0, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout)) {
        win_error("制御転送のタイムアウト設定");
    }
}

void WinUsb::transfer(uint8_t type, uint8_t request, uint16_t index, std::span<uint8_t> data) {
    const WINUSB_SETUP_PACKET packet{type, request, 0, index, static_cast<USHORT>(data.size())};
    ULONG count = 0;
    if (!WinUsb_ControlTransfer(interface_.get(), packet, data.data(), packet.Length, &count, nullptr)) {
        const auto error = GetLastError();
        const auto operation = "制御転送 req=" + std::to_string(request) + " index=" + std::to_string(index);
        win_error(operation.c_str(), error);
    }
    if (count != data.size()) throw std::runtime_error("制御転送の長さが一致しません");
}

void WinUsb::alt(uint8_t setting) {
    if (!WinUsb_SetCurrentAlternateSetting(interface_.get(), setting)) win_error("USB 代替設定の変更");
}

struct IsoStream::Slot {
    Slot(WINUSB_INTERFACE_HANDLE usb, ULONG size, ULONG packets) : data(size), descriptors(packets) {
        event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) win_error("等時性転送の完了イベント作成");
        if (!WinUsb_RegisterIsochBuffer(usb, endpoint, data.data(), size, &registered)) {
            win_error("WinUsb_RegisterIsochBuffer");
        }
    }

    ~Slot() { WinUsb_UnregisterIsochBuffer(registered); }

    DWORD submit() {
        overlapped = {};
        overlapped.hEvent = event.get();
        if (!ResetEvent(event.get())) win_error("等時性転送の完了イベント初期化");
        std::fill(descriptors.begin(), descriptors.end(), USBD_ISO_PACKET_DESCRIPTOR{});
        // 保留中の転送の直後に予約される。Silex 経由では連続を必須にすると再投入が失敗する。
        if (WinUsb_ReadIsochPipeAsap(registered, 0, static_cast<ULONG>(data.size()), FALSE,
                static_cast<ULONG>(descriptors.size()), descriptors.data(), &overlapped)) {
            return ERROR_SUCCESS;
        }
        const auto error = GetLastError();
        pending = error == ERROR_IO_PENDING;
        return pending ? ERROR_SUCCESS : error;
    }

    WinHandle event;
    std::vector<uint8_t> data;
    std::vector<USBD_ISO_PACKET_DESCRIPTOR> descriptors;
    WINUSB_ISOCH_BUFFER_HANDLE registered = nullptr;
    OVERLAPPED overlapped{};
    bool pending = false;
};

IsoStream::IsoStream(WinUsb& usb) : usb_(usb) {
    const auto handle = usb_.interface_.get();
    UCHAR speed = 0;
    ULONG size = sizeof(speed);
    if (!WinUsb_QueryDeviceInformation(handle, DEVICE_SPEED, &size, &speed)) win_error("USB 転送速度の取得");
    if (speed != HighSpeed) throw std::runtime_error("SK-MTVU3S の等時性受信には USB High-Speed 接続が必要です");

    USB_INTERFACE_DESCRIPTOR setting{};
    if (!WinUsb_QueryInterfaceSettings(handle, 1, &setting)) win_error("代替設定 1 の記述子の取得");
    for (UCHAR i = 0; i < setting.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION_EX pipe{};
        if (!WinUsb_QueryPipeEx(handle, 1, i, &pipe)) win_error("WinUsb_QueryPipeEx");
        if (pipe.PipeId != endpoint || pipe.PipeType != UsbdPipeTypeIsochronous) continue;
        if (!pipe.MaximumBytesPerInterval || pipe.Interval < 1 || pipe.Interval > 4) {
            throw std::runtime_error("EP 0x84 の等時性転送間隔または最大データ長が不正です");
        }
        bytes_per_interval_ = pipe.MaximumBytesPerInterval;
        // High-Speed の bInterval は 2^(bInterval-1) マイクロフレーム。要求を 2 ms に揃える。
        const ULONG packets = 16u >> (pipe.Interval - 1);
        for (size_t slot = 0; slot < transfer_count; ++slot) {
            slots_.push_back(std::make_unique<Slot>(handle, packets * bytes_per_interval_, packets));
        }
        return;
    }
    throw std::runtime_error("代替設定 1 に等時性 IN EP 0x84 がありません");
}

IsoStream::~IsoStream() { cancel_and_drain(); }

DWORD IsoStream::prime() {
    for (const auto& slot : slots_) {
        if (const auto error = slot->submit(); error != ERROR_SUCCESS) return error;
    }
    return ERROR_SUCCESS;
}

void IsoStream::cancel_and_drain() noexcept {
    // I/O が完了するまで OVERLAPPED と登録済みバッファを再利用しない。
    if (std::none_of(slots_.begin(), slots_.end(), [](const auto& slot) { return slot->pending; })) return;
    for (const auto& slot : slots_) {
        if (slot->pending) CancelIoEx(usb_.file_.get(), &slot->overlapped);
    }
    for (const auto& slot : slots_) {
        if (slot->pending) {
            ULONG ignored = 0;
            WinUsb_GetOverlappedResult(usb_.interface_.get(), &slot->overlapped, &ignored, TRUE);
            slot->pending = false;
        }
    }
}

IsoBlock IsoStream::collect(const Slot& slot) {
    IsoBlock block;
    block.data.reserve(slot.data.size());
    for (const auto& desc : slot.descriptors) {
        if (desc.Status != USBD_STATUS_SUCCESS || desc.Offset > slot.data.size()
            || desc.Length > slot.data.size() - desc.Offset || desc.Length > bytes_per_interval_) {
            ++stats_.failed_packets;
            if (block.gaps.empty() || block.gaps.back() != block.data.size()) block.gaps.push_back(block.data.size());
        } else if (desc.Length != 0) {
            const auto* first = slot.data.data() + desc.Offset;
            block.data.insert(block.data.end(), first, first + desc.Length);
        }
    }
    return block;
}

void IsoStream::read(std::chrono::seconds duration, const std::atomic_bool& stop,
                     const std::function<void(IsoBlock)>& sink) {
    auto error = prime();
    const auto end = Clock::now() + duration;
    size_t index = 0;
    size_t consecutive = slots_.size();
    while (!stop && Clock::now() < end) {
        if (error != ERROR_SUCCESS) {
            if (consecutive < slots_.size() || error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_OPERATION_ABORTED) {
                win_error("等時性転送", error);
            }
            cancel_and_drain();
            ++stats_.restarts;
            IsoBlock gap;
            gap.missing_before = true;
            sink(std::move(gap));
            consecutive = 0;
            index = 0;
            error = prime();
            continue;
        }

        auto& slot = *slots_[index];
        if (slot.pending) {
            const auto stall = Clock::now() + 3s;
            while (!stop && Clock::now() < end) {
                const auto ready = WaitForSingleObject(slot.event.get(), 20);
                if (ready == WAIT_OBJECT_0) break;
                if (ready == WAIT_FAILED) win_error("等時性転送の完了待ち");
                if (Clock::now() >= stall) throw std::runtime_error("等時性転送が 3 秒以内に完了しませんでした");
            }
            if (stop || Clock::now() >= end) break;
            ULONG ignored = 0;
            if (!WinUsb_GetOverlappedResult(usb_.interface_.get(), &slot.overlapped, &ignored, FALSE)) {
                error = GetLastError();
                slot.pending = error == ERROR_IO_INCOMPLETE;
                if (!slot.pending) stats_.failed_packets += slot.descriptors.size();
                continue;
            }
            slot.pending = false;
        }

        auto block = collect(slot);
        error = slot.submit();
        if (!block.data.empty() || !block.gaps.empty()) sink(std::move(block));
        if (error == ERROR_SUCCESS && consecutive < slots_.size()) ++consecutive;
        index = (index + 1) % slots_.size();
    }
    cancel_and_drain();
}

}

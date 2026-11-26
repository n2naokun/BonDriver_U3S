#pragma once

#include <windows.h>

// TVTest の LibISDB BonDriver.hpp に合わせ、宣言順と型を維持する。仮想デストラクターは ABI にない。
// https://github.com/DBCTRADO/LibISDB/blob/605649deec7466f8fda73780b54231a8b0494202/LibISDB/Windows/Base/BonDriver.hpp
class IBonDriver {
public:
    virtual const BOOL OpenTuner() = 0;
    virtual void CloseTuner() = 0;
    virtual const BOOL SetChannel(const BYTE channel) = 0;
    virtual const float GetSignalLevel() = 0;
    virtual const DWORD WaitTsStream(const DWORD timeout = 0) = 0;
    virtual const DWORD GetReadyCount() = 0;
    virtual const BOOL GetTsStream(BYTE* data, DWORD* size, DWORD* remain) = 0;
    virtual const BOOL GetTsStream(BYTE** data, DWORD* size, DWORD* remain) = 0;
    virtual void PurgeTsStream() = 0;
    virtual void Release() = 0;
};

class IBonDriver2 : public IBonDriver {
public:
    virtual const wchar_t* GetTunerName() = 0;
    virtual const BOOL IsTunerOpening() = 0;
    virtual const wchar_t* const EnumTuningSpace(const DWORD space) = 0;
    virtual const wchar_t* const EnumChannelName(const DWORD space, const DWORD channel) = 0;
    virtual const BOOL SetChannel(const DWORD space, const DWORD channel) = 0;
    virtual const DWORD GetCurSpace() = 0;
    virtual const DWORD GetCurChannel() = 0;
    virtual void Release() override = 0;
};

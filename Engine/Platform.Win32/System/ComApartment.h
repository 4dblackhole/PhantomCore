#pragma once

#include <Windows.h>
#include <objbase.h>


// RAII wrapper for one COM apartment on the calling thread.
namespace PhantomCore
{
class ComApartment final
{
public:
    // Require every call site to choose an apartment deliberately. Some
    // COM-backed APIs, including FMOD ASIO enumeration, require an STA.
    explicit ComApartment(DWORD concurrencyModel);
    ~ComApartment();

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT result_{};
};

} // namespace PhantomCore

#include "System/ComApartment.h"

#include <stdexcept>


namespace PhantomCore
{
ComApartment::ComApartment(const DWORD concurrencyModel)
{
    result_ = CoInitializeEx(nullptr, concurrencyModel);
    if (result_ == RPC_E_CHANGED_MODE)
    {
        throw std::runtime_error(
            "The current thread already uses an incompatible COM apartment model.");
    }
    if (FAILED(result_))
    {
        throw std::runtime_error("CoInitializeEx failed.");
    }
}

ComApartment::~ComApartment()
{
    if (SUCCEEDED(result_))
    {
        CoUninitialize();
    }
}

} // namespace PhantomCore

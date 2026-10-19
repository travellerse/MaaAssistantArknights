#pragma once

#include <dlfcn.h>

#include <initializer_list>

namespace asst::waydroid
{
class DynLib
{
public:
    DynLib() = default;

    DynLib(std::initializer_list<const char*> names)
    {
        for (const char* name : names) {
            m_handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (m_handle != nullptr) {
                break;
            }
        }
    }

    ~DynLib()
    {
        if (m_handle != nullptr) {
            dlclose(m_handle);
        }
    }

    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;

    DynLib(DynLib&& other) noexcept :
        m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }

    DynLib& operator=(DynLib&& other) noexcept
    {
        if (this != &other) {
            if (m_handle != nullptr) {
                dlclose(m_handle);
            }
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    explicit operator bool() const noexcept { return m_handle != nullptr; }

    template <typename Func>
    Func load(const char* name) const
    {
        return reinterpret_cast<Func>(dlsym(m_handle, name));
    }

private:
    void* m_handle = nullptr;
};
} // namespace asst::waydroid

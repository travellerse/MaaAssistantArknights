#pragma once

// Minimal libdbus-1 ABI subset. Loaded with dlopen so no build dependency is
// added. Layouts are copied from dbus 1.x public headers; they are frozen ABI.

#include <cstdint>

namespace asst::waydroid::dbus
{
struct Connection;
struct Message;

struct Error
{
    const char* name;
    const char* message;
    unsigned int padding: 1;
    void* padding1;
};

struct MessageIter
{
    void* dummy1;
    void* dummy2;
    uint32_t dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    void* pad2;
    void* pad3;
};

using MessageHandler = int (*)(Connection*, Message*, void*);

struct ObjectPathVTable
{
    void (*unregister_function)(Connection*, void*);
    int (*message_function)(Connection*, Message*, void*);
    void (*dbus_internal_pad1)(void*);
    void (*dbus_internal_pad2)(void*);
    void (*dbus_internal_pad3)(void*);
    void (*dbus_internal_pad4)(void*);
};

inline constexpr int kBusSession = 0;
inline constexpr int kNameFlagDoNotQueue = 0x4;

inline constexpr int kTypeInvalid = 0;
inline constexpr int kTypeBoolean = 'b';
inline constexpr int kTypeInt32 = 'i';
inline constexpr int kTypeUint32 = 'u';
inline constexpr int kTypeInt64 = 'x';
inline constexpr int kTypeUint64 = 't';
inline constexpr int kTypeString = 's';
inline constexpr int kTypeObjectPath = 'o';
inline constexpr int kTypeUnixFd = 'h';
inline constexpr int kTypeArray = 'a';
inline constexpr int kTypeVariant = 'v';
inline constexpr int kTypeStruct = 'r';
inline constexpr int kTypeDictEntry = 'e';

inline constexpr int kMessageTypeMethodCall = 1;
inline constexpr int kMessageTypeMethodReturn = 2;
inline constexpr int kMessageTypeError = 3;
inline constexpr int kMessageTypeSignal = 4;

inline constexpr int kHandlerResultHandled = 0;
inline constexpr int kHandlerResultNotYetHandled = 1;
} // namespace asst::waydroid::dbus

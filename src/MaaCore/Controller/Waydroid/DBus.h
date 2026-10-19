#pragma once

#include "DBusABI.h"
#include "DynLib.h"

#include <optional>
#include <string>
#include <string_view>

namespace asst::waydroid
{
class DBusConnection
{
public:
    static std::optional<DBusConnection> open_session(std::string& error);

    DBusConnection() = default;
    ~DBusConnection();

    DBusConnection(const DBusConnection&) = delete;
    DBusConnection& operator=(const DBusConnection&) = delete;

    DBusConnection(DBusConnection&& other) noexcept;
    DBusConnection& operator=(DBusConnection&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return m_connection != nullptr; }

    dbus::Message*
        new_method_call(const char* destination, const char* path, const char* interface, const char* method) const;

    // Blocks for at most timeout_ms. Returns nullptr and fills error on failure.
    dbus::Message* call(dbus::Message* message, int timeout_ms, std::string& error) const;

    bool add_match(std::string_view rule, std::string& error) const;
    bool add_filter(dbus::MessageHandler handler, void* user_data) const;
    bool register_object_path(std::string_view path, const dbus::ObjectPathVTable* vtable, void* user_data) const;
    bool request_name(std::string_view name, std::string& error) const;
    [[nodiscard]] bool read_write_dispatch(int timeout_ms) const;
    [[nodiscard]] const char* unique_name() const;

private:
    explicit DBusConnection(dbus::Connection* connection);

    void close() noexcept;

    dbus::Connection* m_connection = nullptr;
};

// Iterator helpers shared by the KWin and portal backends.
namespace dbus_util
{
dbus::MessageIter open_container(dbus::MessageIter& parent, int type, const char* contents);
void close_container(dbus::MessageIter& parent, dbus::MessageIter& child);

bool init_append(dbus::Message* message, dbus::MessageIter& iter);
bool init_read(dbus::Message* message, dbus::MessageIter& iter);
dbus::MessageIter recurse(dbus::MessageIter& iter);
bool next(dbus::MessageIter& iter);
int arg_type(dbus::MessageIter& iter);

void append_string(dbus::MessageIter& iter, std::string_view value);
void append_object_path(dbus::MessageIter& iter, std::string_view value);
void append_uint32(dbus::MessageIter& iter, uint32_t value);
void append_boolean(dbus::MessageIter& iter, bool value);
void append_fd(dbus::MessageIter& iter, int fd);

bool get_string(dbus::MessageIter& iter, std::string& value);
bool get_uint32(dbus::MessageIter& iter, uint32_t& value);
bool get_int32(dbus::MessageIter& iter, int32_t& value);
bool get_boolean(dbus::MessageIter& iter, bool& value);
bool get_fd(dbus::MessageIter& iter, int& value);

std::optional<std::string> read_string_argument(dbus::Message* message);
dbus::Message* new_method_return(dbus::Message* call);
bool send_reply(dbus::Connection* connection, dbus::Message* reply);
void message_unref(dbus::Message* message);
const char* message_path(dbus::Message* message);
bool is_signal(dbus::Message* message, const char* interface, const char* member);

void skip_value(dbus::MessageIter& iter);

struct Variant
{
    int type = dbus::kTypeInvalid;
    std::string string_value;
    uint32_t uint32_value = 0;
    int64_t int64_value = 0;
    int fd = -1;
};

Variant read_variant(dbus::MessageIter& variant_iter);
bool read_dict_entry_sv(dbus::MessageIter& entry, std::string& key, Variant& value);
} // namespace dbus_util
} // namespace asst::waydroid

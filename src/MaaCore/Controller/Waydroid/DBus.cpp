#include "DBus.h"

#include "Utils/Logger.hpp"

#include <array>
#include <cstring>

namespace asst::waydroid
{
namespace
{
struct Api
{
    DynLib lib { { "libdbus-1.so.3", "libdbus-1.so" } };

    dbus::Connection* (*bus_get_private)(int, dbus::Error*) = nullptr;
    void (*connection_close)(dbus::Connection*) = nullptr;
    void (*connection_unref)(dbus::Connection*) = nullptr;
    void (*connection_set_exit_on_disconnect)(dbus::Connection*, int) = nullptr;
    int (*connection_send)(dbus::Connection*, dbus::Message*, uint32_t*) = nullptr;
    dbus::Message* (*connection_send_with_reply_and_block)(dbus::Connection*, dbus::Message*, int, dbus::Error*) =
        nullptr;
    int (*connection_add_filter)(dbus::Connection*, dbus::MessageHandler, void*, void (*)(void*)) = nullptr;
    int (*connection_read_write_dispatch)(dbus::Connection*, int) = nullptr;
    int (*connection_register_object_path)(dbus::Connection*, const char*, const dbus::ObjectPathVTable*, void*) =
        nullptr;
    void (*connection_flush)(dbus::Connection*) = nullptr;
    void (*bus_add_match)(dbus::Connection*, const char*, dbus::Error*) = nullptr;
    int (*bus_request_name)(dbus::Connection*, const char*, unsigned int, dbus::Error*) = nullptr;
    const char* (*bus_get_unique_name)(dbus::Connection*) = nullptr;
    void (*error_init)(dbus::Error*) = nullptr;
    void (*error_free)(dbus::Error*) = nullptr;
    int (*error_is_set)(const dbus::Error*) = nullptr;
    dbus::Message* (*message_new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
    dbus::Message* (*message_new_method_return)(dbus::Message*) = nullptr;
    void (*message_unref)(dbus::Message*) = nullptr;
    int (*message_get_type)(dbus::Message*) = nullptr;
    const char* (*message_get_path)(dbus::Message*) = nullptr;
    const char* (*message_get_interface)(dbus::Message*) = nullptr;
    const char* (*message_get_member)(dbus::Message*) = nullptr;
    int (*message_iter_init_append)(dbus::Message*, dbus::MessageIter*) = nullptr;
    int (*message_iter_init)(dbus::Message*, dbus::MessageIter*) = nullptr;
    int (*message_iter_append_basic)(dbus::MessageIter*, int, const void*) = nullptr;
    int (*message_iter_open_container)(dbus::MessageIter*, int, const char*, dbus::MessageIter*) = nullptr;
    int (*message_iter_close_container)(dbus::MessageIter*, dbus::MessageIter*) = nullptr;
    int (*message_iter_get_arg_type)(dbus::MessageIter*) = nullptr;
    void (*message_iter_get_basic)(dbus::MessageIter*, void*) = nullptr;
    void (*message_iter_recurse)(dbus::MessageIter*, dbus::MessageIter*) = nullptr;
    int (*message_iter_next)(dbus::MessageIter*) = nullptr;

    Api()
    {
        if (!lib) {
            return;
        }
        bus_get_private = lib.load<decltype(bus_get_private)>("dbus_bus_get_private");
        connection_close = lib.load<decltype(connection_close)>("dbus_connection_close");
        connection_unref = lib.load<decltype(connection_unref)>("dbus_connection_unref");
        connection_set_exit_on_disconnect =
            lib.load<decltype(connection_set_exit_on_disconnect)>("dbus_connection_set_exit_on_disconnect");
        connection_send = lib.load<decltype(connection_send)>("dbus_connection_send");
        connection_send_with_reply_and_block =
            lib.load<decltype(connection_send_with_reply_and_block)>("dbus_connection_send_with_reply_and_block");
        connection_add_filter = lib.load<decltype(connection_add_filter)>("dbus_connection_add_filter");
        connection_read_write_dispatch =
            lib.load<decltype(connection_read_write_dispatch)>("dbus_connection_read_write_dispatch");
        connection_register_object_path =
            lib.load<decltype(connection_register_object_path)>("dbus_connection_register_object_path");
        connection_flush = lib.load<decltype(connection_flush)>("dbus_connection_flush");
        bus_add_match = lib.load<decltype(bus_add_match)>("dbus_bus_add_match");
        bus_request_name = lib.load<decltype(bus_request_name)>("dbus_bus_request_name");
        bus_get_unique_name = lib.load<decltype(bus_get_unique_name)>("dbus_bus_get_unique_name");
        error_init = lib.load<decltype(error_init)>("dbus_error_init");
        error_free = lib.load<decltype(error_free)>("dbus_error_free");
        error_is_set = lib.load<decltype(error_is_set)>("dbus_error_is_set");
        message_new_method_call = lib.load<decltype(message_new_method_call)>("dbus_message_new_method_call");
        message_new_method_return = lib.load<decltype(message_new_method_return)>("dbus_message_new_method_return");
        message_unref = lib.load<decltype(message_unref)>("dbus_message_unref");
        message_get_type = lib.load<decltype(message_get_type)>("dbus_message_get_type");
        message_get_path = lib.load<decltype(message_get_path)>("dbus_message_get_path");
        message_get_interface = lib.load<decltype(message_get_interface)>("dbus_message_get_interface");
        message_get_member = lib.load<decltype(message_get_member)>("dbus_message_get_member");
        message_iter_init_append = lib.load<decltype(message_iter_init_append)>("dbus_message_iter_init_append");
        message_iter_init = lib.load<decltype(message_iter_init)>("dbus_message_iter_init");
        message_iter_append_basic = lib.load<decltype(message_iter_append_basic)>("dbus_message_iter_append_basic");
        message_iter_open_container =
            lib.load<decltype(message_iter_open_container)>("dbus_message_iter_open_container");
        message_iter_close_container =
            lib.load<decltype(message_iter_close_container)>("dbus_message_iter_close_container");
        message_iter_get_arg_type = lib.load<decltype(message_iter_get_arg_type)>("dbus_message_iter_get_arg_type");
        message_iter_get_basic = lib.load<decltype(message_iter_get_basic)>("dbus_message_iter_get_basic");
        message_iter_recurse = lib.load<decltype(message_iter_recurse)>("dbus_message_iter_recurse");
        message_iter_next = lib.load<decltype(message_iter_next)>("dbus_message_iter_next");
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return bus_get_private != nullptr && connection_send_with_reply_and_block != nullptr &&
               message_new_method_call != nullptr;
    }
};

const Api& api()
{
    static const Api instance;
    return instance;
}

std::string error_text(const dbus::Error& error)
{
    if (error.name != nullptr && error.message != nullptr) {
        return std::string(error.name) + ": " + error.message;
    }
    if (error.message != nullptr) {
        return error.message;
    }
    if (error.name != nullptr) {
        return error.name;
    }
    return "unknown dbus error";
}
} // namespace

DBusConnection::DBusConnection(dbus::Connection* connection) :
    m_connection(connection)
{
}

DBusConnection::~DBusConnection()
{
    close();
}

DBusConnection::DBusConnection(DBusConnection&& other) noexcept :
    m_connection(other.m_connection)
{
    other.m_connection = nullptr;
}

DBusConnection& DBusConnection::operator=(DBusConnection&& other) noexcept
{
    if (this != &other) {
        close();
        m_connection = other.m_connection;
        other.m_connection = nullptr;
    }
    return *this;
}

void DBusConnection::close() noexcept
{
    if (m_connection != nullptr && api().valid()) {
        api().connection_close(m_connection);
        api().connection_unref(m_connection);
        m_connection = nullptr;
    }
}

std::optional<DBusConnection> DBusConnection::open_session(std::string& error)
{
    if (!api().valid()) {
        error = "libdbus-1 is not available";
        return std::nullopt;
    }

    dbus::Error dbus_error {};
    api().error_init(&dbus_error);
    dbus::Connection* connection = api().bus_get_private(dbus::kBusSession, &dbus_error);
    if (connection == nullptr) {
        error = error_text(dbus_error);
        api().error_free(&dbus_error);
        return std::nullopt;
    }
    api().connection_set_exit_on_disconnect(connection, 0);
    return DBusConnection(connection);
}

dbus::Message* DBusConnection::new_method_call(
    const char* destination,
    const char* path,
    const char* interface,
    const char* method) const
{
    if (m_connection == nullptr || !api().valid()) {
        return nullptr;
    }
    return api().message_new_method_call(destination, path, interface, method);
}

dbus::Message* DBusConnection::call(dbus::Message* message, int timeout_ms, std::string& error) const
{
    if (m_connection == nullptr || message == nullptr || !api().valid()) {
        error = "invalid connection or message";
        return nullptr;
    }

    dbus::Error dbus_error {};
    api().error_init(&dbus_error);
    dbus::Message* reply = api().connection_send_with_reply_and_block(m_connection, message, timeout_ms, &dbus_error);
    if (reply == nullptr) {
        error = error_text(dbus_error);
    }
    api().error_free(&dbus_error);
    return reply;
}

bool DBusConnection::add_match(std::string_view rule, std::string& error) const
{
    if (m_connection == nullptr || !api().valid()) {
        error = "not connected";
        return false;
    }
    dbus::Error dbus_error {};
    api().error_init(&dbus_error);
    const std::string rule_str(rule);
    api().bus_add_match(m_connection, rule_str.c_str(), &dbus_error);
    if (api().error_is_set(&dbus_error) != 0) {
        LogWarn << "dbus_bus_add_match failed" << VAR(rule_str) << VAR(dbus_error.name) << VAR(dbus_error.message);
        error = error_text(dbus_error);
        api().error_free(&dbus_error);
        return false;
    }
    api().error_free(&dbus_error);
    return true;
}

bool DBusConnection::add_filter(dbus::MessageHandler handler, void* user_data) const
{
    return m_connection != nullptr && api().valid() &&
           api().connection_add_filter(m_connection, handler, user_data, nullptr) != 0;
}

bool DBusConnection::register_object_path(std::string_view path, const dbus::ObjectPathVTable* vtable, void* user_data)
    const
{
    if (m_connection == nullptr || !api().valid()) {
        return false;
    }
    const std::string path_str(path);
    return api().connection_register_object_path(m_connection, path_str.c_str(), vtable, user_data) != 0;
}

bool DBusConnection::request_name(std::string_view name, std::string& error) const
{
    if (m_connection == nullptr || !api().valid()) {
        error = "not connected";
        return false;
    }
    dbus::Error dbus_error {};
    api().error_init(&dbus_error);
    const std::string name_str(name);
    const int result = api().bus_request_name(m_connection, name_str.c_str(), dbus::kNameFlagDoNotQueue, &dbus_error);
    if (result != 1 && result != 4) {
        error = result < 0 ? error_text(dbus_error) : "name already owned";
        api().error_free(&dbus_error);
        return false;
    }
    api().error_free(&dbus_error);
    return true;
}

bool DBusConnection::read_write_dispatch(int timeout_ms) const
{
    return m_connection != nullptr && api().valid() &&
           api().connection_read_write_dispatch(m_connection, timeout_ms) != 0;
}

const char* DBusConnection::unique_name() const
{
    return m_connection != nullptr && api().valid() ? api().bus_get_unique_name(m_connection) : nullptr;
}

namespace dbus_util
{
dbus::MessageIter open_container(dbus::MessageIter& parent, int type, const char* contents)
{
    dbus::MessageIter child {};
    if (api().valid() && api().message_iter_open_container(&parent, type, contents, &child) == 0) {
        LogWarn << __FUNCTION__ << "open_container failed" << VAR(type) << VAR(contents);
    }
    return child;
}

void close_container(dbus::MessageIter& parent, dbus::MessageIter& child)
{
    if (api().valid() && api().message_iter_close_container(&parent, &child) == 0) {
        LogWarn << __FUNCTION__ << "close_container failed";
    }
}

bool init_append(dbus::Message* message, dbus::MessageIter& iter)
{
    return api().valid() && api().message_iter_init_append(message, &iter) != 0;
}

bool init_read(dbus::Message* message, dbus::MessageIter& iter)
{
    return api().valid() && api().message_iter_init(message, &iter) != 0;
}

dbus::MessageIter recurse(dbus::MessageIter& iter)
{
    dbus::MessageIter child {};
    if (api().valid()) {
        api().message_iter_recurse(&iter, &child);
    }
    return child;
}

bool next(dbus::MessageIter& iter)
{
    return api().valid() && api().message_iter_next(&iter) != 0;
}

int arg_type(dbus::MessageIter& iter)
{
    return api().valid() ? api().message_iter_get_arg_type(&iter) : dbus::kTypeInvalid;
}

void append_string(dbus::MessageIter& iter, std::string_view value)
{
    if (!api().valid()) {
        return;
    }
    const std::string text(value);
    const char* ptr = text.c_str();
    if (api().message_iter_append_basic(&iter, dbus::kTypeString, static_cast<const void*>(&ptr)) == 0) {
        LogWarn << __FUNCTION__ << "append_string failed";
    }
}

void append_object_path(dbus::MessageIter& iter, std::string_view value)
{
    if (!api().valid()) {
        return;
    }
    const std::string text(value);
    const char* ptr = text.c_str();
    if (api().message_iter_append_basic(&iter, dbus::kTypeObjectPath, static_cast<const void*>(&ptr)) == 0) {
        LogWarn << __FUNCTION__ << "append_object_path failed";
    }
}

void append_uint32(dbus::MessageIter& iter, uint32_t value)
{
    if (api().valid() && api().message_iter_append_basic(&iter, dbus::kTypeUint32, &value) == 0) {
        LogWarn << __FUNCTION__ << "append_uint32 failed";
    }
}

void append_boolean(dbus::MessageIter& iter, bool value)
{
    const int raw = value ? 1 : 0;
    if (api().valid() && api().message_iter_append_basic(&iter, dbus::kTypeBoolean, &raw) == 0) {
        LogWarn << __FUNCTION__ << "append_boolean failed";
    }
}

void append_fd(dbus::MessageIter& iter, int fd)
{
    if (api().valid() && api().message_iter_append_basic(&iter, dbus::kTypeUnixFd, &fd) == 0) {
        LogWarn << __FUNCTION__ << "append_fd failed";
    }
}

bool get_string(dbus::MessageIter& iter, std::string& value)
{
    if (!api().valid()) {
        return false;
    }
    const int type = api().message_iter_get_arg_type(&iter);
    if (type != dbus::kTypeString && type != dbus::kTypeObjectPath) {
        return false;
    }
    const char* text = nullptr;
    api().message_iter_get_basic(&iter, static_cast<void*>(&text));
    if (text == nullptr) {
        return false;
    }
    value = text;
    return true;
}

bool get_uint32(dbus::MessageIter& iter, uint32_t& value)
{
    if (!api().valid()) {
        return false;
    }
    const int type = api().message_iter_get_arg_type(&iter);
    if (type == dbus::kTypeUint32) {
        api().message_iter_get_basic(&iter, &value);
        return true;
    }
    if (type == dbus::kTypeInt32) {
        int32_t signed_value = 0;
        api().message_iter_get_basic(&iter, &signed_value);
        value = static_cast<uint32_t>(signed_value);
        return true;
    }
    return false;
}

bool get_int32(dbus::MessageIter& iter, int32_t& value)
{
    if (!api().valid()) {
        return false;
    }
    const int type = api().message_iter_get_arg_type(&iter);
    if (type == dbus::kTypeInt32 || type == dbus::kTypeUint32) {
        api().message_iter_get_basic(&iter, &value);
        return true;
    }
    return false;
}

bool get_boolean(dbus::MessageIter& iter, bool& value)
{
    if (!api().valid() || api().message_iter_get_arg_type(&iter) != dbus::kTypeBoolean) {
        return false;
    }
    int raw = 0;
    api().message_iter_get_basic(&iter, &raw);
    value = raw != 0;
    return true;
}

bool get_fd(dbus::MessageIter& iter, int& value)
{
    if (!api().valid() || api().message_iter_get_arg_type(&iter) != dbus::kTypeUnixFd) {
        return false;
    }
    api().message_iter_get_basic(&iter, &value);
    return true;
}

std::optional<std::string> read_string_argument(dbus::Message* message)
{
    dbus::MessageIter iter {};
    if (message == nullptr || !api().valid() || api().message_iter_init(message, &iter) == 0) {
        return std::nullopt;
    }
    std::string value;
    if (!get_string(iter, value)) {
        return std::nullopt;
    }
    return value;
}

dbus::Message* new_method_return(dbus::Message* call)
{
    return api().valid() ? api().message_new_method_return(call) : nullptr;
}

bool send_reply(dbus::Connection* connection, dbus::Message* reply)
{
    if (connection == nullptr || reply == nullptr || !api().valid()) {
        return false;
    }
    std::array<uint32_t, 1> serial {};
    const bool ok = api().connection_send(connection, reply, serial.data()) != 0;
    api().connection_flush(connection);
    return ok;
}

void message_unref(dbus::Message* message)
{
    if (message != nullptr && api().valid()) {
        api().message_unref(message);
    }
}

int message_type(dbus::Message* message)
{
    return message != nullptr && api().valid() ? api().message_get_type(message) : dbus::kMessageTypeError;
}

const char* message_path(dbus::Message* message)
{
    return message != nullptr && api().valid() ? api().message_get_path(message) : nullptr;
}

bool is_signal(dbus::Message* message, const char* interface, const char* member)
{
    if (message == nullptr || !api().valid() || api().message_get_type(message) != dbus::kMessageTypeSignal) {
        return false;
    }
    const char* message_interface = api().message_get_interface(message);
    const char* message_member = api().message_get_member(message);
    return message_interface != nullptr && message_member != nullptr &&
           std::strcmp(message_interface, interface) == 0 && std::strcmp(message_member, member) == 0;
}

Variant read_variant(dbus::MessageIter& variant_iter)
{
    Variant result;
    if (!api().valid()) {
        return result;
    }
    dbus::MessageIter value {};
    api().message_iter_recurse(&variant_iter, &value);
    result.type = api().message_iter_get_arg_type(&value);
    switch (result.type) {
    case dbus::kTypeString:
    case dbus::kTypeObjectPath: {
        const char* text = nullptr;
        api().message_iter_get_basic(&value, static_cast<void*>(&text));
        if (text != nullptr) {
            result.string_value = text;
        }
        break;
    }
    case dbus::kTypeUint32: {
        uint32_t value32 = 0;
        api().message_iter_get_basic(&value, &value32);
        result.uint32_value = value32;
        break;
    }
    case dbus::kTypeInt32: {
        int32_t value32 = 0;
        api().message_iter_get_basic(&value, &value32);
        result.uint32_value = static_cast<uint32_t>(value32);
        break;
    }
    case dbus::kTypeInt64:
    case dbus::kTypeUint64: {
        int64_t value64 = 0;
        api().message_iter_get_basic(&value, &value64);
        result.int64_value = value64;
        break;
    }
    case dbus::kTypeBoolean: {
        int boolean = 0;
        api().message_iter_get_basic(&value, &boolean);
        result.uint32_value = boolean != 0 ? 1 : 0;
        break;
    }
    case dbus::kTypeUnixFd: {
        int fd = -1;
        api().message_iter_get_basic(&value, &fd);
        result.fd = fd;
        break;
    }
    default:
        break;
    }
    return result;
}

bool read_dict_entry_sv(dbus::MessageIter& entry, std::string& key, Variant& value)
{
    if (!api().valid()) {
        return false;
    }
    dbus::MessageIter fields {};
    api().message_iter_recurse(&entry, &fields);
    if (api().message_iter_get_arg_type(&fields) != dbus::kTypeString) {
        return false;
    }
    const char* key_text = nullptr;
    api().message_iter_get_basic(&fields, static_cast<void*>(&key_text));
    if (key_text == nullptr) {
        return false;
    }
    key = key_text;
    if (api().message_iter_next(&fields) == 0 || api().message_iter_get_arg_type(&fields) != dbus::kTypeVariant) {
        return false;
    }
    value = read_variant(fields);
    return true;
}
} // namespace dbus_util
} // namespace asst::waydroid

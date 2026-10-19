#include <catch2/catch_test_macros.hpp>

#include "Controller/Waydroid/DBusABI.h"
#include "Controller/Waydroid/PipeWireABI.h"

#include <cstddef>

#if defined(MAA_HAVE_DBUS_HEADERS)
#include <dbus/dbus.h>

static_assert(sizeof(asst::waydroid::dbus::Error) == sizeof(DBusError));
static_assert(offsetof(asst::waydroid::dbus::Error, name) == offsetof(DBusError, name));
static_assert(offsetof(asst::waydroid::dbus::Error, message) == offsetof(DBusError, message));
static_assert(offsetof(asst::waydroid::dbus::Error, padding1) == offsetof(DBusError, padding1));

static_assert(sizeof(asst::waydroid::dbus::MessageIter) == sizeof(DBusMessageIter));
static_assert(offsetof(asst::waydroid::dbus::MessageIter, dummy3) == offsetof(DBusMessageIter, dummy3));
static_assert(offsetof(asst::waydroid::dbus::MessageIter, pad2) == offsetof(DBusMessageIter, pad2));
static_assert(offsetof(asst::waydroid::dbus::MessageIter, pad3) == offsetof(DBusMessageIter, pad3));

static_assert(sizeof(asst::waydroid::dbus::ObjectPathVTable) == sizeof(DBusObjectPathVTable));
static_assert(
    offsetof(asst::waydroid::dbus::ObjectPathVTable, message_function) ==
    offsetof(DBusObjectPathVTable, message_function));

static_assert(asst::waydroid::dbus::kBusSession == DBUS_BUS_SESSION);
static_assert(asst::waydroid::dbus::kNameFlagDoNotQueue == DBUS_NAME_FLAG_DO_NOT_QUEUE);
static_assert(asst::waydroid::dbus::kTypeBoolean == DBUS_TYPE_BOOLEAN);
static_assert(asst::waydroid::dbus::kTypeInt32 == DBUS_TYPE_INT32);
static_assert(asst::waydroid::dbus::kTypeUint32 == DBUS_TYPE_UINT32);
static_assert(asst::waydroid::dbus::kTypeString == DBUS_TYPE_STRING);
static_assert(asst::waydroid::dbus::kTypeObjectPath == DBUS_TYPE_OBJECT_PATH);
static_assert(asst::waydroid::dbus::kTypeUnixFd == DBUS_TYPE_UNIX_FD);
static_assert(asst::waydroid::dbus::kTypeArray == DBUS_TYPE_ARRAY);
static_assert(asst::waydroid::dbus::kTypeVariant == DBUS_TYPE_VARIANT);
static_assert(asst::waydroid::dbus::kTypeStruct == DBUS_TYPE_STRUCT);
static_assert(asst::waydroid::dbus::kTypeDictEntry == DBUS_TYPE_DICT_ENTRY);
static_assert(asst::waydroid::dbus::kMessageTypeMethodCall == DBUS_MESSAGE_TYPE_METHOD_CALL);
static_assert(asst::waydroid::dbus::kMessageTypeMethodReturn == DBUS_MESSAGE_TYPE_METHOD_RETURN);
static_assert(asst::waydroid::dbus::kMessageTypeError == DBUS_MESSAGE_TYPE_ERROR);
static_assert(asst::waydroid::dbus::kMessageTypeSignal == DBUS_MESSAGE_TYPE_SIGNAL);
static_assert(asst::waydroid::dbus::kHandlerResultHandled == DBUS_HANDLER_RESULT_HANDLED);
static_assert(asst::waydroid::dbus::kHandlerResultNotYetHandled == DBUS_HANDLER_RESULT_NOT_YET_HANDLED);
#endif

#if defined(MAA_HAVE_SPA_HEADERS)
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/param/param.h>
#include <spa/param/video/raw.h>
#include <spa/pod/pod.h>

static_assert(sizeof(asst::waydroid::pwabi::SpaPod) == sizeof(struct spa_pod));
static_assert(sizeof(asst::waydroid::pwabi::SpaPodProp) == sizeof(struct spa_pod_prop));
static_assert(sizeof(asst::waydroid::pwabi::SpaPodChoiceBody) == sizeof(struct spa_pod_choice_body));
static_assert(sizeof(asst::waydroid::pwabi::SpaChunk) == sizeof(struct spa_chunk));
static_assert(offsetof(asst::waydroid::pwabi::SpaChunk, size) == offsetof(struct spa_chunk, size));
static_assert(offsetof(asst::waydroid::pwabi::SpaChunk, stride) == offsetof(struct spa_chunk, stride));
static_assert(sizeof(asst::waydroid::pwabi::SpaData) == sizeof(struct spa_data));
static_assert(offsetof(asst::waydroid::pwabi::SpaData, type) == offsetof(struct spa_data, type));
static_assert(offsetof(asst::waydroid::pwabi::SpaData, data) == offsetof(struct spa_data, data));
static_assert(offsetof(asst::waydroid::pwabi::SpaData, chunk) == offsetof(struct spa_data, chunk));
static_assert(sizeof(asst::waydroid::pwabi::SpaMeta) == sizeof(struct spa_meta));
static_assert(sizeof(asst::waydroid::pwabi::SpaBuffer) == sizeof(struct spa_buffer));
static_assert(offsetof(asst::waydroid::pwabi::SpaBuffer, datas) == offsetof(struct spa_buffer, datas));

static_assert(asst::waydroid::pwabi::kTypeId == SPA_TYPE_Id);
static_assert(asst::waydroid::pwabi::kTypeInt == SPA_TYPE_Int);
static_assert(asst::waydroid::pwabi::kTypeRectangle == SPA_TYPE_Rectangle);
static_assert(asst::waydroid::pwabi::kTypeChoice == SPA_TYPE_Choice);
static_assert(asst::waydroid::pwabi::kObjectFormat == SPA_TYPE_OBJECT_Format);
static_assert(asst::waydroid::pwabi::kObjectParamBuffers == SPA_TYPE_OBJECT_ParamBuffers);
static_assert(asst::waydroid::pwabi::kParamEnumFormat == SPA_PARAM_EnumFormat);
static_assert(asst::waydroid::pwabi::kParamFormat == SPA_PARAM_Format);
static_assert(asst::waydroid::pwabi::kParamBuffers == SPA_PARAM_Buffers);
static_assert(asst::waydroid::pwabi::kParamBuffersBuffers == SPA_PARAM_BUFFERS_buffers);
static_assert(asst::waydroid::pwabi::kParamBuffersDataType == SPA_PARAM_BUFFERS_dataType);
static_assert(asst::waydroid::pwabi::kFormatMediaType == SPA_FORMAT_mediaType);
static_assert(asst::waydroid::pwabi::kFormatMediaSubtype == SPA_FORMAT_mediaSubtype);
static_assert(asst::waydroid::pwabi::kFormatVideoFormat == SPA_FORMAT_VIDEO_format);
static_assert(asst::waydroid::pwabi::kFormatVideoSize == SPA_FORMAT_VIDEO_size);
static_assert(asst::waydroid::pwabi::kMediaTypeVideo == SPA_MEDIA_TYPE_video);
static_assert(asst::waydroid::pwabi::kMediaSubtypeRaw == SPA_MEDIA_SUBTYPE_raw);
static_assert(asst::waydroid::pwabi::kVideoFormatRGBx == SPA_VIDEO_FORMAT_RGBx);
static_assert(asst::waydroid::pwabi::kVideoFormatBGRx == SPA_VIDEO_FORMAT_BGRx);
static_assert(asst::waydroid::pwabi::kVideoFormatRGBA == SPA_VIDEO_FORMAT_RGBA);
static_assert(asst::waydroid::pwabi::kVideoFormatBGRA == SPA_VIDEO_FORMAT_BGRA);
static_assert(asst::waydroid::pwabi::kChoiceRange == SPA_CHOICE_Range);
static_assert(asst::waydroid::pwabi::kChoiceEnum == SPA_CHOICE_Enum);
static_assert(asst::waydroid::pwabi::kChoiceFlags == SPA_CHOICE_Flags);
static_assert(asst::waydroid::pwabi::kDataMemPtr == SPA_DATA_MemPtr);
static_assert(asst::waydroid::pwabi::kDataMemFd == SPA_DATA_MemFd);
static_assert(asst::waydroid::pwabi::kDataDmaBuf == SPA_DATA_DmaBuf);
static_assert(asst::waydroid::pwabi::kMetaVideoTransform == SPA_META_VideoTransform);
#endif

#if defined(MAA_HAVE_PIPEWIRE_HEADERS)
#include <pipewire/stream.h>

static_assert(asst::waydroid::pwabi::kVersionStreamEvents == PW_VERSION_STREAM_EVENTS);
static_assert(sizeof(asst::waydroid::pwabi::PwStreamEvents) == sizeof(struct pw_stream_events));
static_assert(
    offsetof(asst::waydroid::pwabi::PwStreamEvents, state_changed) == offsetof(struct pw_stream_events, state_changed));
static_assert(
    offsetof(asst::waydroid::pwabi::PwStreamEvents, param_changed) == offsetof(struct pw_stream_events, param_changed));
static_assert(offsetof(asst::waydroid::pwabi::PwStreamEvents, process) == offsetof(struct pw_stream_events, process));
static_assert(
    offsetof(asst::waydroid::pwabi::PwStreamEvents, trigger_done) == offsetof(struct pw_stream_events, trigger_done));
static_assert(sizeof(asst::waydroid::pwabi::SpaHookStorage) >= sizeof(struct spa_hook));
static_assert(asst::waydroid::pwabi::kDirectionInput == PW_DIRECTION_INPUT);
static_assert(asst::waydroid::pwabi::kStreamFlagAutoConnect == PW_STREAM_FLAG_AUTOCONNECT);
static_assert(asst::waydroid::pwabi::kStreamFlagMapBuffers == PW_STREAM_FLAG_MAP_BUFFERS);
#endif

TEST_CASE("hand-written ABI constants are self-consistent", "[waydroid][abi]")
{
    using namespace asst::waydroid;
    STATIC_REQUIRE(pwabi::kParamEnumFormat + 1 == pwabi::kParamFormat);
    STATIC_REQUIRE(pwabi::kParamFormat + 1 == pwabi::kParamBuffers);
    STATIC_REQUIRE(pwabi::kTypeId + 1 == pwabi::kTypeInt);
    STATIC_REQUIRE((1U << pwabi::kDataMemPtr) != (1U << pwabi::kDataMemFd));
    STATIC_REQUIRE(pwabi::kObjectFormat != pwabi::kObjectParamBuffers);
}

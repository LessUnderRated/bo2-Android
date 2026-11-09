// runtime/harness/include/aaudio/AAudio.h — the Linux test harness's stand-in for the NDK's AAudio header.
// Declares only what bo2-mac's coreaudio_out.cpp calls. aaudio_stub.cpp implements them: the stream never opens,
// so the runtime takes its silent fallback (the paced clock that keeps the engine's audio time without a device).
#pragma once
#include <cstdint>

typedef int32_t aaudio_result_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_data_callback_result_t;
typedef int32_t aaudio_direction_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_sharing_mode_t;
typedef int32_t aaudio_usage_t;
typedef int32_t aaudio_content_type_t;
typedef uint32_t aaudio_channel_mask_t;

enum : int32_t {
    AAUDIO_OK = 0,
    AAUDIO_ERROR_UNAVAILABLE = -889,
    AAUDIO_CALLBACK_RESULT_CONTINUE = 0,
    AAUDIO_DIRECTION_OUTPUT = 0,
    AAUDIO_FORMAT_PCM_I16 = 1,
    AAUDIO_FORMAT_PCM_FLOAT = 2,
    AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 12,
    AAUDIO_SHARING_MODE_SHARED = 1,
    AAUDIO_USAGE_GAME = 14,
    AAUDIO_CONTENT_TYPE_MOVIE = 3,
};
enum : uint32_t { AAUDIO_CHANNEL_5POINT1 = 0x3f };

struct AAudioStreamStruct;
struct AAudioStreamBuilderStruct;
typedef struct AAudioStreamStruct AAudioStream;
typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream*, void*, void*, int32_t);
typedef void (*AAudioStream_errorCallback)(AAudioStream*, void*, aaudio_result_t);

extern "C" {
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** builder);
const char* AAudio_convertResultToText(aaudio_result_t r);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder*, aaudio_direction_t);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder*, int32_t);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*, aaudio_format_t);
void AAudioStreamBuilder_setChannelMask(AAudioStreamBuilder*, aaudio_channel_mask_t);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*, int32_t);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder*, aaudio_performance_mode_t);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder*, aaudio_sharing_mode_t);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder*, aaudio_usage_t);
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder*, aaudio_content_type_t);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*, AAudioStream_dataCallback, void*);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder*, AAudioStream_errorCallback, void*);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder*, AAudioStream**);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder*);
int32_t AAudioStream_getChannelCount(AAudioStream*);
int32_t AAudioStream_getSampleRate(AAudioStream*);
aaudio_format_t AAudioStream_getFormat(AAudioStream*);
int32_t AAudioStream_getFramesPerBurst(AAudioStream*);
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream*);
aaudio_result_t AAudioStream_requestStart(AAudioStream*);
aaudio_result_t AAudioStream_requestStop(AAudioStream*);
aaudio_result_t AAudioStream_close(AAudioStream*);
}

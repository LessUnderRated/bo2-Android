// runtime/harness/aaudio_stub.cpp — no audio device in the Linux test harness: AAudio_createStreamBuilder fails, so
// coreaudio_out.cpp starts its silent fallback (the paced clock), as on a phone whose audio device refuses a stream.
#include <aaudio/AAudio.h>

extern "C" {
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** builder) {
    if (builder) *builder = nullptr;
    return AAUDIO_ERROR_UNAVAILABLE;
}
const char* AAudio_convertResultToText(aaudio_result_t r) { return r == AAUDIO_OK ? "OK" : "no audio device (harness)"; }
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder*, aaudio_direction_t) {}
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder*, int32_t) {}
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*, aaudio_format_t) {}
void AAudioStreamBuilder_setChannelMask(AAudioStreamBuilder*, aaudio_channel_mask_t) {}
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*, int32_t) {}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder*, aaudio_performance_mode_t) {}
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder*, aaudio_sharing_mode_t) {}
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder*, aaudio_usage_t) {}
void AAudioStreamBuilder_setContentType(AAudioStreamBuilder*, aaudio_content_type_t) {}
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*, AAudioStream_dataCallback, void*) {}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder*, AAudioStream_errorCallback, void*) {}
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder*, AAudioStream** s) {
    if (s) *s = nullptr;
    return AAUDIO_ERROR_UNAVAILABLE;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder*) { return AAUDIO_OK; }
int32_t AAudioStream_getChannelCount(AAudioStream*) { return 0; }
int32_t AAudioStream_getSampleRate(AAudioStream*) { return 0; }
aaudio_format_t AAudioStream_getFormat(AAudioStream*) { return 0; }
int32_t AAudioStream_getFramesPerBurst(AAudioStream*) { return 0; }
aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream*) { return 0; }
aaudio_result_t AAudioStream_requestStart(AAudioStream*) { return AAUDIO_ERROR_UNAVAILABLE; }
aaudio_result_t AAudioStream_requestStop(AAudioStream*) { return AAUDIO_OK; }
aaudio_result_t AAudioStream_close(AAudioStream*) { return AAUDIO_OK; }
}

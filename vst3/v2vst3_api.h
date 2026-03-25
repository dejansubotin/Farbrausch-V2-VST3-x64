#pragma once

#include <windows.h>
#include <cmath>

#ifndef _SOUNDDEF_H_
enum V2CTLTYPES
{
	VCTL_SKIP,
	VCTL_SLIDER,
	VCTL_MB,
};

typedef struct
{
	int no;
	char *name;
	char *name2;
} V2TOPIC;

typedef struct
{
	int version;
	char *name;
	V2CTLTYPES ctltype;
	int offset;
	int min;
	int max;
	int isdest;
	char *ctlstr;
} V2PARAM;
#endif

typedef void (__cdecl *V2Vst3DisplayCallback)(void *context);

typedef bool (__cdecl *V2VST3_INIT_FUNC)(const char *bankPath);
typedef void (__cdecl *V2VST3_SHUTDOWN_FUNC)();
typedef void (__cdecl *V2VST3_SET_SAMPLERATE_FUNC)(int newRate);
typedef void (__cdecl *V2VST3_QUEUE_MIDI_EVENT_FUNC)(unsigned int offset, DWORD midiData);
typedef void (__cdecl *V2VST3_PROCESS_REPLACING_FUNC)(float **outputs, int sampleFrames);
typedef long (__cdecl *V2VST3_GET_CHUNK_FUNC)(void **data);
typedef long (__cdecl *V2VST3_SET_CHUNK_FUNC)(void *data, long byteSize);
typedef void (__cdecl *V2VST3_GET_PARAM_DEFS_FUNC)(
	V2TOPIC **topics,
	int *topicCount,
	V2PARAM **parameters,
	int *parameterCount,
	V2TOPIC **globalTopics,
	int *globalTopicCount,
	V2PARAM **globalParameters,
	int *globalParameterCount);
typedef int (__cdecl *V2VST3_GET_HOST_PARAMETER_FUNC)(int index);
typedef void (__cdecl *V2VST3_SET_HOST_PARAMETER_FUNC)(int index, int value);
typedef int (__cdecl *V2VST3_GET_CURRENT_PATCH_FUNC)();
typedef void (__cdecl *V2VST3_SET_CURRENT_PATCH_FUNC)(int index);
typedef void (__cdecl *V2VST3_GET_EDITOR_SIZE_FUNC)(int *width, int *height);
typedef HWND (__cdecl *V2VST3_EDITOR_ATTACH_FUNC)(HWND parent);
typedef void (__cdecl *V2VST3_EDITOR_REMOVE_FUNC)();
typedef void (__cdecl *V2VST3_SET_DISPLAY_CALLBACK_FUNC)(V2Vst3DisplayCallback callback, void *context);

inline double v2ParmToNormalized(int value, int minValue, int maxValue)
{
	if (maxValue <= minValue)
	{
		return 0.0;
	}

	return static_cast<double>(value - minValue) / (static_cast<double>(maxValue - minValue) + 0.5);
}

inline int v2NormalizedToParm(double normalizedValue, int minValue, int maxValue)
{
	if (maxValue <= minValue)
	{
		return minValue;
	}

	const double scaled = normalizedValue * static_cast<double>(maxValue - minValue) - 0.5;
	return static_cast<int>(std::lround(scaled + 0.5)) + minValue;
}

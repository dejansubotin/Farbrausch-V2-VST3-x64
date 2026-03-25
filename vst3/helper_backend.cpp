#pragma warning(disable:4244)

#include "helper_backend.h"

#include <math.h>

#include "../types.h"
#include "../tool/file.h"
#include "../vsti/midi.h"
#include "../synth.h"
#include "../vsti/v2mrecorder.h"

static CRITICAL_SECTION g_cs;
static LARGE_INTEGER g_perfFreq;

static sU8 g_midiBuffer[65536];
static int g_midiBufferLength;
static int g_lastOffset;
static sU32 *g_framePtr;

static float g_peakLeft;
static float g_peakRight;
static int g_clipLeft;
static int g_clipRight;
static int g_cpuUsage;
static int g_polyPressureAsCC;

static CV2MRecorder *g_recorder = 0;
static CV2MRecorder *g_lastRecorder = 0;
static int g_recordTime = 0;
static int g_sampleRate = 44100;

sU8 *theSynth = 0;

void msInit()
{
	InitializeCriticalSection(&g_cs);
	QueryPerformanceFrequency(&g_perfFreq);

	g_peakLeft = 0.0f;
	g_peakRight = 0.0f;
	g_clipLeft = 0;
	g_clipRight = 0;
	g_cpuUsage = 0;
	g_polyPressureAsCC = 1;
	g_recordTime = 0;

	g_lastOffset = 0;
	*(sU32 *)g_midiBuffer = g_lastOffset;
	g_framePtr = reinterpret_cast<sU32 *>(g_midiBuffer + 4);
	g_midiBufferLength = 8;
	g_midiBuffer[g_midiBufferLength] = 0xfd;

	if (!theSynth)
	{
		theSynth = new sU8[3 * 1024 * 1024];
	}
}

void msClose()
{
	delete g_recorder;
	g_recorder = 0;

	delete g_lastRecorder;
	g_lastRecorder = 0;

	delete[] theSynth;
	theSynth = 0;

	DeleteCriticalSection(&g_cs);
}

void msProcessEvent(DWORD offset, DWORD midiData)
{
	EnterCriticalSection(&g_cs);

	if (g_midiBufferLength < 65500)
	{
		if (static_cast<int>(offset) < g_lastOffset)
		{
			LeaveCriticalSection(&g_cs);
			return;
		}

		if (static_cast<int>(offset) != g_lastOffset)
		{
			g_midiBuffer[g_midiBufferLength++] = 0xfd;
			*g_framePtr = static_cast<sU32>(g_midiBuffer + g_midiBufferLength - reinterpret_cast<sU8 *>(g_framePtr) - 4);
			*reinterpret_cast<sU32 *>(g_midiBuffer + g_midiBufferLength) = g_lastOffset = static_cast<int>(offset);
			g_framePtr = reinterpret_cast<sU32 *>(g_midiBuffer + g_midiBufferLength + 4);
			g_midiBufferLength += 8;
		}

		sU8 b0 = static_cast<sU8>(midiData & 0xff);
		sU8 b1 = static_cast<sU8>((midiData >> 8) & 0xff);
		sU8 b2 = static_cast<sU8>((midiData >> 16) & 0xff);

#ifdef SINGLECHN
		if (b0 < 0xf0)
		{
			b0 &= 0xf0;
		}
#endif

		if ((b0 >> 4) == 0xa && g_polyPressureAsCC)
		{
			b0 = static_cast<sU8>((b0 & 0x0f) | 0xb0);
		}

		if (g_recorder)
		{
			g_recorder->AddEvent(g_recordTime + static_cast<int>(offset), b0, b1, b2);
		}

		switch (b0 >> 4)
		{
		case 0x8:
		case 0x9:
		case 0xa:
		case 0xb:
		case 0xe:
			g_midiBuffer[g_midiBufferLength++] = b0;
			g_midiBuffer[g_midiBufferLength++] = b1;
			g_midiBuffer[g_midiBufferLength++] = b2;
			break;

		case 0xc:
		case 0xd:
			g_midiBuffer[g_midiBufferLength++] = b0;
			g_midiBuffer[g_midiBufferLength++] = b1;
			break;

		default:
			break;
		}
	}

	LeaveCriticalSection(&g_cs);
}

void msSetProgram1(int program)
{
#ifdef SINGLECHN
	EnterCriticalSection(&g_cs);
	unsigned char midiData[4] = { 0xc0, static_cast<unsigned char>(program & 0x7f), 0xfd, 0 };
	synthProcessMIDI(theSynth, midiData);
	LeaveCriticalSection(&g_cs);
#else
	(void)program;
#endif
}

static void Render(float *left, float *right, unsigned int frameCount)
{
	LARGE_INTEGER t0;
	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t0);

	EnterCriticalSection(&g_cs);

	sUInt renderedFrames = 0;
	sInt midiOffset = 0;

	g_midiBuffer[g_midiBufferLength++] = 0xfd;
	*g_framePtr = static_cast<sU32>(g_midiBuffer + g_midiBufferLength - reinterpret_cast<sU8 *>(g_framePtr) - 4);

	while (midiOffset < g_midiBufferLength || renderedFrames < frameCount)
	{
		sU32 nextOffset = frameCount;
		sU32 eventLength = 0;
		if (midiOffset < g_midiBufferLength)
		{
			nextOffset = *reinterpret_cast<sU32 *>(g_midiBuffer + midiOffset);
			midiOffset += 4;
			eventLength = *reinterpret_cast<sU32 *>(g_midiBuffer + midiOffset);
			midiOffset += 4;
		}

		if (nextOffset > renderedFrames)
		{
			synthRender(theSynth, left + renderedFrames, static_cast<int>(nextOffset - renderedFrames), right + renderedFrames, 0);
		}
		renderedFrames = nextOffset;

		if (eventLength)
		{
			synthProcessMIDI(theSynth, g_midiBuffer + midiOffset);
		}
		midiOffset += eventLength;
	}

	g_lastOffset = 0;
	*reinterpret_cast<sU32 *>(g_midiBuffer) = g_lastOffset;
	g_framePtr = reinterpret_cast<sU32 *>(g_midiBuffer + 4);
	g_midiBufferLength = 8;

	LeaveCriticalSection(&g_cs);

	QueryPerformanceCounter(&t1);
	t1.QuadPart = 44100 * (t1.QuadPart - t0.QuadPart) / g_perfFreq.QuadPart;
	g_cpuUsage = frameCount ? static_cast<int>(100 * t1.QuadPart / frameCount) : 0;
	g_recordTime += static_cast<int>(frameCount);

	for (unsigned int i = 0; i < frameCount; ++i)
	{
		const float leftValue = fabsf(left[i]);
		g_peakLeft *= 0.99985f;
		if (leftValue > g_peakLeft)
		{
			g_peakLeft = leftValue;
		}
		if (leftValue > 1.0f)
		{
			g_clipLeft = 1;
		}

		const float rightValue = fabsf(right[i]);
		g_peakRight *= 0.99985f;
		if (rightValue > g_peakRight)
		{
			g_peakRight = rightValue;
		}
		if (rightValue > 1.0f)
		{
			g_clipRight = 1;
		}
	}
}

void v2vst3RenderBlock(float **outputs, int sampleFrames)
{
	if (!outputs || sampleFrames <= 0)
	{
		return;
	}

	Render(outputs[0], outputs[1], static_cast<unsigned int>(sampleFrames));
}

void msGetLD(float *left, float *right, int *clipLeft, int *clipRight)
{
	if (left)
	{
		*left = g_peakLeft;
	}
	if (right)
	{
		*right = g_peakRight;
	}
	if (clipLeft)
	{
		*clipLeft = g_clipLeft;
	}
	if (clipRight)
	{
		*clipRight = g_clipRight;
	}

	g_clipLeft = 0;
	g_clipRight = 0;
}

int msGetCPUUsage()
{
	return g_cpuUsage;
}

#ifdef RONAN
void msStartAudio(HWND, void *patchMap, void *globalState, const char **speech)
#else
void msStartAudio(HWND, void *patchMap, void *globalState)
#endif
{
	EnterCriticalSection(&g_cs);
	synthInit(theSynth, patchMap, g_sampleRate);
	synthSetGlobals(theSynth, globalState);
#ifdef RONAN
	synthSetLyrics(theSynth, speech);
#endif
	LeaveCriticalSection(&g_cs);
}

void msStopAudio()
{
}

#ifdef RONAN
void msSetSampleRate(int newRate, void *patchMap, void *globalState, const char **speech)
#else
void msSetSampleRate(int newRate, void *patchMap, void *globalState)
#endif
{
	if (newRate > 0)
	{
		g_sampleRate = newRate;
	}

#ifdef RONAN
	msStartAudio(0, patchMap, globalState, speech);
#else
	msStartAudio(0, patchMap, globalState);
#endif
}

void msStartRecord()
{
	if (g_lastRecorder)
	{
		delete g_lastRecorder;
		g_lastRecorder = 0;
	}
	if (g_recorder)
	{
		delete g_recorder;
	}

	g_recorder = new CV2MRecorder(synthGetFrameSize(theSynth), g_sampleRate);

	int programs[16] = {};
	synthGetPgm(theSynth, programs);
	for (sU32 i = 0; i < 16; ++i)
	{
		g_recorder->AddEvent(0, static_cast<sU8>(0xc0 | i), static_cast<sU8>(programs[i]), 0);
	}

	g_recordTime = 0;
}

int msIsRecording()
{
	return g_recorder != 0;
}

int msStopRecord(file &f)
{
	if (!g_recorder)
	{
		return 0;
	}

	const int result = g_recorder->Export(f);
	delete g_recorder;
	g_recorder = 0;
	return result;
}

void msStopRecord()
{
	if (g_recorder)
	{
		delete g_lastRecorder;
		g_lastRecorder = g_recorder;
		g_recorder = 0;
	}
}

int msWriteLastRecord(file &f)
{
	if (!g_lastRecorder)
	{
		return 0;
	}

	const int result = g_lastRecorder->Export(f);
	delete g_lastRecorder;
	g_lastRecorder = 0;
	return result;
}

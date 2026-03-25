#include "midi_backend.h"

#include <deque>
#include <algorithm>
#include <vector>
#include <mmsystem.h>

#include "../dsio.h"
#include "../synth.h"
#include "../tool/file.h"
#include "../sounddef.h"
#include "../vsti/v2mrecorder.h"

#pragma comment(lib, "winmm.lib")

namespace
{
	struct QueuedMidiEvent
	{
		sU64 samplePos;
		DWORD data;
	};

	static CRITICAL_SECTION g_cs;
	static LARGE_INTEGER g_perfFreq;
	static std::deque<QueuedMidiEvent> g_eventQueue;
	static std::vector<HMIDIIN> g_midiInputs;
	static int g_midiDeviceCount = 0;
	static bool g_audioOpen = false;
	static sU64 g_renderedSamples = 0;
	static sU64 g_recordBaseSample = 0;
	static float g_peakLeft = 0.0f;
	static float g_peakRight = 0.0f;
	static int g_clipLeft = 0;
	static int g_clipRight = 0;
	static int g_cpuUsage = 0;
	static int g_sampleRate = 44100;
	static CV2MRecorder *g_recorder = 0;
	static CV2MRecorder *g_lastRecorder = 0;
}

sU8 *theSynth = 0;

static void CloseMidiInputDevices()
{
	for (size_t index = 0; index < g_midiInputs.size(); ++index)
	{
		HMIDIIN midiIn = g_midiInputs[index];
		if (!midiIn)
		{
			continue;
		}

		midiInStop(midiIn);
		midiInClose(midiIn);
	}
	g_midiInputs.clear();
}

static sU64 GetCurrentSamplePosition()
{
	if (!g_audioOpen)
	{
		return g_renderedSamples;
	}

	const sS32 samplePos = dsGetCurSmp();
	if (samplePos < 0)
	{
		return g_renderedSamples;
	}

	return static_cast<sU64>(samplePos);
}

static void EncodeMidiData(DWORD data, unsigned char *buffer, int &length)
{
	const unsigned char b0 = static_cast<unsigned char>(data & 0xff);
	const unsigned char b1 = static_cast<unsigned char>((data >> 8) & 0xff);
	const unsigned char b2 = static_cast<unsigned char>((data >> 16) & 0xff);

	buffer[0] = b0;

	switch (b0 >> 4)
	{
	case 0x8:
	case 0x9:
	case 0xa:
	case 0xb:
	case 0xe:
		buffer[1] = b1;
		buffer[2] = b2;
		buffer[3] = 0xfd;
		length = 3;
		break;

	case 0xc:
	case 0xd:
		buffer[1] = b1;
		buffer[2] = 0xfd;
		length = 2;
		break;

	default:
		buffer[1] = 0xfd;
		length = 1;
		break;
	}
}

static void QueueMidiEventInternal(sU64 samplePos, DWORD midiData)
{
	QueuedMidiEvent event = { samplePos, midiData };

	std::deque<QueuedMidiEvent>::iterator it = g_eventQueue.end();
	while (it != g_eventQueue.begin())
	{
		std::deque<QueuedMidiEvent>::iterator prev = it;
		--prev;
		if (prev->samplePos <= event.samplePos)
		{
			break;
		}
		it = prev;
	}
	g_eventQueue.insert(it, event);
}

static void QueueMidiEvent(sU64 samplePos, DWORD midiData)
{
	EnterCriticalSection(&g_cs);
	QueueMidiEventInternal(samplePos, midiData);

	if (g_recorder)
	{
		const unsigned char b0 = static_cast<unsigned char>(midiData & 0xff);
		const unsigned char b1 = static_cast<unsigned char>((midiData >> 8) & 0xff);
		const unsigned char b2 = static_cast<unsigned char>((midiData >> 16) & 0xff);
		const sU64 relativeSample = (samplePos > g_recordBaseSample) ? (samplePos - g_recordBaseSample) : 0;
		g_recorder->AddEvent(static_cast<long>(relativeSample), b0, b1, b2);
	}

	LeaveCriticalSection(&g_cs);
}

static void ProcessMidiInputMessage(DWORD midiData)
{
	const unsigned char b0 = static_cast<unsigned char>(midiData & 0xff);
	const unsigned char hi = static_cast<unsigned char>(b0 >> 4);
	if (hi == 0xf)
	{
		return;
	}

	switch (hi)
	{
	case 0x8:
	case 0x9:
	case 0xa:
	case 0xb:
	case 0xc:
	case 0xd:
	case 0xe:
		QueueMidiEvent(GetCurrentSamplePosition(), midiData);
		break;

	default:
		break;
	}
}

static void CALLBACK MidiInCallback(HMIDIIN, UINT message, DWORD_PTR, DWORD_PTR param1, DWORD_PTR)
{
	if (message == MIM_DATA)
	{
		ProcessMidiInputMessage(static_cast<DWORD>(param1));
	}
}

static bool OpenMidiInputDevice(int deviceIndex)
{
	if (deviceIndex < 0 || deviceIndex >= g_midiDeviceCount)
	{
		return false;
	}

	HMIDIIN midiIn = 0;
	if (midiInOpen(&midiIn, static_cast<UINT>(deviceIndex), reinterpret_cast<DWORD_PTR>(&MidiInCallback), 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
	{
		return false;
	}

	if (midiInStart(midiIn) != MMSYSERR_NOERROR)
	{
		midiInClose(midiIn);
		return false;
	}

	g_midiInputs.push_back(midiIn);
	return true;
}

static void RenderAudio(float *outBuffer, unsigned int frameCount)
{
	LARGE_INTEGER t0;
	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t0);

	EnterCriticalSection(&g_cs);

	const sU64 blockStart = g_renderedSamples;
	const sU64 blockEnd = blockStart + frameCount;
	unsigned int renderedFrames = 0;

	while (!g_eventQueue.empty())
	{
		sU64 eventPos = g_eventQueue.front().samplePos;
		if (eventPos > blockEnd)
		{
			break;
		}

		if (eventPos < blockStart)
		{
			eventPos = blockStart;
		}

		const unsigned int eventFrame = static_cast<unsigned int>(std::min<sU64>(frameCount, eventPos - blockStart));
		if (eventFrame > renderedFrames)
		{
			synthRender(theSynth, outBuffer + (renderedFrames * 2), static_cast<int>(eventFrame - renderedFrames));
			renderedFrames = eventFrame;
		}

		unsigned char midiBuffer[4] = {};
		int midiLength = 0;
		EncodeMidiData(g_eventQueue.front().data, midiBuffer, midiLength);
		(void)midiLength;
		synthProcessMIDI(theSynth, midiBuffer);
		g_eventQueue.pop_front();
	}

	if (renderedFrames < frameCount)
	{
		synthRender(theSynth, outBuffer + (renderedFrames * 2), static_cast<int>(frameCount - renderedFrames));
	}

	g_renderedSamples += frameCount;

	LeaveCriticalSection(&g_cs);

	QueryPerformanceCounter(&t1);
	const sS64 elapsed = t1.QuadPart - t0.QuadPart;
	if (frameCount)
	{
		const sS64 samplesAt44100 = (44100LL * elapsed) / g_perfFreq.QuadPart;
		g_cpuUsage = static_cast<int>((100LL * samplesAt44100) / frameCount);
	}
	else
	{
		g_cpuUsage = 0;
	}

	for (unsigned int i = 0; i < frameCount; ++i)
	{
		const float left = fabsf(outBuffer[i * 2 + 0]);
		g_peakLeft *= 0.99985f;
		if (left > g_peakLeft)
		{
			g_peakLeft = left;
		}
		if (left > 1.0f)
		{
			g_clipLeft = 1;
		}

		const float right = fabsf(outBuffer[i * 2 + 1]);
		g_peakRight *= 0.99985f;
		if (right > g_peakRight)
		{
			g_peakRight = right;
		}
		if (right > 1.0f)
		{
			g_clipRight = 1;
		}
	}
}

static void __stdcall DsRenderCallback(void *, float *outBuffer, unsigned long frameCount)
{
	RenderAudio(outBuffer, frameCount);
}

void msInit()
{
	InitializeCriticalSection(&g_cs);
	QueryPerformanceFrequency(&g_perfFreq);

	g_eventQueue.clear();
	g_renderedSamples = 0;
	g_recordBaseSample = 0;
	g_peakLeft = g_peakRight = 0.0f;
	g_clipLeft = g_clipRight = 0;
	g_cpuUsage = 0;
	g_sampleRate = 44100;
	g_audioOpen = false;
	g_midiDeviceCount = static_cast<int>(midiInGetNumDevs());
	g_midiInputs.clear();
	g_recorder = 0;
	g_lastRecorder = 0;

	if (!theSynth)
	{
		theSynth = new sU8[3 * 1024 * 1024];
	}
}

void msClose()
{
	msClosePreferredMidiInput();
	msCloseStandaloneAudio();

	EnterCriticalSection(&g_cs);
	delete g_recorder;
	g_recorder = 0;
	delete g_lastRecorder;
	g_lastRecorder = 0;
	g_eventQueue.clear();
	LeaveCriticalSection(&g_cs);

	delete[] theSynth;
	theSynth = 0;

	DeleteCriticalSection(&g_cs);
}

bool msOpenStandaloneAudio(HWND hwnd)
{
	if (g_audioOpen)
	{
		return true;
	}

	g_renderedSamples = 0;
	if (dsInit(&DsRenderCallback, 0, hwnd) != 0)
	{
		return false;
	}

	g_audioOpen = true;
	return true;
}

void msCloseStandaloneAudio()
{
	if (!g_audioOpen)
	{
		return;
	}

	dsClose();
	g_audioOpen = false;
	EnterCriticalSection(&g_cs);
	g_eventQueue.clear();
	g_renderedSamples = 0;
	LeaveCriticalSection(&g_cs);
}

bool msOpenPreferredMidiInput()
{
	CloseMidiInputDevices();

	int openedCount = 0;
	for (int i = 0; i < g_midiDeviceCount; ++i)
	{
		if (OpenMidiInputDevice(i))
		{
			++openedCount;
		}
	}

	return openedCount > 0;
}

void msClosePreferredMidiInput()
{
	CloseMidiInputDevices();
}

void msProcessEvent(DWORD offs, DWORD midiData)
{
	QueueMidiEvent(g_renderedSamples + static_cast<sU64>(offs), midiData);
}

void msSetProgram1(int program)
{
#ifdef SINGLECHN
	unsigned char midiBuffer[4] = { 0xc0, static_cast<unsigned char>(program & 0x7f), 0xfd, 0 };
	EnterCriticalSection(&g_cs);
	synthProcessMIDI(theSynth, midiBuffer);
	LeaveCriticalSection(&g_cs);
#else
	(void)program;
#endif
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
void msStartAudio(HWND, void *patchmap, void *globalsPtr, const char **speech)
#else
void msStartAudio(HWND, void *patchmap, void *globalsPtr)
#endif
{
	EnterCriticalSection(&g_cs);
	synthInit(theSynth, patchmap, g_sampleRate);
	synthSetGlobals(theSynth, globalsPtr);
#ifdef RONAN
	synthSetLyrics(theSynth, speech);
#endif
	LeaveCriticalSection(&g_cs);
}

void msStopAudio()
{
}

#ifdef RONAN
void msSetSampleRate(int newrate, void *patchmap, void *globalsPtr, const char **speech)
#else
void msSetSampleRate(int newrate, void *patchmap, void *globalsPtr)
#endif
{
	if (newrate > 0)
	{
		g_sampleRate = newrate;
	}

#ifdef RONAN
	msStartAudio(0, patchmap, globalsPtr, speech);
#else
	msStartAudio(0, patchmap, globalsPtr);
#endif
}

void msStartRecord()
{
	EnterCriticalSection(&g_cs);

	delete g_lastRecorder;
	g_lastRecorder = 0;
	delete g_recorder;
	g_recorder = new CV2MRecorder(synthGetFrameSize(theSynth), g_sampleRate);
	g_recordBaseSample = GetCurrentSamplePosition();

	int programs[16] = {};
	synthGetPgm(theSynth, programs);
	for (sU32 i = 0; i < 16; ++i)
	{
		g_recorder->AddEvent(0, static_cast<sU8>(0xc0 | i), static_cast<sU8>(programs[i]), 0);
	}

	LeaveCriticalSection(&g_cs);
}

int msIsRecording()
{
	return g_recorder != 0;
}

int msStopRecord(file &f)
{
	EnterCriticalSection(&g_cs);

	if (!g_recorder)
	{
		LeaveCriticalSection(&g_cs);
		return 0;
	}

	const int result = g_recorder->Export(f);
	delete g_recorder;
	g_recorder = 0;

	LeaveCriticalSection(&g_cs);
	return result;
}

void msStopRecord()
{
	EnterCriticalSection(&g_cs);

	if (g_recorder)
	{
		delete g_lastRecorder;
		g_lastRecorder = g_recorder;
		g_recorder = 0;
	}

	LeaveCriticalSection(&g_cs);
}

int msWriteLastRecord(file &f)
{
	EnterCriticalSection(&g_cs);

	if (!g_lastRecorder)
	{
		LeaveCriticalSection(&g_cs);
		return 0;
	}

	const int result = g_lastRecorder->Export(f);
	delete g_lastRecorder;
	g_lastRecorder = 0;

	LeaveCriticalSection(&g_cs);
	return result;
}

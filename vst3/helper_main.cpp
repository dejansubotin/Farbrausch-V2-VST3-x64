#define WINVER 0x0601
#define _WIN32_IE 0x0601
#define _RICHEDIT_VER 0x0200
#define _CRT_SECURE_NO_DEPRECATE
#define _CRT_NON_CONFORMING_SWPRINTFS
#define _WTL_NO_CSTRING

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <atlbase.h>
#include <atlstr.h>
#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlcrack.h>
#include <atlctrls.h>
#include <atlctrlw.h>
#include <atlctrlx.h>
#include <atldlgs.h>
#include <atlframe.h>
#include <atlgdi.h>
#include <atlres.h>
#include <atlscrl.h>
#include <atlsplit.h>
#include <atlmisc.h>
#include <commctrl.h>

#include "../types.h"
#include "../tool/file.h"
#include "../sounddef.h"
#include "../synth.h"
class AudioEffectX
{
public:
	virtual ~AudioEffectX()
	{
	}

	virtual void updateDisplay() = 0;
};

AudioEffectX *v2vstiAEffect = 0;
CAppModule _Module;

#include "../vsti/resource.h"
#include "../vsti/v2host.h"
#include "../vsti/midi.h"

#include "helper_backend.h"
#include "v2vst3_api.h"

namespace
{
	static const int kEditorWidth = 695;
	static const int kEditorHeight = 872;

	class DisplayBridge : public AudioEffectX
	{
	public:
		void SetCallback(V2Vst3DisplayCallback callback, void *context)
		{
			m_callback = callback;
			m_context = context;
		}

		void updateDisplay() override
		{
			if (m_callback)
			{
				m_callback(m_context);
			}
		}

	private:
		V2Vst3DisplayCallback m_callback = 0;
		void *m_context = 0;
	};

	class HelperClient : public V2::IClient
	{
	public:
		void Attach(V2::GUI::View *view)
		{
			m_view = view;
		}

		void CurrentParameterChanged(int) override
		{
		}

		void CurrentChannelChanged() override
		{
			PostPatchChanged();
		}

		void CurrentPatchChanged() override
		{
			PostPatchChanged();
		}

	private:
		void PostPatchChanged()
		{
			if (m_view && m_view->m_hWnd)
			{
				m_view->PostPatchChanged();
			}
		}

		V2::GUI::View *m_view = 0;
	};

	class HelperEditor
	{
	public:
		void AttachClient(HelperClient &client)
		{
			m_client = &client;
		}

		HWND Attach(HWND parent)
		{
			if (m_view.m_hWnd)
			{
				return m_view.m_hWnd;
			}

			m_view.VSTMode();
			const HWND child = m_view.Create(parent);
			if (child)
			{
				::SetWindowPos(child, 0, 0, 0, kEditorWidth, kEditorHeight, SWP_NOZORDER | SWP_SHOWWINDOW);
			}

			if (m_client)
			{
				m_client->Attach(&m_view);
			}

			return child;
		}

		void Remove()
		{
			if (m_client)
			{
				m_client->Attach(0);
			}

			if (m_view.m_hWnd)
			{
				m_view.DestroyWindow();
			}
		}

		void Refresh()
		{
			if (m_view.m_hWnd)
			{
				m_view.PostPatchChanged();
			}
		}

	private:
		V2::GUI::View m_view;
		HelperClient *m_client = 0;
	};

	static bool g_initialized = false;
	static unsigned char *g_chunk = 0;

	static HelperClient g_client;
	static HelperEditor g_editor;
	static DisplayBridge g_displayBridge;

	static bool LoadFactoryBank(const char *bankPath)
	{
		if (!bankPath || !*bankPath)
		{
			return false;
		}

		fileS bankFile;
		if (!bankFile.open(bankPath))
		{
			return false;
		}

		const bool loaded = sdLoad(bankFile) ? true : false;
		bankFile.close();
		return loaded;
	}

	static void FreeChunk()
	{
		delete[] g_chunk;
		g_chunk = 0;
	}

	static int ClampHostParameter(int index, int value)
	{
		if (index < v2nparms)
		{
			if (value < v2parms[index].min)
			{
				value = v2parms[index].min;
			}
			if (value > v2parms[index].max)
			{
				value = v2parms[index].max;
			}
			return value;
		}

		index -= v2nparms;
		if (index < 0 || index >= v2ngparms)
		{
			return 0;
		}
		if (value < v2gparms[index].min)
		{
			value = v2gparms[index].min;
		}
		if (value > v2gparms[index].max)
		{
			value = v2gparms[index].max;
		}
		return value;
	}

	static bool InitializeState(const char *bankPath)
	{
		if (g_initialized)
		{
			return true;
		}

		sdInit();
		LoadFactoryBank(bankPath);

		msInit();
		g_client.Attach(0);
		g_editor.AttachClient(g_client);
		V2::g_theHost.SetClient(g_client);

#ifdef RONAN
		msStartAudio(0, soundmem, globals, const_cast<const char **>(speechptrs));
#else
		msStartAudio(0, soundmem, globals);
#endif

		g_initialized = true;
		return true;
	}

	static void ShutdownState()
	{
		if (!g_initialized)
		{
			return;
		}

		g_editor.Remove();
		FreeChunk();
		V2::g_theHost.Release();
		msStopAudio();
		msClose();
		sdClose();
		g_initialized = false;
	}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		::CoInitialize(0);

		INITCOMMONCONTROLSEX commonControls = {};
		commonControls.dwSize = sizeof(commonControls);
		commonControls.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
		::InitCommonControlsEx(&commonControls);

		_Module.Init(0, instance);
		V2::GUI::Initialize();
		v2vstiAEffect = &g_displayBridge;
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		ShutdownState();
		v2vstiAEffect = 0;
		V2::GUI::Uninitialize();
		_Module.Term();
		::CoUninitialize();
	}

	return TRUE;
}

extern "C"
{
	__declspec(dllexport) bool __cdecl v2vst3Init(const char *bankPath)
	{
		return InitializeState(bankPath);
	}

	__declspec(dllexport) void __cdecl v2vst3Shutdown()
	{
		ShutdownState();
	}

	__declspec(dllexport) void __cdecl v2vst3SetSampleRate(int newRate)
	{
#ifdef RONAN
		msSetSampleRate(newRate, soundmem, globals, const_cast<const char **>(speechptrs));
#else
		msSetSampleRate(newRate, soundmem, globals);
#endif
	}

	__declspec(dllexport) void __cdecl v2vst3QueueMidiEvent(unsigned int offset, DWORD midiData)
	{
		msProcessEvent(offset, midiData);
	}

	__declspec(dllexport) void __cdecl v2vst3ProcessReplacing(float **outputs, int sampleFrames)
	{
		v2vst3RenderBlock(outputs, sampleFrames);
	}

	__declspec(dllexport) long __cdecl v2vst3GetChunk(void **data)
	{
		if (!data)
		{
			return 0;
		}

		FreeChunk();

		fileMTmp temp;
		temp.open();

		temp.putsU32('hund');
		const int version = 1;
		temp.write(version);
		temp.putsU32('PGM0');

		int programs[16] = {};
		synthGetPgm(theSynth, programs);
		temp.write(programs, 16 * 4);

		sdSaveBank(temp);
		V2::GUI::GetTheAppearance()->SaveAppearance(temp);

		temp.seek(0);
		const int chunkSize = temp.size();
		g_chunk = new unsigned char[chunkSize];
		temp.read(g_chunk, chunkSize);
		temp.close();

		*data = g_chunk;
		return chunkSize;
	}

	__declspec(dllexport) long __cdecl v2vst3SetChunk(void *data, long byteSize)
	{
		if (!data || byteSize <= 0)
		{
			return 0;
		}

		fileM chunk;
		chunk.open(data, byteSize);

		int programs[16] = {};
		int version = 0;

		sU32 programTag = chunk.getsU32();
		if (programTag == 'hund')
		{
			chunk.read(version);
			programTag = chunk.getsU32();
		}

		if (programTag == 'PGM0')
		{
			chunk.read(programs, 4 * 16);
		}
		else
		{
			chunk.seekcur(-4);
		}

		sdLoad(chunk);
		if (version >= 1)
		{
			V2::GUI::GetTheAppearance()->LoadAppearance(chunk);
		}

#ifdef RONAN
		msStartAudio(0, soundmem, globals, const_cast<const char **>(speechptrs));
#else
		msStartAudio(0, soundmem, globals);
#endif

		if (programTag == 'PGM0')
		{
#ifdef SINGLECHN
			msProcessEvent(0, (programs[0] << 8) | 0xc0);
#else
			for (sU32 i = 0; i < 16; ++i)
			{
				msProcessEvent(0, (programs[i] << 8) | 0xc0 | i);
			}
#endif
		}

		g_editor.Refresh();
		return 0;
	}

	__declspec(dllexport) void __cdecl v2vst3GetParameterDefs(
		V2TOPIC **topics,
		int *topicCount,
		V2PARAM **parameters,
		int *parameterCount,
		V2TOPIC **globalTopics,
		int *globalTopicCount,
		V2PARAM **globalParameters,
		int *globalParameterCount)
	{
		if (topics)
		{
			*topics = const_cast<V2TOPIC *>(v2topics);
		}
		if (topicCount)
		{
			*topicCount = v2ntopics;
		}
		if (parameters)
		{
			*parameters = const_cast<V2PARAM *>(v2parms);
		}
		if (parameterCount)
		{
			*parameterCount = v2nparms;
		}
		if (globalTopics)
		{
			*globalTopics = const_cast<V2TOPIC *>(v2gtopics);
		}
		if (globalTopicCount)
		{
			*globalTopicCount = v2ngtopics;
		}
		if (globalParameters)
		{
			*globalParameters = const_cast<V2PARAM *>(v2gparms);
		}
		if (globalParameterCount)
		{
			*globalParameterCount = v2ngparms;
		}
	}

	__declspec(dllexport) int __cdecl v2vst3GetHostParameter(int index)
	{
		if (index < 0)
		{
			return 0;
		}

		if (index < v2nparms)
		{
			const int offset = 128 * 4 + (v2soundsize * v2curpatch);
			return soundmem[offset + index];
		}

		index -= v2nparms;
		if (index >= 0 && index < v2ngparms)
		{
			return static_cast<unsigned char>(globals[index]);
		}

		return 0;
	}

	__declspec(dllexport) void __cdecl v2vst3SetHostParameter(int index, int value)
	{
		value = ClampHostParameter(index, value);
		if (index < 0)
		{
			return;
		}

		if (index < v2nparms)
		{
			const int offset = 128 * 4 + (v2soundsize * v2curpatch);
			soundmem[offset + index] = static_cast<unsigned char>(value);
			g_editor.Refresh();
			return;
		}

		index -= v2nparms;
		if (index >= 0 && index < v2ngparms)
		{
			globals[index] = static_cast<char>(value);
			synthSetGlobals(theSynth, globals);
			g_editor.Refresh();
		}
	}

	__declspec(dllexport) int __cdecl v2vst3GetCurrentPatchIndex()
	{
		return v2curpatch;
	}

	__declspec(dllexport) void __cdecl v2vst3SetCurrentPatchIndex(int index)
	{
		V2::GetTheSynth()->SetCurrentPatchIndex(index);
	}

	__declspec(dllexport) void __cdecl v2vst3GetEditorSize(int *width, int *height)
	{
		if (width)
		{
			*width = kEditorWidth;
		}
		if (height)
		{
			*height = kEditorHeight;
		}
	}

	__declspec(dllexport) HWND __cdecl v2vst3EditorAttach(HWND parent)
	{
		return g_editor.Attach(parent);
	}

	__declspec(dllexport) void __cdecl v2vst3EditorRemove()
	{
		g_editor.Remove();
	}

	__declspec(dllexport) void __cdecl v2vst3SetDisplayCallback(V2Vst3DisplayCallback callback, void *context)
	{
		g_displayBridge.SetCallback(callback, context);
	}
}

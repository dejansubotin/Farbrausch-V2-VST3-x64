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
#include <algorithm>

class AudioEffectX
{
public:
	void updateDisplay()
	{
	}
};

AudioEffectX *v2vstiAEffect = 0;

#include "../types.h"
#include "../sounddef.h"
#include "../synth.h"
#include "../tool/file.h"
#include "../vsti/resource.h"
#include "../vsti/v2host.h"
#include "midi_backend.h"

CAppModule _Module;

namespace
{
	bool LoadFactoryBank()
	{
		char modulePath[MAX_PATH] = {};
		if (!::GetModuleFileNameA(0, modulePath, MAX_PATH))
		{
			return false;
		}

		char *slash = modulePath + lstrlenA(modulePath);
		while (slash > modulePath && *slash != '\\' && *slash != '/')
		{
			--slash;
		}
		if (*slash == '\\' || *slash == '/')
		{
			*slash = 0;
		}

		lstrcatA(modulePath, "\\presets.v2b");

		fileS bankFile;
		if (!bankFile.open(modulePath))
		{
			return false;
		}

		const bool loaded = sdLoad(bankFile) ? true : false;
		bankFile.close();
		return loaded;
	}

	class StandaloneClient : public V2::IClient
	{
	public:
		void Attach(V2::GUI::View *view)
		{
			m_view = view;
		}

		void CurrentParameterChanged(int) override
		{
			PostPatchChanged();
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

	class StandaloneFrame : public V2::GUI::Frame
	{
	public:
		BEGIN_MSG_MAP_EX(StandaloneFrame)
			MSG_WM_CLOSE(OnClose)
			MSG_WM_DESTROY(OnDestroy)
			CHAIN_MSG_MAP(V2::GUI::Frame)
		END_MSG_MAP()

		void OnClose()
		{
			SetMsgHandled(TRUE);
			DestroyWindow();
		}

		void OnDestroy()
		{
			PostQuitMessage(0);
			SetMsgHandled(FALSE);
		}
	};
}

static int Run(int nCmdShow)
{
	CMessageLoop messageLoop;
	_Module.AddMessageLoop(&messageLoop);

	V2::GUI::Initialize();
	sdInit();
	LoadFactoryBank();
	msInit();

	StandaloneClient client;
	V2::g_theHost.SetClient(client);

	StandaloneFrame frame;
	if (frame.Create() == NULL)
	{
		V2::g_theHost.Release();
		msClose();
		sdClose();
		V2::GUI::Uninitialize();
		_Module.RemoveMessageLoop();
		return 0;
	}

	client.Attach(&frame.m_view);

#ifdef RONAN
	msStartAudio(frame, soundmem, globals, const_cast<const char **>(speechptrs));
#else
	msStartAudio(frame, soundmem, globals);
#endif

	if (!msOpenStandaloneAudio(frame))
	{
		MessageBox(frame, "DirectSound output could not be opened.", "farbrausch V2", MB_OK | MB_ICONERROR);
		frame.DestroyWindow();
		V2::g_theHost.Release();
		msClose();
		sdClose();
		V2::GUI::Uninitialize();
		_Module.RemoveMessageLoop();
		return 0;
	}

	if (!msOpenPreferredMidiInput())
	{
		MessageBox(frame, "No MIDI input devices could be opened. The editor and audio output will still work, but live MIDI input is unavailable.", "farbrausch V2", MB_OK | MB_ICONWARNING);
	}

	frame.ShowWindow(nCmdShow);
	frame.UpdateWindow();

	const int result = messageLoop.Run();

	msClosePreferredMidiInput();
	msCloseStandaloneAudio();
	V2::g_theHost.Release();
	msClose();
	sdClose();
	V2::GUI::Uninitialize();
	_Module.RemoveMessageLoop();

	return result;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int nCmdShow)
{
	HRESULT hr = ::CoInitialize(0);
	if (FAILED(hr))
	{
		return 1;
	}

	INITCOMMONCONTROLSEX commonControls = {};
	commonControls.dwSize = sizeof(commonControls);
	commonControls.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
	::InitCommonControlsEx(&commonControls);

	hr = _Module.Init(0, instance);
	if (FAILED(hr))
	{
		::CoUninitialize();
		return 1;
	}

	const int result = Run(nCmdShow);

	_Module.Term();
	::CoUninitialize();
	return result;
}

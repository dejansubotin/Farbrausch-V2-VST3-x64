#define WINVER 0x0601
#define _WIN32_IE 0x0601
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../types.h"
#include "../dsio.h"
#include "../sounddef.h"
#include "../v2mconv.h"
#include "../v2mplayer.h"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

namespace
{
	const wchar_t *kWindowClass = L"FarbrauschV2MPlayerX64";
	const UINT_PTR kUiTimerId = 1;
	const UINT kUiTimerMs = 33;
	const UINT kTargetSampleRate = 44100;

	template <typename T>
	T ClampValue(T value, T minimum, T maximum)
	{
		return std::max(minimum, std::min(maximum, value));
	}

	std::wstring Widen(const char *text)
	{
		if (!text || !*text)
			return std::wstring();

		const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, 0, 0);
		if (length <= 1)
			return std::wstring();

		std::wstring result(static_cast<size_t>(length - 1), L'\0');
		MultiByteToWideChar(CP_ACP, 0, text, -1, &result[0], length);
		return result;
	}

	std::wstring GetFileNameFromPath(const std::wstring &path)
	{
		const size_t slash = path.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
	}

	std::wstring FormatTimeMs(sU32 milliseconds)
	{
		const sU32 totalSeconds = milliseconds / 1000;
		const sU32 seconds = totalSeconds % 60;
		const sU32 minutes = (totalSeconds / 60) % 60;
		const sU32 hours = totalSeconds / 3600;

		wchar_t buffer[32] = {};
		if (hours)
			swprintf(buffer, 32, L"%u:%02u:%02u", hours, minutes, seconds);
		else
			swprintf(buffer, 32, L"%02u:%02u", minutes, seconds);
		return buffer;
	}

	bool ReadWholeFile(const std::wstring &path, std::vector<unsigned char> &data)
	{
		data.clear();

		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
		if (file == INVALID_HANDLE_VALUE)
			return false;

		LARGE_INTEGER size = {};
		if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024)
		{
			CloseHandle(file);
			return false;
		}

		data.resize(static_cast<size_t>(size.QuadPart));
		DWORD bytesRead = 0;
		const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &bytesRead, 0);
		CloseHandle(file);
		return ok && bytesRead == data.size();
	}

	void BuildRoundedRect(GraphicsPath &path, const RectF &rect, REAL radius)
	{
		path.Reset();
		const REAL diameter = radius * 2.0f;
		path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
		path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
		path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter, 0.0f, 90.0f);
		path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
		path.CloseFigure();
	}

	struct ButtonLayout
	{
		RectF rect;
		std::wstring label;
		bool enabled = true;
		bool active = false;
	};

	struct UiLayout
	{
		RectF chrome;
		RectF panel;
		RectF dropZone;
		RectF visualizer;
		RectF progressTrack;
		RectF fileNameRect;
		RectF statusRect;
		RectF timeRect;
		ButtonLayout openButton;
		ButtonLayout playButton;
		ButtonLayout pauseButton;
		ButtonLayout stopButton;
	};

	class VisualizerBuffer
	{
	public:
		VisualizerBuffer()
		{
			InitializeCriticalSection(&m_lock);
			Clear();
		}

		~VisualizerBuffer()
		{
			DeleteCriticalSection(&m_lock);
		}

		void Clear()
		{
			EnterCriticalSection(&m_lock);
			m_bins.fill(0.0f);
			m_writeIndex = 0;
			m_levelLeft = 0.0f;
			m_levelRight = 0.0f;
			LeaveCriticalSection(&m_lock);
		}

		void PushSamples(const float *samples, sU32 frameCount)
		{
			if (!samples || !frameCount)
				return;

			const sU32 slices = std::min<sU32>(16, std::max<sU32>(1, frameCount / 64));
			const sU32 sliceSize = std::max<sU32>(1, frameCount / slices);

			EnterCriticalSection(&m_lock);
			for (sU32 slice = 0; slice < slices; ++slice)
			{
				const sU32 begin = slice * sliceSize;
				const sU32 end = (slice + 1 == slices) ? frameCount : std::min(frameCount, begin + sliceSize);
				float peak = 0.0f;
				float peakLeft = 0.0f;
				float peakRight = 0.0f;
				for (sU32 index = begin; index < end; ++index)
				{
					const float left = std::fabs(samples[index * 2 + 0]);
					const float right = std::fabs(samples[index * 2 + 1]);
					peak = std::max(peak, std::max(left, right));
					peakLeft = std::max(peakLeft, left);
					peakRight = std::max(peakRight, right);
				}
				m_bins[m_writeIndex] = peak;
				m_writeIndex = (m_writeIndex + 1) % m_bins.size();
				m_levelLeft = std::max(m_levelLeft * 0.9f, peakLeft);
				m_levelRight = std::max(m_levelRight * 0.9f, peakRight);
			}
			LeaveCriticalSection(&m_lock);
		}

		void Tick()
		{
			EnterCriticalSection(&m_lock);
			for (size_t i = 0; i < m_bins.size(); ++i)
				m_bins[i] *= 0.94f;
			m_levelLeft *= 0.9f;
			m_levelRight *= 0.9f;
			LeaveCriticalSection(&m_lock);
		}

		void Snapshot(std::array<float, 192> &bins, float &levelLeft, float &levelRight)
		{
			EnterCriticalSection(&m_lock);
			for (size_t i = 0; i < bins.size(); ++i)
			{
				const size_t source = (m_writeIndex + i) % m_bins.size();
				bins[i] = m_bins[source];
			}
			levelLeft = m_levelLeft;
			levelRight = m_levelRight;
			LeaveCriticalSection(&m_lock);
		}

	private:
		CRITICAL_SECTION m_lock;
		std::array<float, 192> m_bins = {};
		size_t m_writeIndex = 0;
		float m_levelLeft = 0.0f;
		float m_levelRight = 0.0f;
	};

	class PlayerEngine
	{
	public:
		enum PlaybackState
		{
			StateEmpty,
			StateStopped,
			StatePlaying,
			StatePaused,
		};

		PlayerEngine()
		{
			m_player.Init();
		}

		~PlayerEngine()
		{
			Shutdown();
		}

		bool Initialize(HWND hwnd)
		{
			if (m_audioReady)
				return true;

			if (dsInit(&PlayerEngine::RenderThunk, this, hwnd) != 0)
			{
				m_statusText = L"DirectSound output could not be opened.";
				return false;
			}

			m_audioReady = true;
			return true;
		}

		void Shutdown()
		{
			if (m_audioReady)
			{
				dsLock();
				m_player.Close();
				dsUnlock();
				dsClose();
				m_audioReady = false;
			}
			else
			{
				m_player.Close();
			}
		}

		bool LoadFile(const std::wstring &path)
		{
			std::vector<unsigned char> sourceData;
			if (!ReadWholeFile(path, sourceData))
			{
				m_statusText = L"Could not read the dropped file.";
				return false;
			}

			int versionDelta = 0;
			if (!sourceData.empty())
				versionDelta = CheckV2MVersion(sourceData.data(), static_cast<int>(sourceData.size()));

			if (versionDelta < 0)
			{
				const int errorIndex = -versionDelta;
				if (errorIndex >= 0 && errorIndex < 3)
					m_statusText = Widen(v2mconv_errors[errorIndex]);
				else
					m_statusText = L"Unsupported or broken V2M file.";
				return false;
			}

			std::vector<unsigned char> playbackData;
			bool converted = false;
			if (versionDelta != 0)
			{
				unsigned char *convertedData = 0;
				int convertedSize = 0;
				ConvertV2M(sourceData.data(), static_cast<int>(sourceData.size()), &convertedData, &convertedSize);
				if (!convertedData || convertedSize <= 0)
				{
					delete[] convertedData;
					m_statusText = L"Failed to convert the V2M file to the current format.";
					return false;
				}
				playbackData.assign(convertedData, convertedData + convertedSize);
				delete[] convertedData;
				converted = true;
			}
			else
			{
				playbackData.swap(sourceData);
			}

			if (m_audioReady)
				dsLock();

			m_player.Close();
			m_fileData.swap(playbackData);
			if (!m_player.Open(m_fileData.data(), kTargetSampleRate))
			{
				m_fileData.clear();
				if (m_audioReady)
					dsUnlock();
				m_statusText = L"V2M playback engine failed to open the file.";
				m_state = StateEmpty;
				m_durationMs = 0;
				m_queuedPositionMs = 0;
				return false;
			}
			m_player.Stop(0);

			if (m_audioReady)
				dsUnlock();

			m_loadedPath = path;
			m_loadedName = GetFileNameFromPath(path);
			m_state = StateStopped;
			m_durationMs = m_player.GetLengthMs();
			m_queuedPositionMs = 0;
			m_legacyConverted = converted;
			m_visualizer.Clear();
			m_statusText = converted
				? L"Loaded and converted a legacy V2M file on the fly."
				: L"Drop another .v2m file or click the timeline to seek.";
			return true;
		}

		void Play()
		{
			if (m_state == StateEmpty)
				return;

			sU32 startPosition = m_queuedPositionMs;
			if (m_durationMs && startPosition >= m_durationMs)
				startPosition = 0;

			if (m_audioReady)
				dsLock();
			m_player.Play(startPosition);
			if (m_audioReady)
				dsUnlock();

			m_queuedPositionMs = startPosition;
			m_state = StatePlaying;
		}

		void Pause()
		{
			if (m_state != StatePlaying)
				return;

			if (m_audioReady)
				dsLock();
			m_queuedPositionMs = m_player.GetPlaybackTimeMs();
			m_player.Stop(0);
			if (m_audioReady)
				dsUnlock();

			m_state = StatePaused;
		}

		void Stop()
		{
			if (m_state == StateEmpty)
				return;

			if (m_state == StatePlaying && m_audioReady)
				dsLock();
			if (m_state == StatePlaying)
				m_player.Stop(0);
			if (m_state == StatePlaying && m_audioReady)
				dsUnlock();

			m_queuedPositionMs = 0;
			m_state = StateStopped;
		}

		void SeekToMs(sU32 positionMs)
		{
			if (m_state == StateEmpty)
				return;

			positionMs = ClampValue<sU32>(positionMs, static_cast<sU32>(0), m_durationMs);
			if (m_state == StatePlaying)
			{
				if (m_audioReady)
					dsLock();
				m_player.Play(positionMs);
				if (m_audioReady)
					dsUnlock();
			}
			m_queuedPositionMs = positionMs;
		}

		void Tick()
		{
			m_visualizer.Tick();
			if (m_state != StatePlaying)
				return;

			sU32 playbackMs = 0;
			sBool stillPlaying = 0;
			if (m_audioReady)
				dsLock();
			playbackMs = m_player.GetPlaybackTimeMs();
			stillPlaying = m_player.IsPlaying();
			if (m_audioReady)
				dsUnlock();

			if (!stillPlaying)
			{
				m_queuedPositionMs = m_durationMs;
				m_state = StateStopped;
			}
			else
			{
				m_queuedPositionMs = playbackMs;
			}
		}

		sU32 GetCurrentMs() const
		{
			return m_queuedPositionMs;
		}

		sU32 GetDurationMs() const
		{
			return m_durationMs;
		}

		PlaybackState GetState() const
		{
			return m_state;
		}

		bool HasFile() const
		{
			return m_state != StateEmpty;
		}

		const std::wstring &GetLoadedName() const
		{
			return m_loadedName;
		}

		const std::wstring &GetLoadedPath() const
		{
			return m_loadedPath;
		}

		const std::wstring &GetStatusText() const
		{
			return m_statusText;
		}

		bool WasConverted() const
		{
			return m_legacyConverted;
		}

		void GetVisualizer(std::array<float, 192> &bins, float &levelLeft, float &levelRight)
		{
			m_visualizer.Snapshot(bins, levelLeft, levelRight);
		}

	private:
		static void __stdcall RenderThunk(void *context, sF32 *buffer, sU32 frameCount)
		{
			static_cast<PlayerEngine *>(context)->Render(buffer, frameCount);
		}

		void Render(sF32 *buffer, sU32 frameCount)
		{
			m_player.Render(buffer, frameCount, 0);
			m_visualizer.PushSamples(buffer, frameCount);
		}

		V2MPlayer m_player;
		std::vector<unsigned char> m_fileData;
		VisualizerBuffer m_visualizer;
		std::wstring m_loadedPath;
		std::wstring m_loadedName;
		std::wstring m_statusText = L"Drop a .v2m file anywhere in the window.";
		PlaybackState m_state = StateEmpty;
		sU32 m_durationMs = 0;
		sU32 m_queuedPositionMs = 0;
		bool m_audioReady = false;
		bool m_legacyConverted = false;
	};

	class PlayerWindow
	{
	public:
		PlayerWindow()
		{
			m_app = this;
		}

		~PlayerWindow()
		{
			if (m_gdiplusToken)
				GdiplusShutdown(m_gdiplusToken);
			if (m_dropTarget)
				DragFinish(m_dropTarget);
			sdClose();
		}

		bool Create(HINSTANCE instance, const std::wstring &startupPath)
		{
			sdInit();

			GdiplusStartupInput gdiplusStartupInput;
			if (GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, 0) != Ok)
				return false;

			WNDCLASSW windowClass = {};
			windowClass.hCursor = LoadCursor(0, IDC_ARROW);
			windowClass.hInstance = instance;
			windowClass.lpfnWndProc = &PlayerWindow::WndProc;
			windowClass.lpszClassName = kWindowClass;
			windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
			if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
				return false;

			const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME;
			RECT rect = { 0, 0, 920, 560 };
			AdjustWindowRect(&rect, style, FALSE);

			m_hwnd = CreateWindowW(
				kWindowClass,
				L"Farbrausch V2M Player x64",
				style,
				CW_USEDEFAULT,
				CW_USEDEFAULT,
				rect.right - rect.left,
				rect.bottom - rect.top,
				0,
				0,
				instance,
				this);
			if (!m_hwnd)
				return false;

			DragAcceptFiles(m_hwnd, TRUE);
			SetTimer(m_hwnd, kUiTimerId, kUiTimerMs, 0);

			if (!m_engine.Initialize(m_hwnd))
			{
				MessageBoxW(m_hwnd, L"DirectSound output could not be initialized.", L"Farbrausch V2M Player x64", MB_OK | MB_ICONERROR);
				return false;
			}

			ShowWindow(m_hwnd, SW_SHOWDEFAULT);
			UpdateWindow(m_hwnd);

			if (!startupPath.empty())
				LoadFile(startupPath);

			return true;
		}

	private:
		static PlayerWindow *m_app;

		static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
		{
			PlayerWindow *self = reinterpret_cast<PlayerWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				CREATESTRUCTW *createStruct = reinterpret_cast<CREATESTRUCTW *>(lParam);
				self = reinterpret_cast<PlayerWindow *>(createStruct->lpCreateParams);
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			}

			return self ? self->HandleMessage(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
		}

		LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
		{
			switch (message)
			{
			case WM_DROPFILES:
				OnDropFiles(reinterpret_cast<HDROP>(wParam));
				return 0;

			case WM_TIMER:
				if (wParam == kUiTimerId)
				{
					m_engine.Tick();
					InvalidateRect(hwnd, 0, FALSE);
				}
				return 0;

			case WM_GETMINMAXINFO:
				{
					MINMAXINFO *info = reinterpret_cast<MINMAXINFO *>(lParam);
					info->ptMinTrackSize.x = 760;
					info->ptMinTrackSize.y = 460;
				}
				return 0;

			case WM_LBUTTONDOWN:
				OnLeftButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
				return 0;

			case WM_PAINT:
				OnPaint();
				return 0;

			case WM_ERASEBKGND:
				return 1;

			case WM_DESTROY:
				KillTimer(hwnd, kUiTimerId);
				PostQuitMessage(0);
				return 0;
			}

			return DefWindowProcW(hwnd, message, wParam, lParam);
		}

		void OnDropFiles(HDROP drop)
		{
			wchar_t path[MAX_PATH] = {};
			if (DragQueryFileW(drop, 0, path, MAX_PATH))
				LoadFile(path);
			DragFinish(drop);
		}

		void OnLeftButtonDown(int x, int y)
		{
			const PointF point(static_cast<REAL>(x), static_cast<REAL>(y));
			const UiLayout layout = GetLayout();

			if (layout.openButton.rect.Contains(point))
			{
				OpenDialog();
				return;
			}
			if (layout.playButton.enabled && layout.playButton.rect.Contains(point))
			{
				m_engine.Play();
				return;
			}
			if (layout.pauseButton.enabled && layout.pauseButton.rect.Contains(point))
			{
				m_engine.Pause();
				return;
			}
			if (layout.stopButton.enabled && layout.stopButton.rect.Contains(point))
			{
				m_engine.Stop();
				return;
			}
			if (layout.progressTrack.Contains(point) && m_engine.HasFile())
			{
				const float normalized = (point.X - layout.progressTrack.X) / layout.progressTrack.Width;
				const sU32 seekMs = static_cast<sU32>(ClampValue(normalized, 0.0f, 1.0f) * static_cast<float>(m_engine.GetDurationMs()));
				m_engine.SeekToMs(seekMs);
				return;
			}
			if (layout.dropZone.Contains(point) && !m_engine.HasFile())
			{
				OpenDialog();
			}
		}

		void OpenDialog()
		{
			wchar_t path[MAX_PATH] = {};
			OPENFILENAMEW ofn = {};
			ofn.lStructSize = sizeof(ofn);
			ofn.hwndOwner = m_hwnd;
			ofn.lpstrFile = path;
			ofn.nMaxFile = MAX_PATH;
			ofn.lpstrFilter = L"V2 modules (*.v2m)\0*.v2m\0All files (*.*)\0*.*\0\0";
			ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
			if (GetOpenFileNameW(&ofn))
				LoadFile(path);
		}

		void LoadFile(const std::wstring &path)
		{
			if (!m_engine.LoadFile(path))
			{
				MessageBoxW(m_hwnd, m_engine.GetStatusText().c_str(), L"Farbrausch V2M Player x64", MB_OK | MB_ICONERROR);
			}
			InvalidateRect(m_hwnd, 0, TRUE);
		}

		UiLayout GetLayout() const
		{
			RECT clientRect = {};
			GetClientRect(m_hwnd, &clientRect);

			const float width = static_cast<float>(clientRect.right - clientRect.left);
			const float height = static_cast<float>(clientRect.bottom - clientRect.top);
			const float pad = 24.0f;
			const float gap = 16.0f;

			UiLayout layout = {};
			layout.chrome = RectF(0.0f, 0.0f, width, height);
			layout.panel = RectF(pad, 88.0f, width - pad * 2.0f, std::max(220.0f, height - 220.0f));
			layout.dropZone = RectF(layout.panel.X + 18.0f, layout.panel.Y + 18.0f, layout.panel.Width - 36.0f, layout.panel.Height - 36.0f);
			layout.visualizer = RectF(layout.panel.X + 24.0f, layout.panel.Y + 24.0f, layout.panel.Width - 48.0f, std::max(120.0f, layout.panel.Height - 118.0f));
			layout.progressTrack = RectF(layout.panel.X + 24.0f, layout.panel.GetBottom() - 40.0f, layout.panel.Width - 48.0f, 12.0f);
			layout.fileNameRect = RectF(pad, 28.0f, width - 280.0f, 34.0f);
			layout.timeRect = RectF(width - 190.0f, layout.panel.GetBottom() - 68.0f, 160.0f, 22.0f);

			const float buttonY = layout.panel.GetBottom() + 24.0f;
			layout.openButton.rect = RectF(pad, buttonY, 90.0f, 36.0f);
			layout.openButton.label = L"Open";
			layout.playButton.rect = RectF(layout.openButton.rect.GetRight() + gap, buttonY, 90.0f, 36.0f);
			layout.playButton.label = L"Play";
			layout.pauseButton.rect = RectF(layout.playButton.rect.GetRight() + gap, buttonY, 90.0f, 36.0f);
			layout.pauseButton.label = L"Pause";
			layout.stopButton.rect = RectF(layout.pauseButton.rect.GetRight() + gap, buttonY, 90.0f, 36.0f);
			layout.stopButton.label = L"Stop";
			layout.statusRect = RectF(layout.stopButton.rect.GetRight() + 24.0f, buttonY + 2.0f, width - (layout.stopButton.rect.GetRight() + 48.0f), 34.0f);

			layout.playButton.enabled = m_engine.HasFile();
			layout.pauseButton.enabled = m_engine.GetState() == PlayerEngine::StatePlaying;
			layout.stopButton.enabled = m_engine.HasFile();
			layout.playButton.active = m_engine.GetState() == PlayerEngine::StatePlaying;
			layout.pauseButton.active = m_engine.GetState() == PlayerEngine::StatePaused;
			layout.stopButton.active = m_engine.GetState() == PlayerEngine::StateStopped && m_engine.HasFile() && m_engine.GetCurrentMs() == 0;
			return layout;
		}

		void DrawButton(Graphics &graphics, const ButtonLayout &button)
		{
			const Color baseFill = button.enabled
				? (button.active ? Color(255, 70, 186, 214) : Color(255, 33, 42, 66))
				: Color(255, 22, 28, 46);
			const Color borderColor = button.active ? Color(255, 112, 230, 255) : Color(255, 66, 80, 108);
			const Color textColor = button.enabled ? Color(255, 245, 249, 255) : Color(255, 128, 138, 160);

			GraphicsPath path(FillModeWinding);
			BuildRoundedRect(path, button.rect, 12.0f);
			SolidBrush fillBrush(baseFill);
			Pen borderPen(borderColor, 1.0f);
			graphics.FillPath(&fillBrush, &path);
			graphics.DrawPath(&borderPen, &path);

			Font font(L"Segoe UI Semibold", 11.5f, FontStyleRegular, UnitPixel);
			StringFormat format;
			format.SetAlignment(StringAlignmentCenter);
			format.SetLineAlignment(StringAlignmentCenter);
			SolidBrush textBrush(textColor);
			graphics.DrawString(button.label.c_str(), -1, &font, button.rect, &format, &textBrush);
		}

		void OnPaint()
		{
			PAINTSTRUCT paintStruct = {};
			HDC hdc = BeginPaint(m_hwnd, &paintStruct);

			RECT clientRect = {};
			GetClientRect(m_hwnd, &clientRect);
			const int width = clientRect.right - clientRect.left;
			const int height = clientRect.bottom - clientRect.top;

			Bitmap backBuffer(width, height, PixelFormat32bppPARGB);
			Graphics graphics(&backBuffer);
			graphics.SetSmoothingMode(SmoothingModeAntiAlias);
			graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

			const UiLayout layout = GetLayout();

			LinearGradientBrush backgroundBrush(
				PointF(0.0f, 0.0f),
				PointF(static_cast<REAL>(width), static_cast<REAL>(height)),
				Color(255, 8, 12, 21),
				Color(255, 18, 25, 39));
			graphics.FillRectangle(&backgroundBrush, layout.chrome);

			SolidBrush titleBrush(Color(255, 246, 250, 255));
			SolidBrush subtitleBrush(Color(255, 143, 156, 178));
			Font titleFont(L"Segoe UI Semibold", 24.0f, FontStyleRegular, UnitPixel);
			Font subtitleFont(L"Segoe UI", 12.5f, FontStyleRegular, UnitPixel);

			const std::wstring title = m_engine.HasFile() ? m_engine.GetLoadedName() : L"Farbrausch V2M Player x64";
			graphics.DrawString(title.c_str(), -1, &titleFont, layout.fileNameRect, 0, &titleBrush);

			RectF subtitleRect(layout.fileNameRect.X, layout.fileNameRect.Y + 36.0f, layout.fileNameRect.Width, 20.0f);
			const std::wstring subtitle = m_engine.HasFile()
				? (m_engine.WasConverted() ? L"Legacy V2M converted on load for playback." : L"Native V2M playback with the original Farbrausch synth.")
				: L"Drag and drop a .v2m file to play it with the original Farbrausch V2 engine.";
			graphics.DrawString(subtitle.c_str(), -1, &subtitleFont, subtitleRect, 0, &subtitleBrush);

			GraphicsPath panelPath(FillModeWinding);
			BuildRoundedRect(panelPath, layout.panel, 24.0f);
			SolidBrush panelBrush(Color(230, 17, 24, 38));
			Pen panelBorder(Color(255, 52, 68, 92), 1.0f);
			graphics.FillPath(&panelBrush, &panelPath);
			graphics.DrawPath(&panelBorder, &panelPath);

			std::array<float, 192> bins = {};
			float levelLeft = 0.0f;
			float levelRight = 0.0f;
			m_engine.GetVisualizer(bins, levelLeft, levelRight);

			if (m_engine.HasFile())
			{
				const float barWidth = std::max(2.0f, layout.visualizer.Width / static_cast<float>(bins.size()));
				SolidBrush barBrush(Color(220, 77, 208, 219));
				SolidBrush glowBrush(Color(110, 119, 240, 255));
				for (size_t index = 0; index < bins.size(); ++index)
				{
					const float normalized = ClampValue(bins[index], 0.0f, 1.0f);
					const float x = layout.visualizer.X + static_cast<float>(index) * barWidth;
					const float barHeight = std::max(3.0f, normalized * (layout.visualizer.Height - 8.0f));
					const RectF barRect(x, layout.visualizer.GetBottom() - barHeight, std::max(1.5f, barWidth - 1.0f), barHeight);
					graphics.FillRectangle(&barBrush, barRect);
					if (normalized > 0.65f)
					{
						RectF glowRect(barRect.X, barRect.Y, barRect.Width, std::min(12.0f, barRect.Height));
						graphics.FillRectangle(&glowBrush, glowRect);
					}
				}

				const RectF leftMeter(layout.visualizer.X, layout.visualizer.Y, layout.visualizer.Width * 0.22f, 6.0f);
				const RectF rightMeter(layout.visualizer.GetRight() - leftMeter.Width, layout.visualizer.Y, leftMeter.Width, 6.0f);
				SolidBrush meterTrack(Color(255, 33, 42, 66));
				SolidBrush meterFill(Color(255, 255, 179, 102));
				graphics.FillRectangle(&meterTrack, leftMeter);
				graphics.FillRectangle(&meterTrack, rightMeter);
				graphics.FillRectangle(&meterFill, RectF(leftMeter.X, leftMeter.Y, leftMeter.Width * ClampValue(levelLeft, 0.0f, 1.0f), leftMeter.Height));
				graphics.FillRectangle(&meterFill, RectF(rightMeter.X, rightMeter.Y, rightMeter.Width * ClampValue(levelRight, 0.0f, 1.0f), rightMeter.Height));
			}
			else
			{
				Pen outline(Color(255, 67, 87, 117), 2.0f);
				outline.SetDashStyle(DashStyleDash);
				GraphicsPath dropPath(FillModeWinding);
				BuildRoundedRect(dropPath, layout.dropZone, 18.0f);
				graphics.DrawPath(&outline, &dropPath);

				Font emptyTitleFont(L"Segoe UI Semibold", 20.0f, FontStyleRegular, UnitPixel);
				Font emptyBodyFont(L"Segoe UI", 13.0f, FontStyleRegular, UnitPixel);
				StringFormat centered;
				centered.SetAlignment(StringAlignmentCenter);
				centered.SetLineAlignment(StringAlignmentCenter);
				SolidBrush emptyTitleBrush(Color(255, 241, 247, 255));
				SolidBrush emptyBodyBrush(Color(255, 142, 156, 180));
				RectF emptyTitleRect(layout.dropZone.X, layout.dropZone.Y + 56.0f, layout.dropZone.Width, 40.0f);
				RectF emptyBodyRect(layout.dropZone.X + 40.0f, layout.dropZone.Y + 112.0f, layout.dropZone.Width - 80.0f, 80.0f);
				graphics.DrawString(L"Drop a V2M file here", -1, &emptyTitleFont, emptyTitleRect, &centered, &emptyTitleBrush);
				graphics.DrawString(L"The player keeps the original Farbrausch V2 synthesis path and supports direct seek, pause, stop, and drag-and-drop playback.", -1, &emptyBodyFont, emptyBodyRect, &centered, &emptyBodyBrush);
			}

			SolidBrush progressTrackBrush(Color(255, 32, 40, 63));
			SolidBrush progressFillBrush(Color(255, 99, 224, 238));
			GraphicsPath progressPath(FillModeWinding);
			BuildRoundedRect(progressPath, layout.progressTrack, 6.0f);
			graphics.FillPath(&progressTrackBrush, &progressPath);

			const float progress = (m_engine.GetDurationMs() > 0)
				? (static_cast<float>(m_engine.GetCurrentMs()) / static_cast<float>(m_engine.GetDurationMs()))
				: 0.0f;
			RectF progressFill = layout.progressTrack;
			progressFill.Width *= ClampValue(progress, 0.0f, 1.0f);
			if (progressFill.Width > 0.0f)
			{
				GraphicsPath fillPath(FillModeWinding);
				BuildRoundedRect(fillPath, progressFill, 6.0f);
				graphics.FillPath(&progressFillBrush, &fillPath);
			}

			SolidBrush handleBrush(Color(255, 255, 245, 224));
			const float handleX = layout.progressTrack.X + layout.progressTrack.Width * ClampValue(progress, 0.0f, 1.0f);
			graphics.FillEllipse(&handleBrush, handleX - 8.0f, layout.progressTrack.Y - 4.0f, 16.0f, 20.0f);

			Font timeFont(L"Consolas", 14.0f, FontStyleRegular, UnitPixel);
			StringFormat timeFormat;
			timeFormat.SetAlignment(StringAlignmentFar);
			SolidBrush timeBrush(Color(255, 230, 237, 247));
			const std::wstring timeText = FormatTimeMs(m_engine.GetCurrentMs()) + L" / " + FormatTimeMs(m_engine.GetDurationMs());
			graphics.DrawString(timeText.c_str(), -1, &timeFont, layout.timeRect, &timeFormat, &timeBrush);

			DrawButton(graphics, layout.openButton);
			DrawButton(graphics, layout.playButton);
			DrawButton(graphics, layout.pauseButton);
			DrawButton(graphics, layout.stopButton);

			Font statusFont(L"Segoe UI", 11.5f, FontStyleRegular, UnitPixel);
			SolidBrush statusBrush(Color(255, 148, 161, 182));
			StringFormat statusFormat;
			statusFormat.SetTrimming(StringTrimmingEllipsisCharacter);
			graphics.DrawString(m_engine.GetStatusText().c_str(), -1, &statusFont, layout.statusRect, &statusFormat, &statusBrush);

			Graphics screen(hdc);
			screen.DrawImage(&backBuffer, 0, 0);
			EndPaint(m_hwnd, &paintStruct);
		}

		HWND m_hwnd = 0;
		HDROP m_dropTarget = 0;
		ULONG_PTR m_gdiplusToken = 0;
		PlayerEngine m_engine;
	};

	PlayerWindow *PlayerWindow::m_app = 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	HRESULT hr = CoInitialize(0);
	if (FAILED(hr))
		return 1;

	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring startupPath;
	if (argv && argc > 1)
		startupPath = argv[1];
	LocalFree(argv);

	std::unique_ptr<PlayerWindow> window(new PlayerWindow());
	if (!window->Create(instance, startupPath))
	{
		CoUninitialize();
		return 1;
	}

	MSG message = {};
	while (GetMessageW(&message, 0, 0, 0))
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	CoUninitialize();
	return static_cast<int>(message.wParam);
}

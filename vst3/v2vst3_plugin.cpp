#include "v2vst3_api.h"
#include "version.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

namespace Steinberg {
namespace Vst {

static const FUID kFarbrauschV2UID(0x3A4ABCF1, 0x09F642CA, 0xA1D8D6BB, 0x0A62C3D7);

namespace
{
	static std::atomic<unsigned long> g_instanceCounter{1};

	static int ClampToMidiRange(float value)
	{
		if (value <= 0.0f)
		{
			return 0;
		}
		if (value >= 1.0f)
		{
			return 127;
		}
		return static_cast<int>(value * 127.0f + 0.5f);
	}

	static std::string GetModuleDirectory()
	{
		HMODULE module = 0;
		if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&GetModuleDirectory),
			&module))
		{
			return std::string();
		}

		char modulePath[MAX_PATH] = {};
		GetModuleFileNameA(module, modulePath, MAX_PATH);

		std::string directory = modulePath;
		const std::string::size_type slash = directory.find_last_of("\\/");
		if (slash != std::string::npos)
		{
			directory.resize(slash);
		}
		return directory;
	}

	static std::string MakeUniqueTempDllPath()
	{
		char tempDirectory[MAX_PATH] = {};
		GetTempPathA(MAX_PATH, tempDirectory);

		const unsigned long counter = g_instanceCounter.fetch_add(1);
		char fileName[MAX_PATH] = {};
		wsprintfA(fileName, "%sFarbrauschV2Helper_%lu_%lu.dll", tempDirectory, GetCurrentProcessId(), counter);
		return fileName;
	}

	static std::u16string ToWide(const std::string &ascii)
	{
		String128 buffer = {};
		UString(buffer, 128).fromAscii(ascii.c_str());
		return std::u16string(buffer);
	}

	class V2WrappedParameter : public RangeParameter
	{
	public:
		V2WrappedParameter(const std::string &title, ParamID tag, const V2PARAM &definition)
		: RangeParameter(ToWide(title).c_str(), tag, nullptr, definition.min, definition.max, definition.min,
			definition.max - definition.min, ParameterInfo::kCanAutomate, kRootUnitId)
		, m_definition(definition)
		{
			if (definition.ctltype == VCTL_MB)
			{
				getInfo().flags |= ParameterInfo::kIsList;
				ParseOptions();
			}
		}

		void toString(ParamValue normalizedValue, String128 string) const SMTG_OVERRIDE
		{
			const int rawValue = static_cast<int>(RangeParameter::toPlain(normalizedValue) + 0.5);
			if (m_definition.ctltype == VCTL_MB && !m_options.empty())
			{
				const int optionIndex = std::clamp(rawValue - m_definition.min, 0, static_cast<int>(m_options.size()) - 1);
				UString(string, 128).fromAscii(m_options[optionIndex].c_str());
				return;
			}

			char buffer[64] = {};
			wsprintfA(buffer, "%d", rawValue - m_definition.offset);
			UString(string, 128).fromAscii(buffer);
		}

	private:
		void ParseOptions()
		{
			const char *text = m_definition.ctlstr ? m_definition.ctlstr : "";
			if (*text == '!')
			{
				++text;
			}

			while (*text)
			{
				const char *separator = strchr(text, '|');
				if (separator)
				{
					m_options.emplace_back(text, separator);
					text = separator + 1;
				}
				else
				{
					m_options.emplace_back(text);
					break;
				}
			}
		}

		V2PARAM m_definition;
		std::vector<std::string> m_options;
	};

	class V2Vst3Plugin;

	class V2PlugView : public CPluginView
	{
	public:
		explicit V2PlugView(V2Vst3Plugin &plugin, const ViewRect &rect);

		tresult PLUGIN_API isPlatformTypeSupported(FIDString type) SMTG_OVERRIDE;
		tresult PLUGIN_API attached(void *parent, FIDString type) SMTG_OVERRIDE;
		tresult PLUGIN_API removed() SMTG_OVERRIDE;

	private:
		V2Vst3Plugin &m_plugin;
		HWND m_child = 0;
	};

	class V2Vst3Plugin : public SingleComponentEffect
	{
	public:
		V2Vst3Plugin() = default;
		~V2Vst3Plugin() SMTG_OVERRIDE
		{
			UnloadHelper();
		}

		static FUnknown *createInstance(void *)
		{
			return static_cast<IAudioProcessor *>(new V2Vst3Plugin());
		}

		tresult PLUGIN_API initialize(FUnknown *context) SMTG_OVERRIDE;
		tresult PLUGIN_API terminate() SMTG_OVERRIDE;
		tresult PLUGIN_API setupProcessing(ProcessSetup &setup) SMTG_OVERRIDE;
		tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE;
		tresult PLUGIN_API process(ProcessData &data) SMTG_OVERRIDE;
		tresult PLUGIN_API setState(IBStream *state) SMTG_OVERRIDE;
		tresult PLUGIN_API getState(IBStream *state) SMTG_OVERRIDE;
		IPlugView *PLUGIN_API createView(FIDString name) SMTG_OVERRIDE;
		tresult PLUGIN_API setEditorState(IBStream *) SMTG_OVERRIDE { return kResultFalse; }
		tresult PLUGIN_API getEditorState(IBStream *) SMTG_OVERRIDE { return kResultFalse; }
		tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) SMTG_OVERRIDE;

		void OnDisplayUpdate()
		{
			SyncParameterObjects();
			if (componentHandler)
			{
				componentHandler->restartComponent(kParamValuesChanged);
			}
		}

		void GetEditorSize(int &width, int &height) const
		{
			width = m_editorWidth;
			height = m_editorHeight;
		}

		HWND AttachEditor(HWND parent)
		{
			return m_editorAttach ? m_editorAttach(parent) : 0;
		}

		void RemoveEditor()
		{
			if (m_editorRemove)
			{
				m_editorRemove();
			}
		}

		OBJ_METHODS(V2Vst3Plugin, SingleComponentEffect)
		REFCOUNT_METHODS(SingleComponentEffect)

	private:
		bool LoadHelper();
		void UnloadHelper();
		void BuildParameters();
		void SyncParameterObjects();
		int GetHostParameterRaw(int index) const;
		void SetHostParameterRaw(int index, int value);
		void QueueEvents(IEventList *events, int sampleFrames);
		void ZeroOutputs(ProcessData &data);

		static void __cdecl DisplayUpdateThunk(void *context)
		{
			if (context)
			{
				static_cast<V2Vst3Plugin *>(context)->OnDisplayUpdate();
			}
		}

		HMODULE m_helperModule = 0;
		std::string m_helperCopyPath;
		std::string m_bankPath;

		V2VST3_INIT_FUNC m_init = 0;
		V2VST3_SHUTDOWN_FUNC m_shutdown = 0;
		V2VST3_SET_SAMPLERATE_FUNC m_setSampleRate = 0;
		V2VST3_QUEUE_MIDI_EVENT_FUNC m_queueMidiEvent = 0;
		V2VST3_PROCESS_REPLACING_FUNC m_processReplacing = 0;
		V2VST3_GET_CHUNK_FUNC m_getChunk = 0;
		V2VST3_SET_CHUNK_FUNC m_setChunk = 0;
		V2VST3_GET_PARAM_DEFS_FUNC m_getParameterDefs = 0;
		V2VST3_GET_HOST_PARAMETER_FUNC m_getHostParameter = 0;
		V2VST3_SET_HOST_PARAMETER_FUNC m_setHostParameter = 0;
		V2VST3_GET_CURRENT_PATCH_FUNC m_getCurrentPatch = 0;
		V2VST3_GET_EDITOR_SIZE_FUNC m_getEditorSize = 0;
		V2VST3_EDITOR_ATTACH_FUNC m_editorAttach = 0;
		V2VST3_EDITOR_REMOVE_FUNC m_editorRemove = 0;
		V2VST3_SET_DISPLAY_CALLBACK_FUNC m_setDisplayCallback = 0;

		V2TOPIC *m_topics = 0;
		V2PARAM *m_parameters = 0;
		V2TOPIC *m_globalTopics = 0;
		V2PARAM *m_globalParameters = 0;
		int m_topicCount = 0;
		int m_parameterCount = 0;
		int m_globalTopicCount = 0;
		int m_globalParameterCount = 0;
		int m_editorWidth = 695;
		int m_editorHeight = 872;
	};

	V2PlugView::V2PlugView(V2Vst3Plugin &plugin, const ViewRect &rect)
	: CPluginView(&rect)
	, m_plugin(plugin)
	{
	}

	tresult PLUGIN_API V2PlugView::isPlatformTypeSupported(FIDString type)
	{
		return (type && strcmp(type, kPlatformTypeHWND) == 0) ? kResultTrue : kResultFalse;
	}

	tresult PLUGIN_API V2PlugView::attached(void *parent, FIDString type)
	{
		if (!parent || strcmp(type, kPlatformTypeHWND) != 0)
		{
			return kResultFalse;
		}

		const tresult result = CPluginView::attached(parent, type);
		if (result != kResultOk)
		{
			return result;
		}

		m_child = m_plugin.AttachEditor(static_cast<HWND>(parent));
		if (!m_child)
		{
			CPluginView::removed();
			return kResultFalse;
		}

		const ViewRect &currentRect = getRect();
		::SetWindowPos(m_child, 0, 0, 0, currentRect.getWidth(), currentRect.getHeight(), SWP_NOZORDER | SWP_SHOWWINDOW);
		return kResultOk;
	}

	tresult PLUGIN_API V2PlugView::removed()
	{
		m_plugin.RemoveEditor();
		m_child = 0;
		return CPluginView::removed();
	}

	bool V2Vst3Plugin::LoadHelper()
	{
		if (m_helperModule)
		{
			return true;
		}

		const std::string moduleDirectory = GetModuleDirectory();
		if (moduleDirectory.empty())
		{
			return false;
		}

		const std::string helperSourcePath = moduleDirectory + "\\FarbrauschV2Helper.dll";
		m_bankPath = moduleDirectory + "\\presets.v2b";
		m_helperCopyPath = MakeUniqueTempDllPath();

		if (!CopyFileA(helperSourcePath.c_str(), m_helperCopyPath.c_str(), FALSE))
		{
			return false;
		}

		m_helperModule = LoadLibraryA(m_helperCopyPath.c_str());
		if (!m_helperModule)
		{
			DeleteFileA(m_helperCopyPath.c_str());
			m_helperCopyPath.clear();
			return false;
		}

		m_init = reinterpret_cast<V2VST3_INIT_FUNC>(GetProcAddress(m_helperModule, "v2vst3Init"));
		m_shutdown = reinterpret_cast<V2VST3_SHUTDOWN_FUNC>(GetProcAddress(m_helperModule, "v2vst3Shutdown"));
		m_setSampleRate = reinterpret_cast<V2VST3_SET_SAMPLERATE_FUNC>(GetProcAddress(m_helperModule, "v2vst3SetSampleRate"));
		m_queueMidiEvent = reinterpret_cast<V2VST3_QUEUE_MIDI_EVENT_FUNC>(GetProcAddress(m_helperModule, "v2vst3QueueMidiEvent"));
		m_processReplacing = reinterpret_cast<V2VST3_PROCESS_REPLACING_FUNC>(GetProcAddress(m_helperModule, "v2vst3ProcessReplacing"));
		m_getChunk = reinterpret_cast<V2VST3_GET_CHUNK_FUNC>(GetProcAddress(m_helperModule, "v2vst3GetChunk"));
		m_setChunk = reinterpret_cast<V2VST3_SET_CHUNK_FUNC>(GetProcAddress(m_helperModule, "v2vst3SetChunk"));
		m_getParameterDefs = reinterpret_cast<V2VST3_GET_PARAM_DEFS_FUNC>(GetProcAddress(m_helperModule, "v2vst3GetParameterDefs"));
		m_getHostParameter = reinterpret_cast<V2VST3_GET_HOST_PARAMETER_FUNC>(GetProcAddress(m_helperModule, "v2vst3GetHostParameter"));
		m_setHostParameter = reinterpret_cast<V2VST3_SET_HOST_PARAMETER_FUNC>(GetProcAddress(m_helperModule, "v2vst3SetHostParameter"));
		m_getCurrentPatch = reinterpret_cast<V2VST3_GET_CURRENT_PATCH_FUNC>(GetProcAddress(m_helperModule, "v2vst3GetCurrentPatchIndex"));
		m_getEditorSize = reinterpret_cast<V2VST3_GET_EDITOR_SIZE_FUNC>(GetProcAddress(m_helperModule, "v2vst3GetEditorSize"));
		m_editorAttach = reinterpret_cast<V2VST3_EDITOR_ATTACH_FUNC>(GetProcAddress(m_helperModule, "v2vst3EditorAttach"));
		m_editorRemove = reinterpret_cast<V2VST3_EDITOR_REMOVE_FUNC>(GetProcAddress(m_helperModule, "v2vst3EditorRemove"));
		m_setDisplayCallback = reinterpret_cast<V2VST3_SET_DISPLAY_CALLBACK_FUNC>(GetProcAddress(m_helperModule, "v2vst3SetDisplayCallback"));

		if (!m_init || !m_shutdown || !m_setSampleRate || !m_queueMidiEvent || !m_processReplacing ||
			!m_getChunk || !m_setChunk || !m_getParameterDefs || !m_getHostParameter || !m_setHostParameter ||
			!m_getCurrentPatch || !m_getEditorSize || !m_editorAttach || !m_editorRemove || !m_setDisplayCallback)
		{
			UnloadHelper();
			return false;
		}

		if (!m_init(m_bankPath.c_str()))
		{
			UnloadHelper();
			return false;
		}

		m_setDisplayCallback(&DisplayUpdateThunk, this);
		m_getParameterDefs(
			&m_topics,
			&m_topicCount,
			&m_parameters,
			&m_parameterCount,
			&m_globalTopics,
			&m_globalTopicCount,
			&m_globalParameters,
			&m_globalParameterCount);
		m_getEditorSize(&m_editorWidth, &m_editorHeight);
		return true;
	}

	void V2Vst3Plugin::UnloadHelper()
	{
		if (!m_helperModule)
		{
			return;
		}

		if (m_setDisplayCallback)
		{
			m_setDisplayCallback(0, 0);
		}

		if (m_editorRemove)
		{
			m_editorRemove();
		}

		if (m_shutdown)
		{
			m_shutdown();
		}

		FreeLibrary(m_helperModule);
		m_helperModule = 0;
		m_init = 0;
		m_shutdown = 0;
		m_setSampleRate = 0;
		m_queueMidiEvent = 0;
		m_processReplacing = 0;
		m_getChunk = 0;
		m_setChunk = 0;
		m_getParameterDefs = 0;
		m_getHostParameter = 0;
		m_setHostParameter = 0;
		m_getCurrentPatch = 0;
		m_getEditorSize = 0;
		m_editorAttach = 0;
		m_editorRemove = 0;
		m_setDisplayCallback = 0;
		m_topics = 0;
		m_parameters = 0;
		m_globalTopics = 0;
		m_globalParameters = 0;
		m_topicCount = 0;
		m_parameterCount = 0;
		m_globalTopicCount = 0;
		m_globalParameterCount = 0;
		m_editorWidth = 0;
		m_editorHeight = 0;
		m_bankPath.clear();

		if (!m_helperCopyPath.empty())
		{
			DeleteFileA(m_helperCopyPath.c_str());
			m_helperCopyPath.clear();
		}
	}

	void V2Vst3Plugin::BuildParameters()
	{
		int paramId = 0;
		for (int topicIndex = 0; topicIndex < m_topicCount; ++topicIndex)
		{
			for (int parameterIndex = 0; parameterIndex < m_topics[topicIndex].no; ++parameterIndex, ++paramId)
			{
				const V2PARAM &definition = m_parameters[paramId];
				const std::string title = std::string(m_topics[topicIndex].name2) + " " + definition.name;
				parameters.addParameter(new V2WrappedParameter(title, paramId, definition));
			}
		}

		for (int topicIndex = 0; topicIndex < m_globalTopicCount; ++topicIndex)
		{
			for (int parameterIndex = 0; parameterIndex < m_globalTopics[topicIndex].no; ++parameterIndex, ++paramId)
			{
				const V2PARAM &definition = m_globalParameters[paramId - m_parameterCount];
				const std::string title = std::string(m_globalTopics[topicIndex].name2) + " " + definition.name;
				parameters.addParameter(new V2WrappedParameter(title, paramId, definition));
			}
		}
	}

	void V2Vst3Plugin::SyncParameterObjects()
	{
		for (int index = 0; index < m_parameterCount + m_globalParameterCount; ++index)
		{
			if (Parameter *parameter = parameters.getParameter(index))
			{
				const V2PARAM &definition = (index < m_parameterCount)
					? m_parameters[index]
					: m_globalParameters[index - m_parameterCount];
				const int rawValue = GetHostParameterRaw(index);
				parameter->setNormalized(v2ParmToNormalized(rawValue, definition.min, definition.max));
			}
		}
	}

	int V2Vst3Plugin::GetHostParameterRaw(int index) const
	{
		return m_getHostParameter ? m_getHostParameter(index) : 0;
	}

	void V2Vst3Plugin::SetHostParameterRaw(int index, int value)
	{
		if (m_setHostParameter)
		{
			m_setHostParameter(index, value);
		}
	}

	void V2Vst3Plugin::QueueEvents(IEventList *events, int sampleFrames)
	{
		if (!events || !m_queueMidiEvent)
		{
			return;
		}

		const int32 eventCount = events->getEventCount();
		for (int32 eventIndex = 0; eventIndex < eventCount; ++eventIndex)
		{
			Event event = {};
			if (events->getEvent(eventIndex, event) != kResultOk)
			{
				continue;
			}

			const unsigned int offset = static_cast<unsigned int>(std::clamp(event.sampleOffset, 0, sampleFrames));
			switch (event.type)
			{
			case Event::kNoteOnEvent:
			{
				const DWORD midiData =
					(static_cast<DWORD>(0x90 | (event.noteOn.channel & 0x0f))) |
					(static_cast<DWORD>(event.noteOn.pitch & 0x7f) << 8) |
					(static_cast<DWORD>(ClampToMidiRange(event.noteOn.velocity)) << 16);
				m_queueMidiEvent(offset, midiData);
				break;
			}

			case Event::kNoteOffEvent:
			{
				const DWORD midiData =
					(static_cast<DWORD>(0x80 | (event.noteOff.channel & 0x0f))) |
					(static_cast<DWORD>(event.noteOff.pitch & 0x7f) << 8) |
					(static_cast<DWORD>(ClampToMidiRange(event.noteOff.velocity)) << 16);
				m_queueMidiEvent(offset, midiData);
				break;
			}

			case Event::kPolyPressureEvent:
			{
				const DWORD midiData =
					(static_cast<DWORD>(0xA0 | (event.polyPressure.channel & 0x0f))) |
					(static_cast<DWORD>(event.polyPressure.pitch & 0x7f) << 8) |
					(static_cast<DWORD>(ClampToMidiRange(event.polyPressure.pressure)) << 16);
				m_queueMidiEvent(offset, midiData);
				break;
			}

			default:
				break;
			}
		}
	}

	void V2Vst3Plugin::ZeroOutputs(ProcessData &data)
	{
		for (int32 outputIndex = 0; outputIndex < data.numOutputs; ++outputIndex)
		{
			AudioBusBuffers &output = data.outputs[outputIndex];
			for (int32 channel = 0; channel < output.numChannels; ++channel)
			{
				if (output.channelBuffers32 && output.channelBuffers32[channel])
				{
					memset(output.channelBuffers32[channel], 0, sizeof(float) * data.numSamples);
				}
			}
		}
	}

	tresult PLUGIN_API V2Vst3Plugin::initialize(FUnknown *context)
	{
		const tresult result = SingleComponentEffect::initialize(context);
		if (result != kResultOk)
		{
			return result;
		}

		addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
		addEventInput(STR16("Event In"), 16);

		if (!LoadHelper())
		{
			return kResultFalse;
		}

		BuildParameters();
		SyncParameterObjects();
		return kResultOk;
	}

	tresult PLUGIN_API V2Vst3Plugin::terminate()
	{
		const tresult result = SingleComponentEffect::terminate();
		UnloadHelper();
		return result;
	}

	tresult PLUGIN_API V2Vst3Plugin::setupProcessing(ProcessSetup &setup)
	{
		const tresult result = SingleComponentEffect::setupProcessing(setup);
		if (result == kResultOk && m_setSampleRate)
		{
			m_setSampleRate(static_cast<int>(setup.sampleRate));
		}
		return result;
	}

	tresult PLUGIN_API V2Vst3Plugin::setActive(TBool)
	{
		return kResultOk;
	}

	tresult PLUGIN_API V2Vst3Plugin::process(ProcessData &data)
	{
		if (!m_processReplacing || data.numSamples <= 0 || data.numOutputs == 0 || !data.outputs[0].channelBuffers32)
		{
			ZeroOutputs(data);
			return kResultOk;
		}

		if (data.inputParameterChanges)
		{
			const int32 changedCount = data.inputParameterChanges->getParameterCount();
			for (int32 changedIndex = 0; changedIndex < changedCount; ++changedIndex)
			{
				if (IParamValueQueue *queue = data.inputParameterChanges->getParameterData(changedIndex))
				{
					const ParamID parameterId = queue->getParameterId();
					ParamValue value = 0.0;
					int32 sampleOffset = 0;
					if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, value) == kResultTrue)
					{
						setParamNormalized(parameterId, value);
					}
				}
			}
		}

		QueueEvents(data.inputEvents, data.numSamples);

		float *outputs[2] =
		{
			data.outputs[0].channelBuffers32[0],
			(data.outputs[0].numChannels > 1) ? data.outputs[0].channelBuffers32[1] : data.outputs[0].channelBuffers32[0]
		};

		if (data.outputs[0].numChannels > 1)
		{
			m_processReplacing(outputs, data.numSamples);
		}
		else
		{
			std::vector<float> mono(data.numSamples, 0.0f);
			float *tempOutputs[2] = { mono.data(), mono.data() };
			m_processReplacing(tempOutputs, data.numSamples);
			memcpy(outputs[0], mono.data(), sizeof(float) * data.numSamples);
		}

		return kResultOk;
	}

	tresult PLUGIN_API V2Vst3Plugin::setState(IBStream *state)
	{
		if (!state || !m_setChunk)
		{
			return kResultFalse;
		}

		std::vector<char> buffer;
		buffer.reserve(131072);

		char temp[4096];
		int32 bytesRead = 0;
		while (state->read(temp, static_cast<int32>(sizeof(temp)), &bytesRead) == kResultOk && bytesRead > 0)
		{
			buffer.insert(buffer.end(), temp, temp + bytesRead);
		}

		if (!buffer.empty())
		{
			m_setChunk(buffer.data(), static_cast<long>(buffer.size()));
			SyncParameterObjects();
		}

		return kResultOk;
	}

	tresult PLUGIN_API V2Vst3Plugin::getState(IBStream *state)
	{
		if (!state || !m_getChunk)
		{
			return kResultFalse;
		}

		void *chunkData = 0;
		const long chunkSize = m_getChunk(&chunkData);
		if (!chunkData || chunkSize <= 0)
		{
			return kResultFalse;
		}

		int32 bytesWritten = 0;
		return state->write(chunkData, chunkSize, &bytesWritten);
	}

	IPlugView *PLUGIN_API V2Vst3Plugin::createView(FIDString name)
	{
		if (!name || strcmp(name, ViewType::kEditor) != 0)
		{
			return 0;
		}

		int width = 0;
		int height = 0;
		GetEditorSize(width, height);
		ViewRect rect(0, 0, width, height);
		return new V2PlugView(*this, rect);
	}

	tresult PLUGIN_API V2Vst3Plugin::setParamNormalized(ParamID tag, ParamValue value)
	{
		if (tag < 0 || tag >= (m_parameterCount + m_globalParameterCount))
		{
			return kResultFalse;
		}

		const V2PARAM &definition = (tag < m_parameterCount)
			? m_parameters[tag]
			: m_globalParameters[tag - m_parameterCount];
		const int rawValue = v2NormalizedToParm(value, definition.min, definition.max);

		SetHostParameterRaw(static_cast<int>(tag), rawValue);
		if (Parameter *parameter = parameters.getParameter(tag))
		{
			parameter->setNormalized(value);
		}
		return kResultOk;
	}
}

} // namespace Vst
} // namespace Steinberg

BEGIN_FACTORY_DEF(stringCompanyName, stringCompanyWeb, stringCompanyEmail)

	DEF_CLASS2(
		INLINE_UID_FROM_FUID(Steinberg::Vst::kFarbrauschV2UID),
		PClassInfo::kManyInstances,
		kVstAudioEffectClass,
		"Farbrausch V2",
		0,
		"Instrument|Synth",
		FULL_VERSION_STR,
		kVstVersionString,
		Steinberg::Vst::V2Vst3Plugin::createInstance)

END_FACTORY

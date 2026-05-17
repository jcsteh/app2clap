/*
 * App2Clap
 * App2Clap plug-in code
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2025-2026 James Teh
 * License: GNU General Public License version 2.0
 */

#include "common.h"

#include <atlcomcli.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "circular_buffer.h"
#include "clap/helpers/plugin.hxx"

#include "resource.h"

constexpr DWORD IDLE_PID = 0;
constexpr DWORD SYSTEM_PID = 4;

class ActivateCompletionHandler : public IActivateAudioInterfaceCompletionHandler {
	public:
	ActivateCompletionHandler() {
		this->_event = CreateEvent(nullptr, true, false, nullptr);
	}

	void wait() {
		WaitForSingleObject(this->_event, 5000);
	}

	// IUnknown
	ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }

	HRESULT STDMETHODCALLTYPE QueryInterface(_In_ REFIID riid,
		_Outptr_ void** ppInterface
	) override {
		*ppInterface = this;
		return S_OK;
	}

	// IActivateAudioInterfaceCompletionHandler
	HRESULT ActivateCompleted(IActivateAudioInterfaceAsyncOperation* activateOperation) override {
		SetEvent(this->_event);
		return S_OK;
	}

	private:
	AutoHandle _event;
};

struct Process {
	FILETIME creationTime;
	DWORD pid;
	std::wstring exe;
	std::wstring desc;

	Process(const PROCESSENTRY32& entry):
	pid(entry.th32ProcessID), exe(entry.szExeFile) {
		FILETIME exitTime, kernelTime, userTime;
		AutoHandle process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION , false,
			this->pid);
		GetProcessTimes(process, &this->creationTime, &exitTime, &kernelTime,
			&userTime);
		std::wostringstream s;
		s << this->exe << " " << this->pid;
		this->desc = s.str();
	}

	bool operator<(const Process& other) {
		// Order by creation time.
		return CompareFileTime(&this->creationTime, &other.creationTime) == -1;
	}
};

const uint32_t STATE_VERSION = 2;

class App2Clap : public BasePlugin {
	public:
	App2Clap(const clap_plugin_descriptor* desc, const clap_host* host)
		: BasePlugin(desc, host) {}

	protected:
	bool implementsAudioPorts() const noexcept override { return true; }

	uint32_t audioPortsCount(bool isInput) const noexcept override {
		return isInput ? 0 : 1;
	}

	bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info *info) const noexcept override {
		if (isInput || index > 0) {
			return false;
		}
		info->id = 0;
		info->channel_count = NUM_CHANNELS;
		info->flags = CLAP_AUDIO_PORT_IS_MAIN;
		info->port_type = CLAP_PORT_STEREO;
		info->in_place_pair = CLAP_INVALID_ID;
		snprintf(info->name, sizeof(info->name), "Main");
		return true;
	}

	bool activate(double sampleRate, uint32_t minFrameCount, uint32_t maxFrameCount) noexcept override {
		if (!this->_pid) {
			if (!this->_captureFirstMatching || this->_filter.empty()) {
				// Nothing to capture yet.
				return false;
			}
			// We're capturing the first matching process.
			this->buildProcessList();
			if (this->_processes.empty()) {
				// No matching processes.
				return false;
			}
			this->_pid = this->_processes[0].pid;
		}
		AUDIOCLIENT_ACTIVATION_PARAMS params = {
			.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
		};
		params.ProcessLoopbackParams.TargetProcessId = this->_pid;
		params.ProcessLoopbackParams.ProcessLoopbackMode =
			this->_include ?
			PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE :
			PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
		PROPVARIANT propvar = { .vt = VT_BLOB };
		propvar.blob.cbSize = sizeof(params);
		propvar.blob.pBlobData = (BYTE*)&params;
		auto getClient = [&propvar] () -> CComPtr<IAudioClient> {
			auto completion = std::make_unique<ActivateCompletionHandler>();
			CComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
			HRESULT hr = ActivateAudioInterfaceAsync(
				VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
				__uuidof(IAudioClient),
				&propvar,
				completion.get(),
				&asyncOp
			);
			if (FAILED(hr)) {
				return nullptr;
			}
			completion->wait();
			HRESULT asyncHr;
			CComPtr<IUnknown> activated;
			hr = asyncOp->GetActivateResult(&asyncHr, &activated);
			if (FAILED(hr) || FAILED(asyncHr)) {
				return nullptr;
			}
			return CComQIPtr<IAudioClient>(activated);
		};
		this->_client = getClient();
		if (!this->_client) {
			return false;
		}
		WAVEFORMATEX format = {
			.wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
			.nChannels = NUM_CHANNELS,
			.nSamplesPerSec = (DWORD)sampleRate,
			.nAvgBytesPerSec = (DWORD)sampleRate * BYTES_PER_FRAME,
			.nBlockAlign = BYTES_PER_FRAME,
			.wBitsPerSample = BITS_PER_SAMPLE,
		};
		// Contrary to the documentation, IAudioClient::Initialize ignores the buffer
		// duration here and can return a smaller buffer. We provide it anyway, but
		// it can't be relied upon.
		const REFERENCE_TIME bufferDuration = (REFERENCE_TIME)maxFrameCount *
			REFTIMES_PER_SEC / sampleRate;
		HRESULT hr = this->_client->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
			AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			bufferDuration, 0, &format, nullptr
		);
		if (FAILED(hr)) {
			return false;
		}
		UINT32 bufferSize = 0;
		this->_client->GetBufferSize(&bufferSize);
		if (FAILED(hr)) {
			return false;
		}
		AutoHandle event;
		if (bufferSize * 3 < maxFrameCount) {
			// Windows will only buffer 3 packets at a time. If the host max frame
			// count is larger than that, capture audio in a background thread to
			// avoid continual buffer underruns. Note that the thread is less optimal
			// (and results in glitches) when the host max frame count is lower.
			this->_client = getClient();
			if (!this->_client) {
				return false;
			}
			hr = this->_client->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
				AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				bufferDuration, 0, &format, nullptr
			);
			if (FAILED(hr)) {
				return false;
			}
			event = CreateEvent(nullptr, false, false, nullptr);
			hr = this->_client->SetEventHandle(event);
			if (FAILED(hr)) {
				return false;
			}
		}
		dbg(
			"activate: maxFrameCount " << maxFrameCount <<
			" sampleRate " << sampleRate <<
			" requested bufferDuration " << bufferDuration <<
			" received bufferSize " << bufferSize <<
			" threaded " << (bool)event
		);
		hr = this->_client->GetService(__uuidof(IAudioCaptureClient), (void**)&this->_capture);
		if (FAILED(hr)) {
			return false;
		}
		if (event) {
			this->_captureEvent =std::move(event);
			this->_captureThread = std::thread([this] {
				this->_captureThreadFunc();
			});
		}
		this->_client->Start();
		return true;
	}

	void deactivate() noexcept  override {
		if (!this->_client) {
			return;
		}
		this->_client->Stop();
		this->_client = nullptr;
		if (this->_captureEvent) {
			// Signal the capture thread to exit.
			SetEvent(this->_captureEvent);
			this->_captureThread.join();
			this->_captureEvent = nullptr;
		}
		this->_capture = nullptr;
	}

	clap_process_status process(const clap_process *process) noexcept override {
		if (!this->_capture) {
			return CLAP_PROCESS_SLEEP;
		}
		if (!this->_captureEvent) {
			// We aren't using a background thread to capture audio, so capture here.
			// There might be multiple packets ready to capture.
			while (this->_buffer.size() < process->frames_count && this->_doCapture()) {}
		}
		dbg(
			"process: frames_count " << process->frames_count <<
			" buffer size " << this->_buffer.size()
		);
		if (this->_buffer.size() < process->frames_count) {
			return CLAP_PROCESS_CONTINUE;
		}
		for (uint32_t f = 0; f < process->frames_count; ++f) {
			std::tie(
				process->audio_outputs[0].data32[0][f],
				process->audio_outputs[0].data32[1][f]
			) = this->_buffer.front();
			this->_buffer.pop_front();
		}
		return CLAP_PROCESS_CONTINUE;
	}

	bool implementsGui() const noexcept override { return true; }

	bool guiIsApiSupported(const char* api, bool isFloating) noexcept override {
		return strcmp(api, CLAP_WINDOW_API_WIN32) == 0 && !isFloating;
	}

	bool guiGetPreferredApi(const char** api, bool* is_floating) noexcept override {
		*api = CLAP_WINDOW_API_WIN32;
		*is_floating = false;
		return true;
	}

	bool guiCreate(const char *api, bool isFloating) noexcept override {
		// We create the GUI in guiSetParent below.
		return true;
	}

	void guiDestroy() noexcept override {
		DestroyWindow(this->_dialog);
		this->_dialog = this->_processCombo = nullptr;
	}

	bool guiShow() noexcept override {
		return guiShowCommon(this->_dialog);
	}

	bool guiHide() noexcept override {
		ShowWindow(this->_dialog, SW_HIDE);
		return true;
	}

	bool guiSetParent(const clap_window* window) noexcept override {
		// We create the dialog here instead of guiCreate because CreateDialog needs
		// the parent HWND in order to make a DS_CHILD dialog.
		this->_dialog = createDialog((HWND)window->win32, ID_APP2CLAP_DLG,
			App2Clap::dialogProc);
		SetWindowLongPtr(this->_dialog, GWLP_USERDATA, (LONG_PTR)this);
		this->_processCombo = GetDlgItem(this->_dialog, ID_PROCESS);
		this->buildProcessList();
		if (this->_pid == SYSTEM_PID) {
			CheckDlgButton(this->_dialog, ID_EVERYTHING, BST_CHECKED);
			this->enableProcessChoice(false);
		} else {
			CheckDlgButton(
				this->_dialog,
				this->_include ? ID_PROCESS_INCLUDE : ID_PROCESS_EXCLUDE,
				BST_CHECKED
			);
			this->enableProcessChoice(true);
			SetDlgItemText(this->_dialog, ID_FILTER, this->_filter.c_str());
			CheckDlgButton(this->_dialog, ID_FIRST,
				this->_captureFirstMatching ? BST_CHECKED : BST_UNCHECKED);
		}
		return true;
	}

	bool implementsState() const noexcept override { return true; }

	bool stateSave(const clap_ostream* stream) noexcept override {
		stream->write(stream, &STATE_VERSION, sizeof(uint32_t));
		stream->write(stream, &this->_include, sizeof(bool));
		const bool everything = this->_pid == SYSTEM_PID;
		stream->write(stream, &everything, sizeof(bool));
		const size_t nBytes = this->_filter.size() * sizeof(wchar_t);
		stream->write(stream, &nBytes, sizeof(size_t));
		const wchar_t* filter = this->_filter.c_str();
		stream->write(stream, filter, nBytes);
		stream->write(stream, &this->_captureFirstMatching, sizeof(bool));
		return true;
	}

	bool stateLoad(const clap_istream* stream) noexcept override {
		uint32_t version = 0;
		stream->read(stream, &version, sizeof(uint32_t));
		if (version != STATE_VERSION) {
			return false;
		}
		stream->read(stream, &this->_include, sizeof(bool));
		bool everything = false;
		stream->read(stream, &everything, sizeof(bool));
		if (everything) {
			this->_pid = SYSTEM_PID;
		}
		size_t nBytes = 0;
		stream->read(stream, &nBytes, sizeof(size_t));
		if (nBytes > 0) {
			const size_t nChars = nBytes / sizeof(wchar_t);
			auto filter = std::make_unique<wchar_t[]>(nChars);
			stream->read(stream, filter.get(), nBytes);
			this->_filter = std::wstring(filter.get(), nChars);
		}
		stream->read(stream, &this->_captureFirstMatching, sizeof(bool));
		// Restart the plugin. We will set up the send in activate().
		this->_host.host()->request_restart(this->_host.host());
		return true;
	}

	private:
	static INT_PTR CALLBACK dialogProc(HWND dialogHwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		if (dialogProcCommon(dialogHwnd, msg)) {
			return TRUE;
		}
		auto* plugin = (App2Clap*)GetWindowLongPtr(dialogHwnd, GWLP_USERDATA);
		if (msg == WM_COMMAND) {
			const WORD cid = LOWORD(wParam);
			if (cid == ID_PROCESS_INCLUDE || cid == ID_PROCESS_EXCLUDE || cid == ID_EVERYTHING) {
				plugin->enableProcessChoice(!IsDlgButtonChecked(dialogHwnd, ID_EVERYTHING));
				return TRUE;
			}
			if (cid == ID_FIRST) {
				plugin->_captureFirstMatching = IsDlgButtonChecked(dialogHwnd,
					ID_FIRST);
				return TRUE;
			}
			if (cid == ID_FILTER && HIWORD(wParam) == EN_KILLFOCUS) {
				wchar_t rawFilter[100];
				GetDlgItemText(dialogHwnd, ID_FILTER, rawFilter, _countof(rawFilter));
				plugin->_filter = rawFilter;
				// We want to match case insensitively, so convert to lower case.
				std::transform(plugin->_filter.begin(), plugin->_filter.end(),
					plugin->_filter.begin(), std::tolower);
				plugin->buildProcessList();
				return TRUE;
			}
			if (cid == ID_REFRESH) {
				plugin->buildProcessList();
				return TRUE;
			}
			if (cid == ID_CAPTURE) {
				if (IsDlgButtonChecked(dialogHwnd, ID_EVERYTHING)) {
					plugin->_pid = SYSTEM_PID;
					plugin->_include = false;
				} else {
					const int choice = ComboBox_GetCurSel(plugin->_processCombo);
					if (choice == CB_ERR) {
						plugin->_pid = 0;
					} else {
						plugin->_pid = plugin->_processes[choice].pid;
					}
					plugin->_include = IsDlgButtonChecked(dialogHwnd, ID_PROCESS_INCLUDE);
				}
				// Restart the plugin. We will set up the capture in activate().
				plugin->_host.host()->request_restart(plugin->_host.host());
				return TRUE;
			}
		}
		return FALSE;
	}

	void buildProcessList() {
		PROCESSENTRY32 entry;
		entry.dwSize = sizeof(PROCESSENTRY32);
		AutoHandle snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (!Process32First(snapshot, &entry)) {
			return;
		}
		DWORD chosenPid = this->_pid;
		if (this->_processCombo) {
			const int choice = ComboBox_GetCurSel(this->_processCombo);
			if (choice != CB_ERR) {
				chosenPid = this->_processes[choice].pid;
			}
			ComboBox_ResetContent(this->_processCombo);
		}
		this->_processes.clear();
		do {
			if (entry.th32ProcessID == IDLE_PID || entry.th32ProcessID == SYSTEM_PID) {
				continue;
			}
			Process process(entry);
			if (!this->_filter.empty()) {
				// Convert to lower case for match.
				std::wstring lower = process.desc;
				std::transform(lower.begin(), lower.end(), lower.begin(), std::tolower);
				if (lower.find(this->_filter) == std::string::npos) {
					continue;
				}
			}
			this->_processes.push_back(std::move(process));
		} while (Process32Next(snapshot, &entry));
		// Sort processes by creation time so parent processes always appear first.
		std::sort(this->_processes.begin(), this->_processes.end());
		if (!this->_processCombo) {
			return;
		}
		for (const auto& process: this->_processes) {
			ComboBox_AddString(this->_processCombo, process.desc.c_str());
			if (process.pid == chosenPid) {
				// Select the previously chosen process.
				ComboBox_SetCurSel(this->_processCombo,
					ComboBox_GetCount(this->_processCombo) - 1);
			}
		}
	}

	bool _doCapture() {
		UINT32 numFrames; // The number of captured frames.
		// GetNextPacketSize and GetBuffer should return the same number of frames.
		// The documentation doesn't say that GetNextPacketSize is required.
		// However, if you don't call it first and there is no packet to retrieve,
		// GetBuffer succeeds even though it returns a packet with bogus data.
		HRESULT hr = this->_capture->GetNextPacketSize(&numFrames);
		if (FAILED(hr)) {
			return false;
		}
		if (numFrames == 0) {
			return false;
		}
		DWORD flags;
		BYTE* data;
		hr = this->_capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
		if (FAILED(hr) || numFrames == 0) {
			return false;
		}
		dbg("_doCapture: captured " << numFrames << " frames");
		for (UINT32 f = 0; f < numFrames; ++f) {
			std::pair<float, float> frame;
			memcpy(&frame, data, BYTES_PER_FRAME);
			this->_buffer.push_back(frame);
			data += BYTES_PER_FRAME;
		}
		this->_capture->ReleaseBuffer(numFrames);
		return true;
	}

	void _captureThreadFunc() {
		for (; ;) {
			WaitForSingleObject(this->_captureEvent, INFINITE);
			if (!this->_client) {
				return;
			}
			this->_doCapture();
			dbg("thread: size after capture " << this->_buffer.size());
		}
	}

	void enableProcessChoice(bool enable) {
		EnableWindow(this->_processCombo, enable);
		EnableWindow(GetDlgItem(this->_dialog, ID_FILTER), enable);
		EnableWindow(GetDlgItem(this->_dialog, ID_REFRESH), enable);
	}

	CComPtr<IAudioClient> _client;
	CComPtr<IAudioCaptureClient> _capture;
	// A buffer to store audio we've captured but not yet sent to the host.
	using Buffer = CircularBuffer<std::pair<float, float>>;
	// Windows sometimes returns a much smaller buffer size than the size of the
	// packets it subsequently returns. Since we can't trust that, just use a
	// large constant buffer size.
	Buffer _buffer{24576};
	HWND _dialog = nullptr;
	HWND _processCombo = nullptr;
	// The string by which to filter processes.
	std::wstring _filter;
	// The processes we have found.
	std::vector<Process> _processes;
	// The chosen pid.
	DWORD _pid = 0;
	// Whether to include audio from this pid or exclude audio from this pid.
	bool _include = true;
	// Whether to capture the first matching process when reloaded.
	bool _captureFirstMatching = false;
	std::thread _captureThread;
	AutoHandle _captureEvent;
};

extern const clap_plugin_descriptor app2ClapDescriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "jantrid.app2clap",
	.name = "App2Clap",
	.vendor = "James Teh",
	.url = "",
	.manual_url = "",
	.support_url = "",
	.version = "2026.1",
	.description = "",
	.features = (const char *[]) {
		CLAP_PLUGIN_FEATURE_STEREO,
		NULL,
	}
};

const clap_plugin* createApp2Clap(const clap_host* host) {
	auto plugin = new App2Clap(&app2ClapDescriptor, host);
	return plugin->clapPlugin();
}

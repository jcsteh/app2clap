/*
 * App2Clap
 * App2Clap plug-in code
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2025 James Teh
 * License: GNU General Public License version 2.0
 */

// Avoid min macro conflict with std::min.
#define NOMINMAX

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

#include "common.h"
#include "resource.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISDLL ((HINSTANCE)&__ImageBase)

constexpr DWORD IDLE_PID = 0;
constexpr DWORD SYSTEM_PID = 4;

class AutoHandle {
	public:
	AutoHandle(): _handle(nullptr) {}
	AutoHandle(HANDLE handle): _handle(handle) {}
	AutoHandle(AutoHandle& handle) = delete;

	~AutoHandle() {
		if (this->_handle) {
			CloseHandle(this->_handle);
		}
	}

	AutoHandle& operator=(HANDLE newHandle) {
		if (this->_handle) {
			CloseHandle(this->_handle);
		}
		this->_handle = newHandle;
		return *this;
	}

	// Don't allow copy assignment, since the other AutoHandle will close the
	// handle when it is destroyed.
	AutoHandle& operator=(const AutoHandle& newHandle) = delete;

	AutoHandle& operator=(AutoHandle&& newHandle) {
		this->_handle = newHandle._handle;
		newHandle._handle = nullptr;
		return *this;
	}

	operator HANDLE() {
		return this->_handle;
	}

	private:
	HANDLE _handle;
};

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

using BasePlugin = clap::helpers::Plugin<
	clap::helpers::MisbehaviourHandler::Ignore,
	clap::helpers::CheckingLevel::None
>;
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
		DWORD pid = this->getChosenPid();
		if (!pid) {
			return false;
		}
		AUDIOCLIENT_ACTIVATION_PARAMS params = {
			.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
		};
		params.ProcessLoopbackParams.TargetProcessId = pid;
		params.ProcessLoopbackParams.ProcessLoopbackMode =
			IsDlgButtonChecked(this->_dialog, ID_PROCESS_INCLUDE) ?
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
		UINT32 bufferSize;
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
		hr = this->_client->GetService(__uuidof(IAudioCaptureClient), (void**)&this->_capture);
		if (FAILED(hr)) {
			return false;
		}
		this->_buffer = Buffer(std::max(bufferSize, maxFrameCount) * 2);
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
	}

	bool guiShow() noexcept override {
		ShowWindow(this->_dialog, SW_SHOW);
		return true;
	}

	bool guiHide() noexcept override {
		ShowWindow(this->_dialog, SW_HIDE);
		return true;
	}

	bool guiSetParent(const clap_window* window) noexcept override {
		// We create the dialog here instead of guiCreate because CreateDialog needs
		// the parent HWND in order to make a DS_CHILD dialog.
		this->_dialog = CreateDialog(
			HINST_THISDLL, MAKEINTRESOURCE(ID_APP2CLAP_DLG),
			// hack: Use the grandparent so tabbing works in REAPER.
			GetParent((HWND)window->win32), App2Clap::dialogProc
		);
		SetWindowLongPtr(this->_dialog, GWLP_USERDATA, (LONG_PTR)this);
		this->_processCombo = GetDlgItem(this->_dialog, ID_PROCESS);
		this->buildProcessList();
		CheckDlgButton(this->_dialog, ID_PROCESS_INCLUDE, BST_CHECKED);
		return true;
	}

	private:
	static INT_PTR CALLBACK dialogProc(HWND dialogHwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		auto* plugin = (App2Clap*)GetWindowLongPtr(dialogHwnd, GWLP_USERDATA);
		if (msg == WM_COMMAND) {
			const WORD cid = LOWORD(wParam);
			if (cid == ID_PROCESS_INCLUDE || cid == ID_PROCESS_EXCLUDE || cid == ID_EVERYTHING) {
				const bool enable = cid != ID_EVERYTHING;
				EnableWindow(plugin->_processCombo, enable);
				EnableWindow(GetDlgItem(plugin->_dialog, ID_FILTER), enable);
				EnableWindow(GetDlgItem(plugin->_dialog, ID_REFRESH), enable);
				return TRUE;
			}
			if (
				cid == ID_REFRESH ||
				(cid == ID_FILTER && HIWORD(wParam) == EN_KILLFOCUS)
			) {
				plugin->buildProcessList();
				return TRUE;
			}
			if (cid == ID_CAPTURE) {
				// Restart the plugin. We will set up the capture in activate().
				plugin->_host.host()->request_restart(plugin->_host.host());
				return TRUE;
			}
		}
		return FALSE;
	}

	void buildProcessList() {
		char rawFilter[100];
		GetDlgItemText(this->_dialog, ID_FILTER, rawFilter, sizeof(rawFilter));
		std::string filter = rawFilter;
		// We want to match case insensitively, so convert to lower case.
		std::transform(filter.begin(), filter.end(), filter.begin(), std::tolower);
		PROCESSENTRY32 entry;
		entry.dwSize = sizeof(PROCESSENTRY32);
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (!Process32First(snapshot, &entry)) {
			CloseHandle(snapshot);
			return;
		}
		DWORD chosenPid = this->getChosenPid();
		this->_pids.clear();
		ComboBox_ResetContent(this->_processCombo);
		do {
			if (entry.th32ProcessID == IDLE_PID || entry.th32ProcessID == SYSTEM_PID) {
				continue;
			}
			std::ostringstream s;
			s << entry.szExeFile << " " << entry.th32ProcessID;
			bool include = filter.empty();
			if (!include) {
				// Convert to lower case for match.
				std::string lower = s.str();
				std::transform(lower.begin(), lower.end(), lower.begin(), std::tolower);
				include = lower.find(filter) != std::string::npos;
			}
			if (include) {
				ComboBox_AddString(this->_processCombo, s.str().c_str());
				if (entry.th32ProcessID == chosenPid) {
					// Select the previously chosen process.
					ComboBox_SetCurSel(this->_processCombo, this->_pids.size());
				}
				this->_pids.push_back(entry.th32ProcessID);
			}
		} while (Process32Next(snapshot, &entry));
		CloseHandle(snapshot);
	}

	DWORD getChosenPid() const {
		if (!this->_processCombo) {
			// The GUI isn't initialised yet.
			return 0;
		}
		if (IsDlgButtonChecked(this->_dialog, ID_EVERYTHING)) {
			return SYSTEM_PID;
		}
		const int choice = ComboBox_GetCurSel(this->_processCombo);
		if (choice == CB_ERR) {
			// The user hasn't chosen a process yet.
			return 0;
		}
		return this->_pids[choice];
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
		}
	}

	CComPtr<IAudioClient> _client;
	CComPtr<IAudioCaptureClient> _capture;
	// A buffer to store audio we've captured but not yet sent to the host.
	using Buffer = CircularBuffer<std::pair<float, float>>;
	Buffer _buffer{0};
	HWND _dialog = nullptr;
	HWND _processCombo = nullptr;
	// The process ids we have found.
	std::vector<DWORD> _pids;
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
	.version = "2025.1",
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

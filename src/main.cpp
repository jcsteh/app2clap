/*
 * App2Clap
 * Main plug-in code
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

#include "clap/helpers/plugin.hxx"

#include "resource.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISDLL ((HINSTANCE)&__ImageBase)

constexpr DWORD IDLE_PID = 0;
constexpr DWORD SYSTEM_PID = 4;

class ActivateCompletionHandler : public IActivateAudioInterfaceCompletionHandler {
	public:
	ActivateCompletionHandler() {
		this->_event = CreateEvent(nullptr, true, false, nullptr);
	}

	~ActivateCompletionHandler() {
		CloseHandle(this->_event);
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
	HANDLE _event;
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
		info->channel_count = 2;
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
			return false;
		}
		completion->wait();
		HRESULT asyncHr;
		CComPtr<IUnknown> activated;
		hr = asyncOp->GetActivateResult(&asyncHr, &activated);
		if (FAILED(hr) || FAILED(asyncHr)) {
			return false;
		}
		CComQIPtr<IAudioClient> client(activated);
		if (!client) {
			return false;
		}
		this->_client = client;
		WAVEFORMATEX format = {
			.wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
			.nChannels = 2,
			.nSamplesPerSec = (DWORD)sampleRate,
			.nAvgBytesPerSec = (DWORD)sampleRate * 8,
			.nBlockAlign = 8,
			.wBitsPerSample = 32,
		};
		hr = client->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			0, 0, &format, nullptr
		);
		if (FAILED(hr)) {
			return false;
		}
		hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&this->_capture);
		if (FAILED(hr)) {
			return false;
		}
		client->Start();
		return true;
	}

	void deactivate() noexcept  override {
		if (!this->_client) {
			return;
		}
		this->_client->Stop();
		this->_capture = nullptr;
		this->_client = nullptr;
	}

	clap_process_status process(const clap_process *process) noexcept override {
		if (!this->_capture) {
			return CLAP_PROCESS_SLEEP;
		}
		BYTE* data;
		uint32_t f = 0; // How many frames we've sent to the host.
		if (!this->_buffer.empty()) {
			// There's stuff left in the capture buffer from last time. Push that first.
			data = &this->_buffer[this->_bufferConsumed];
			while (f < process->frames_count) {
				memcpy(
					&process->audio_outputs[0].data32[0][f],
					data, sizeof(float)
				);
				data += sizeof(float);
				memcpy(
					&process->audio_outputs[0].data32[1][f],
					data, sizeof(float)
				);
				data += sizeof(float);
				++f;
				this->_bufferConsumed += 2 * sizeof(float);
				if (this->_bufferConsumed >= this->_buffer.size()) {
					// We've exhausted the buffer. Clear it.
					this->_buffer.clear();
					this->_bufferConsumed = 0;
					break;
				}
			}
			if (f >= process->frames_count) {
				// The host can't handle any more frames.
				return CLAP_PROCESS_CONTINUE;
			}
		}

		while (f < process->frames_count) {
			// Capture audio from Windows. We keep doing this until the host can't
			// handle any more frames.
			UINT32 numFrames; // The number of captured frames.
			DWORD flags;
			HRESULT hr = this->_capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
			if (FAILED(hr) || numFrames == 0) {
				return CLAP_PROCESS_CONTINUE;
			}
			uint32_t cf = 0; // How many captured frames we've consumed.
			for (; f < process->frames_count && cf < numFrames; ++f, ++cf) {
				memcpy(
					&process->audio_outputs[0].data32[0][f],
					data, sizeof(float)
				);
				data += sizeof(float);
				memcpy(
					&process->audio_outputs[0].data32[1][f],
					data, sizeof(float)
				);
				data += sizeof(float);
			}
			if (cf < numFrames) {
				// The host can't handle any more frames, but we still have captured frames
				// we haven't pushed. Store them in our buffer for next time.
				this->_buffer.insert(
					this->_buffer.end(),
					data, data + 2 * sizeof(float) * (numFrames - cf)
				);
			}
			this->_capture->ReleaseBuffer(numFrames);
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
			HINST_THISDLL, MAKEINTRESOURCE(ID_MAIN_DLG),
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
				EnableWindow(GetDlgItem(plugin->_dialog, ID_REFRESH), enable);
				return TRUE;
			}
			if (cid == ID_REFRESH) {
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
		PROCESSENTRY32 entry;
		entry.dwSize = sizeof(PROCESSENTRY32);
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (!Process32First(snapshot, &entry)) {
			CloseHandle(snapshot);
			return;
		}
		this->_pids.clear();
		ComboBox_ResetContent(this->_processCombo);
		do {
			if (entry.th32ProcessID == IDLE_PID || entry.th32ProcessID == SYSTEM_PID) {
				continue;
			}
			std::ostringstream s;
			this->_pids.push_back(entry.th32ProcessID);
			s << entry.szExeFile << " " << entry.th32ProcessID;
			ComboBox_AddString(this->_processCombo, s.str().c_str());
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

	CComPtr<IAudioClient> _client;
	CComPtr<IAudioCaptureClient> _capture;
	// A buffer to store audio we've captured but not yet sent to the host.
	std::vector<BYTE> _buffer;
	// How many bytes of _buffer we have already consumed.
	size_t _bufferConsumed = 0;
	HWND _dialog = nullptr;
	HWND _processCombo = nullptr;
	// The process ids we have found.
	std::vector<DWORD> _pids;
};

static const clap_plugin_descriptor descriptor = {
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

static const clap_plugin_factory factory = {
	.get_plugin_count = [] (const clap_plugin_factory* factory) -> uint32_t {
		return 1;
	},
	.get_plugin_descriptor = [] (const clap_plugin_factory* factory, uint32_t index) -> const clap_plugin_descriptor* {
		return index == 0 ? &descriptor : nullptr;
	},
	.create_plugin = [] (const clap_plugin_factory* factory, const clap_host* host, const char *pluginID) -> const clap_plugin* {
		if (strcmp(pluginID, descriptor.id) == 0) {
			auto plugin = new App2Clap(&descriptor, host);
			return plugin->clapPlugin();
		}
		return nullptr;
	},
};

CLAP_EXPORT const clap_plugin_entry clap_entry = {
	.clap_version = CLAP_VERSION,
	.init = [] (const char *path) -> bool { return true; },
	.deinit = [] () {},
	.get_factory = [] (const char *factoryID) -> const void * {
		return strcmp(factoryID, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr;
	},
};

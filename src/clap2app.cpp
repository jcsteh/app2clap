/*
 * App2Clap
 * Clap2App plug-in code
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2025 James Teh
 * License: GNU General Public License version 2.0
 */

// Avoid min macro conflict with std::min.
#define NOMINMAX
#define UNICODE

#include <atlcomcli.h>
#include <audioclient.h>
#include <functiondiscoverykeys.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "clap/helpers/plugin.hxx"

#include "common.h"
#include "resource.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISDLL ((HINSTANCE)&__ImageBase)

const uint32_t STATE_VERSION = 1;

using BasePlugin = clap::helpers::Plugin<
	clap::helpers::MisbehaviourHandler::Ignore,
	clap::helpers::CheckingLevel::None
>;
class Clap2App : public BasePlugin {
	public:
	Clap2App(const clap_plugin_descriptor* desc, const clap_host* host)
		: BasePlugin(desc, host) {}

	protected:
	bool implementsAudioPorts() const noexcept override { return true; }

	uint32_t audioPortsCount(bool isInput) const noexcept override {
		return isInput ? 1 : 0;
	}

	bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info *info) const noexcept override {
		if (!isInput || index > 0) {
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
		if (this->_device.empty()) {
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
		CComPtr<IMMDeviceEnumerator> enumerator;
		HRESULT hr = enumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator));
		if (FAILED(hr)) {
			return false;
		}
		CComPtr<IMMDevice> device;
		hr = enumerator->GetDevice(this->_device.c_str(), &device);
		if (FAILED(hr)) {
			return false;
		}
		hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&this->_client);
		if (FAILED(hr)) {
			return false;
		}
		REFERENCE_TIME bufferDuration = (REFERENCE_TIME)maxFrameCount *
			REFTIMES_PER_SEC / sampleRate;
		hr = this->_client->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			bufferDuration, 0, &format, nullptr
		);
		if (FAILED(hr)) {
			return false;
		}
		hr = this->_client->GetBufferSize(&this->_renderMinFrames);
		if (FAILED(hr)) {
			return false;
		}
		if (this->_renderMinFrames > maxFrameCount) {
			// The device's minimum buffer size is larger than the host's max frame
			// count. This means we need to buffer across host chunks, so we need to
			// ensure the buffer has enough space to contain an additional host chunk,
			// since the device's minimum buffer size might not be a multiple of a host
			// chunk.
			hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&this->_client);
			if (FAILED(hr)) {
				return false;
			}
			REFERENCE_TIME bufferDuration =
				(REFERENCE_TIME)(this->_renderMinFrames + maxFrameCount) *
				REFTIMES_PER_SEC / sampleRate;
			hr = this->_client->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
				bufferDuration, 0, &format, nullptr
			);
			if (FAILED(hr)) {
				return false;
			}
			hr = this->_client->GetBufferSize(&this->_renderBufferFrames);
			if (FAILED(hr)) {
				return false;
			}
		} else {
			this->_renderBufferFrames = this->_renderMinFrames;
		}
		hr = this->_client->GetService(__uuidof(IAudioRenderClient), (void**)&this->_render);
		if (FAILED(hr)) {
			return false;
		}
		return true;
	}

	void deactivate() noexcept  override {
		if (!this->_client) {
			return;
		}
		this->reset();
		this->_render = nullptr;
		this->_client = nullptr;
	}

	clap_process_status process(const clap_process *process) noexcept override {
		if (!this->_render) {
			return CLAP_PROCESS_SLEEP;
		}
		UINT32 paddingFrames;
		HRESULT hr = this->_client->GetCurrentPadding(&paddingFrames);
		if (FAILED(hr)) {
			return CLAP_PROCESS_SLEEP;
		}
		const UINT32 sendFrames = std::min(
			process->frames_count,
			this->_renderBufferFrames - paddingFrames
		);
		BYTE* data;
		hr = this->_render->GetBuffer(sendFrames, &data);
		if (FAILED(hr)) {
			return CLAP_PROCESS_SLEEP;
		}
		for (uint32_t f = 0; f < sendFrames; ++f) {
			memcpy(
				data,
				&process->audio_inputs[0].data32[0][f],
				sizeof(float)
			);
			data += sizeof(float);
			memcpy(
				data,
				&process->audio_inputs[0].data32[1][f],
				sizeof(float)
			);
			data += sizeof(float);
		}
		this->_render->ReleaseBuffer(sendFrames, 0);
		if (paddingFrames + sendFrames >= this->_renderMinFrames) {
			// There's enough in the render buffer to begin playback.
			this->_client->Start();
		}
		return CLAP_PROCESS_CONTINUE;
	}

	void reset() noexcept override {
		if (!this->_client) {
			return;
		}
		this->_client->Stop();
		this->_client->Reset();
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
			HINST_THISDLL, MAKEINTRESOURCE(ID_CLAP2APP_DLG),
			// hack: Use the grandparent so tabbing works in REAPER.
			GetParent((HWND)window->win32), Clap2App::dialogProc
		);
		SetWindowLongPtr(this->_dialog, GWLP_USERDATA, (LONG_PTR)this);
		this->_deviceCombo = GetDlgItem(this->_dialog, ID_DEVICE);
		this->buildDeviceList();
		return true;
	}

	bool implementsState() const noexcept override { return true; }

	bool stateSave(const clap_ostream* stream) noexcept override {
		stream->write(stream, &STATE_VERSION, sizeof(uint32_t));
		const size_t nBytes = this->_device.size() * sizeof(wchar_t);
		stream->write(stream, &nBytes, sizeof(size_t));
		const wchar_t* device = this->_device.c_str();
		stream->write(stream, device, nBytes);
		return true;
	}

	bool stateLoad(const clap_istream* stream) noexcept override {
		uint32_t version = 0;
		stream->read(stream, &version, sizeof(uint32_t));
		if (version != STATE_VERSION) {
			return false;
		}
		size_t nBytes = 0;
		stream->read(stream, &nBytes, sizeof(size_t));
		if (nBytes == 0) {
			return true;
		}
		const size_t nChars = nBytes / sizeof(wchar_t);
		auto device = std::make_unique<wchar_t[]>(nChars);
		stream->read(stream, device.get(), nBytes);
		this->_device = std::wstring(device.get(), nChars);
		// Restart the plugin. We will set up the send in activate().
		this->_host.host()->request_restart(this->_host.host());
		return true;
	}

	private:
	static INT_PTR CALLBACK dialogProc(HWND dialogHwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		auto* plugin = (Clap2App*)GetWindowLongPtr(dialogHwnd, GWLP_USERDATA);
		if (msg == WM_COMMAND) {
			const WORD cid = LOWORD(wParam);
			if (cid == ID_SEND) {
				const int choice = ComboBox_GetCurSel(plugin->_deviceCombo);
				if (choice == CB_ERR) {
					// The user hasn't chosen a device yet.
					return TRUE;
				}
				plugin->_device = plugin->_devices[choice];
				// Restart the plugin. We will set up the send in activate().
				plugin->_host.host()->request_restart(plugin->_host.host());
				return TRUE;
			}
		}
		return FALSE;
	}

	void buildDeviceList() {
		this->_devices.clear();
		ComboBox_ResetContent(this->_deviceCombo);
		CComPtr<IMMDeviceEnumerator> enumerator;
		HRESULT hr = enumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator));
		if (FAILED(hr)) {
			return;
		}
		CComPtr<IMMDeviceCollection> devices;
		hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
		if (FAILED(hr)) {
			return;
		}
		UINT count = 0;
		devices->GetCount(&count);
		for (UINT d = 0; d < count; ++d) {
			CComPtr<IMMDevice> device;
			hr = devices->Item(d, &device);
			if (FAILED(hr)) {
				continue;
			}
			wchar_t* id;
			hr = device->GetId(&id);
			if (FAILED(hr)) {
				continue;
			}
			this->_devices.push_back(id);
			const bool selected = this->_device == id;
			CoTaskMemFree(id);
			CComPtr<IPropertyStore> props;
			hr = device->OpenPropertyStore(STGM_READ, &props);
			if (FAILED(hr)) {
				return;
			}
			PROPVARIANT val;
			hr = props->GetValue(PKEY_Device_FriendlyName, &val);
			if (FAILED(hr)) {
				return;
			}
			ComboBox_AddString(this->_deviceCombo, val.pwszVal);
			if (selected) {
				// Select the previously chosen device.
				ComboBox_SetCurSel(this->_deviceCombo, this->_devices.size() - 1);
			}
		}
	}

	CComPtr<IAudioClient> _client;
	CComPtr<IAudioRenderClient> _render;
	HWND _dialog = nullptr;
	HWND _deviceCombo = nullptr;
	// The devices we have found.
	std::vector<std::wstring> _devices;
	// The chosen device.
	std::wstring _device;
	// The maximum number of frames that can fit in the render buffer.
	UINT32 _renderBufferFrames;
	// The minimum number of frames required to prevent rendering glitches.
	UINT32 _renderMinFrames = 0;
};

extern const clap_plugin_descriptor clap2AppDescriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "jantrid.clap2app",
	.name = "Clap2App",
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

const clap_plugin* createClap2App(const clap_host* host) {
	auto plugin = new Clap2App(&clap2AppDescriptor, host);
	return plugin->clapPlugin();
}

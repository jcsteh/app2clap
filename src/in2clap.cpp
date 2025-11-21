/*
 * App2Clap
 * In2Clap plug-in code
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2025 James Teh
 * License: GNU General Public License version 2.0
 */

#include "common.h"

#include <atlcomcli.h>
#include <audioclient.h>
#include <functiondiscoverykeys.h>
#include <Functiondiscoverykeys_devpkey.h>
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

const uint32_t STATE_VERSION = 1;

class In2Clap : public BasePlugin {
	public:
	In2Clap(const clap_plugin_descriptor* desc, const clap_host* host)
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
		if (this->_device.empty()) {
			return false;
		}
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
		WAVEFORMATEX format = {
			.wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
			.nChannels = NUM_CHANNELS,
			.nSamplesPerSec = (DWORD)sampleRate,
			.nAvgBytesPerSec = (DWORD)sampleRate * BYTES_PER_FRAME,
			.nBlockAlign = BYTES_PER_FRAME,
			.wBitsPerSample = BITS_PER_SAMPLE,
		};
		// IAudioClient::Initialize respects the buffer size during initialisation.
		// However, when capturing, it can return a much smaller buffer, so there's
		// no point in requesting a particular buffer size.
		hr = this->_client->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
			0, 0, &format, nullptr
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
		if (bufferSize < maxFrameCount) {
			// The host max frame count is larger than the device buffer. Capture audio
			// in a background thread to avoid continual buffer underruns. Note that the
			// thread is less optimal when the host max frame count is lower.
			hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&this->_client);
			if (FAILED(hr)) {
				return false;
			}
			hr = this->_client->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
				AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				0, 0, &format, nullptr
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
			" received bufferSize " << bufferSize <<
			" threaded " << (bool)event
		);
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
		this->_dialog = createDialog((HWND)window->win32, ID_IN2CLAP_DLG,
			In2Clap::dialogProc);
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
		if (dialogProcCommon(dialogHwnd, msg)) {
			return TRUE;
		}
		auto* plugin = (In2Clap*)GetWindowLongPtr(dialogHwnd, GWLP_USERDATA);
		if (msg == WM_COMMAND) {
			const WORD cid = LOWORD(wParam);
			if (cid == ID_CAPTURE) {
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
		hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
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

	CComPtr<IAudioClient> _client;
	CComPtr<IAudioCaptureClient> _capture;
	// A buffer to store audio we've captured but not yet sent to the host.
	using Buffer = CircularBuffer<std::pair<float, float>>;
	Buffer _buffer{0};
	HWND _dialog = nullptr;
	HWND _deviceCombo = nullptr;
	// The devices we have found.
	std::vector<std::wstring> _devices;
	// The chosen device.
	std::wstring _device;
	std::thread _captureThread;
	AutoHandle _captureEvent;
};

extern const clap_plugin_descriptor in2ClapDescriptor = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "jantrid.in2clap",
	.name = "In2Clap",
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

const clap_plugin* createIn2Clap(const clap_host* host) {
	auto plugin = new In2Clap(&in2ClapDescriptor, host);
	return plugin->clapPlugin();
}

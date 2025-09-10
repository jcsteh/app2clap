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

#include <algorithm>
#include <vector>

#include "clap/helpers/plugin.hxx"

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
		AUDIOCLIENT_ACTIVATION_PARAMS params = {
			.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
		};
		params.ProcessLoopbackParams.TargetProcessId = 8232;
		params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
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
		this->_client->Stop();
		this->_capture = nullptr;
		this->_client = nullptr;
	}

	clap_process_status process(const clap_process *process) noexcept override {
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

	private:
	CComPtr<IAudioClient> _client;
	CComPtr<IAudioCaptureClient> _capture;
	// A buffer to store audio we've captured but not yet sent to the host.
	std::vector<BYTE> _buffer;
	// How many bytes of _buffer we have already consumed.
	size_t _bufferConsumed;
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

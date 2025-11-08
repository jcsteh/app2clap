/*
 * App2Clap
 * Header for common code
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2025 James Teh
 * License: GNU General Public License version 2.0
 */

#pragma once

// Avoid min macro conflict with std::min.
#define NOMINMAX
#define UNICODE

#include <dshow.h>
#include <windows.h>

#include "clap/helpers/plugin.hh"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISDLL ((HINSTANCE)&__ImageBase)

//#define dbg(msg) std::cout << "jtd " << msg << std::endl
#define dbg(msg)

constexpr WORD NUM_CHANNELS = 2;
constexpr WORD BYTES_PER_FRAME = sizeof(float) * NUM_CHANNELS;
constexpr WORD BITS_PER_SAMPLE = sizeof(float) * 8;
constexpr REFERENCE_TIME REFTIMES_PER_SEC = 10000000;

using BasePlugin = clap::helpers::Plugin<
	clap::helpers::MisbehaviourHandler::Ignore,
	clap::helpers::CheckingLevel::None
>;

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

HWND createDialog(HWND parent, int resourceId, DLGPROC dialogProc);
bool guiShowCommon(HWND dialog);
bool dialogProcCommon(HWND dialog, UINT msg);

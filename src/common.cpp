/*
 * App2Clap
 * Common code
 * Author: James Teh <jamie@jantrid.net>
 * Copyright 2025 James Teh
 * License: GNU General Public License version 2.0
 */

#include "common.h"

bool isReaperWrapper(HWND hwnd) {
	wchar_t className[30];
	return GetClassName(hwnd, className, _countof(className)) != 0 &&
		wcscmp(className, L"reaperPluginHostWrapProc") == 0;
}

HWND createDialog(HWND parent, int resourceId, DLGPROC dialogProc) {
	if (isReaperWrapper(parent)) {
		// hack: Use the grandparent so tabbing works in REAPER.
		parent = GetParent(parent);
	}
	return CreateDialog(
		HINST_THISDLL, MAKEINTRESOURCE(resourceId), parent, dialogProc);
}

bool guiShowCommon(HWND dialog) {
	ShowWindow(dialog, SW_SHOW);
	HWND hostParent = GetWindow(dialog, GW_HWNDPREV);
	if (isReaperWrapper(hostParent)) {
		// We need to hide the parent we were supposed to use so hit testing works.
		// However, REAPER calls ShowWindow after this, so we can't do it here.
		// Instead, we handle this in our dialogProc.
		PostMessage(dialog, WM_APP, 0, 0);
	}
	return true;
}

bool dialogProcCommon(HWND dialog, UINT msg) {
	if (msg == WM_APP) {
		// Posted by guiShow().
		HWND hostParent = GetWindow(dialog, GW_HWNDPREV);
		ShowWindow(hostParent, SW_HIDE);
		return TRUE;
	}
	return false;
}

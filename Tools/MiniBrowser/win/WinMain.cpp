/*
 * Copyright (C) 2006-2023 Apple Inc.  All rights reserved.
 * Copyright (C) 2009, 2011 Brent Fulgham.  All rights reserved.
 * Copyright (C) 2009, 2010, 2011 Appcelerator, Inc. All rights reserved.
 * Copyright (C) 2013 Alex Christensen. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#pragma warning(disable: 4091)

#include "stdafx.h"
#include "Common.h"
#include "MiniBrowserLibResource.h"
#include "WebKitBrowserWindow.h"

#include <sddl.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::wstring processMutexName()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return { };
    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<unsigned char> storage(required);
    bool read = required && GetTokenInformation(
        token, TokenUser, storage.data(), required, &required);
    CloseHandle(token);
    if (!read)
        return { };
    auto* user = reinterpret_cast<TOKEN_USER*>(storage.data());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid) || !sid)
        return { };
    std::wstring result = L"Global\\XanhBrowser.WinCairo.Process.v1.";
    result += sid;
    LocalFree(sid);
    return result;
}

bool isSameExecutable(HWND window)
{
    DWORD processID = 0;
    if (!window || !GetWindowThreadProcessId(window, &processID)
        || !processID || processID == GetCurrentProcessId())
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, processID);
    if (!process)
        return false;
    std::vector<wchar_t> other(32768);
    DWORD otherLength = static_cast<DWORD>(other.size());
    bool queried = QueryFullProcessImageNameW(
        process, 0, other.data(), &otherLength);
    CloseHandle(process);
    if (!queried || !otherLength || otherLength >= other.size())
        return false;
    std::vector<wchar_t> current(32768);
    auto currentLength = GetModuleFileNameW(
        nullptr, current.data(), static_cast<DWORD>(current.size()));
    return currentLength && currentLength < current.size()
        && currentLength == otherLength
        && !_wcsnicmp(current.data(), other.data(), currentLength);
}

HWND waitForPrimaryWindow(HINSTANCE instance)
{
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        auto window = MainWindow::findExistingInstance(instance);
        if (isSameExecutable(window))
            return window;
        Sleep(10);
    }
    return nullptr;
}

bool isFirefoxSyncCallback(std::wstring_view value)
{
    constexpr std::wstring_view callbackScheme = L"xanh-browser-wincairo:";
    return value.size() >= callbackScheme.size()
        && !_wcsnicmp(value.data(), callbackScheme.data(), callbackScheme.size());
}

bool forwardToPrimary(HWND primary, const std::wstring& value)
{
    if (!primary || value.size() > XanhMaximumInterprocessURLCharacters
        || value.find(L'\0') != std::wstring_view::npos)
        return false;
    if (value.empty())
        return PostMessageW(primary, XanhActivateExistingWindowMessage, 0, 0);
    COPYDATASTRUCT data { };
    data.dwData = isFirefoxSyncCallback(value)
        ? XanhOAuthCallbackCopyData : XanhNavigationCopyData;
    data.cbData = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(value.data());
    DWORD_PTR response = 0;
    return SendMessageTimeoutW(primary, WM_COPYDATA, 0,
        reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &response)
        && response == TRUE;
}

void wipe(std::wstring& value)
{
    if (!value.empty())
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    value.clear();
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow)
{
    hInst = hInstance;
#ifdef _CRTDBG_MAP_ALLOC
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
#endif

    MSG msg { };
    HACCEL hAccelTable, hPreAccelTable;

    INITCOMMONCONTROLSEX InitCtrlEx;

    InitCtrlEx.dwSize = sizeof(INITCOMMONCONTROLSEX);
    InitCtrlEx.dwICC  = 0x00004000; // ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&InitCtrlEx);

    auto options = parseCommandLine();
    std::wstring requestedURL;
    if (options.requestedURL.length()) {
        requestedURL.assign(
            options.requestedURL.GetBSTR(), options.requestedURL.length());
        SecureZeroMemory(options.requestedURL.GetBSTR(),
            SysStringByteLen(options.requestedURL.GetBSTR()));
        options.requestedURL = L"";
    }

    auto mutexName = processMutexName();
    HANDLE processMutex = mutexName.empty()
        ? nullptr : CreateMutexW(nullptr, FALSE, mutexName.c_str());
    auto mutexError = GetLastError();
    if (!processMutex) {
        wipe(requestedURL);
        MessageBoxW(nullptr,
            L"Xanh Browser could not establish its per-user process lock.",
            L"Xanh Browser", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS) {
        auto primary = waitForPrimaryWindow(hInstance);
        bool forwarded = forwardToPrimary(primary, requestedURL);
        wipe(requestedURL);
        CloseHandle(processMutex);
        if (!forwarded) {
            MessageBoxW(nullptr,
                L"Xanh Browser could not securely deliver this request to the running browser process.",
                L"Xanh Browser", MB_OK | MB_ICONERROR);
            return 1;
        }
        return 0;
    }

    if (options.useFullDesktop)
        computeFullDesktopFrame();

    // Init COM
    OleInitialize(nullptr);

    auto factory = WebKitBrowserWindow::create;
    auto& mainWindow = MainWindow::create().leakRef();
    HRESULT hr = mainWindow.init(factory, hInst, options.usesLayeredWebView);
    if (FAILED(hr))
        goto exit;

    ShowWindow(mainWindow.hwnd(), nCmdShow);

    hAccelTable = LoadAccelerators(hInst, MAKEINTRESOURCE(IDC_MINIBROWSER));
    hPreAccelTable = LoadAccelerators(hInst, MAKEINTRESOURCE(IDR_ACCELERATORS_PRE));

    if (!requestedURL.empty()) {
        if (isFirefoxSyncCallback(requestedURL)) {
            if (!mainWindow.browserWindow()->canHandleFirefoxSyncCallback()
                || !mainWindow.browserWindow()->handleFirefoxSyncCallback(
                    std::move(requestedURL))) {
                wipe(requestedURL);
                MessageBoxW(mainWindow.hwnd(),
                    L"This Firefox Sync callback does not match an active sign-in.",
                    L"Xanh Browser", MB_OK | MB_ICONERROR);
            }
            mainWindow.goHome();
        } else
            mainWindow.loadURL(std::move(requestedURL));
    } else
        mainWindow.goHome();

#pragma warning(disable:4509)

    // Main message loop:
    __try {
        while (GetMessage(&msg, nullptr, 0, 0)) {
            if (TranslateAccelerator(msg.hwnd, hPreAccelTable, &msg))
                continue;
            bool processed = false;
            if (MainWindow::isInstance(msg.hwnd))
                processed = TranslateAccelerator(msg.hwnd, hAccelTable, &msg);
            if (!processed) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    } __except(createCrashReport(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) { }

exit:
#ifdef _CRTDBG_MAP_ALLOC
    _CrtDumpMemoryLeaks();
#endif

    // Shut down COM.
    OleUninitialize();
    wipe(requestedURL);
    CloseHandle(processMutex);

    return static_cast<int>(msg.wParam);
}

/*
 * Copyright (C) 2018 Sony Interactive Entertainment Inc.
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

#include "stdafx.h"
#include "MainWindow.h"

#include "Common.h"
#include "MiniBrowserLibResource.h"
#include "WebKitBrowserWindow.h"
#include "XanhNavigationPolicy.h"
#include "XanhPortableBackup.h"
#include <algorithm>
#include <chrono>
#include <optional>
#include <shobjidl.h>
#include <sstream>
#include <stdexcept>
#include <wrl/client.h>

namespace WebCore {
float deviceScaleFactorForWindow(HWND);
}

static const wchar_t* kMiniBrowserRegistryKey = L"Software\\Xanh Browser\\WebKit";

static constexpr int kToolbarImageSize = 32;
static constexpr int kToolbarURLBarIndex = 4;
static constexpr int kToolbarProgressIndicatorIndex = 5;

static WNDPROC DefEditProc = nullptr;

static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

std::wstring MainWindow::s_windowClass;
size_t MainWindow::s_numInstances;
std::vector<MainWindow*> MainWindow::s_instances;

namespace {

using Microsoft::WRL::ComPtr;

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : m_handle(handle)
    {
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    ~UniqueHandle()
    {
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }

    HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle;
};

class SecurePassword {
public:
    SecurePassword() = default;
    SecurePassword(const SecurePassword&) = delete;
    SecurePassword& operator=(const SecurePassword&) = delete;
    ~SecurePassword() { clear(); }

    void clear()
    {
        if (!value.empty())
            SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
    }

    std::wstring value;
};

struct BackupPasswordDialogState {
    SecurePassword* password { nullptr };
    bool requiresConfirmation { false };
};

std::wstring readDialogText(HWND dialog, int control)
{
    int length = GetWindowTextLength(GetDlgItem(dialog, control));
    if (length < 0 || length > 1024)
        throw std::runtime_error("Invalid backup password length.");
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
    int copied = GetDlgItemText(dialog, control, buffer.data(), static_cast<int>(buffer.size()));
    std::wstring result(buffer.data(), static_cast<size_t>(std::max(copied, 0)));
    SecureZeroMemory(buffer.data(), buffer.size() * sizeof(wchar_t));
    return result;
}

void secureErase(std::wstring& value)
{
    if (!value.empty())
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    value.clear();
}

std::optional<std::wstring> pathFromDialog(IFileDialog& dialog)
{
    ComPtr<IShellItem> item;
    if (FAILED(dialog.GetResult(&item)))
        throw std::runtime_error("Could not read the selected backup path.");
    PWSTR rawPath = nullptr;
    HRESULT pathResult = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    if (FAILED(pathResult) || !rawPath) {
        if (rawPath)
            CoTaskMemFree(rawPath);
        throw std::runtime_error("The backup must be stored in a filesystem location.");
    }
    std::wstring path(rawPath);
    CoTaskMemFree(rawPath);
    if (path.empty())
        throw std::runtime_error("The selected backup path is empty.");
    return path;
}

std::optional<std::wstring> chooseBackupPath(HWND owner, bool exporting)
{
    constexpr COMDLG_FILTERSPEC filter[] = {
        { L"Xanh Browser encrypted backup (*.xanhbackup)", L"*.xanhbackup" },
    };

    ComPtr<IFileDialog> dialog;
    HRESULT result = exporting
        ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
        : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result))
        throw std::runtime_error("Could not open the backup file picker.");
    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
        throw std::runtime_error("Could not configure the backup file picker.");
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    options |= exporting ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN : FOS_FILEMUSTEXIST;
    if (FAILED(dialog->SetOptions(options)) || FAILED(dialog->SetFileTypes(_countof(filter), filter)))
        throw std::runtime_error("Could not configure the backup file picker.");
    if (exporting) {
        if (FAILED(dialog->SetDefaultExtension(L"xanhbackup")) || FAILED(dialog->SetFileName(L"xanh-browser.xanhbackup"))
            || FAILED(dialog->SetTitle(L"Export encrypted Xanh Browser backup")))
            throw std::runtime_error("Could not configure the backup export picker.");
    } else if (FAILED(dialog->SetTitle(L"Import encrypted Xanh Browser backup")))
        throw std::runtime_error("Could not configure the backup import picker.");
    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return std::nullopt;
    if (FAILED(result))
        throw std::runtime_error("The backup file picker failed.");
    auto path = pathFromDialog(*dialog.Get());
    if (exporting) {
        constexpr std::wstring_view extension = L".xanhbackup";
        if (path->size() < extension.size() || _wcsicmp(path->c_str() + path->size() - extension.size(), extension.data()))
            throw std::runtime_error("The confirmed export path must end in .xanhbackup.");
    }
    return path;
}

std::vector<uint8_t> readBackupFile(const std::wstring& path)
{
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Could not open the selected backup.");
    LARGE_INTEGER size { };
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > XanhPortableBackup::maximumEncodedBytes)
        throw std::runtime_error("The selected backup is too large.");
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    size_t position = 0;
    while (position < bytes.size()) {
        DWORD read = 0;
        if (!ReadFile(file.get(), bytes.data() + position, static_cast<DWORD>(bytes.size() - position), &read, nullptr) || !read)
            throw std::runtime_error("Could not read the complete backup.");
        position += read;
    }
    return bytes;
}

std::wstring temporaryBackupPath(const std::wstring& destination)
{
    GUID identifier;
    if (FAILED(CoCreateGuid(&identifier)))
        throw std::runtime_error("Could not create a temporary backup name.");
    wchar_t identifierText[40] { };
    if (!StringFromGUID2(identifier, identifierText, _countof(identifierText)))
        throw std::runtime_error("Could not create a temporary backup name.");
    return destination + L".tmp-" + identifierText;
}

void writeBackupFileAtomically(const std::wstring& destination, const std::vector<uint8_t>& bytes)
{
    if (bytes.empty() || bytes.size() > XanhPortableBackup::maximumEncodedBytes)
        throw std::runtime_error("Invalid backup output size.");
    auto temporary = temporaryBackupPath(destination);
    bool mustDeleteTemporary = true;
    try {
        {
            UniqueHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
            if (file.get() == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Could not create the temporary backup.");
            size_t position = 0;
            while (position < bytes.size()) {
                DWORD written = 0;
                if (!WriteFile(file.get(), bytes.data() + position, static_cast<DWORD>(bytes.size() - position), &written, nullptr) || !written)
                    throw std::runtime_error("Could not write the complete backup.");
                position += written;
            }
            if (!FlushFileBuffers(file.get()))
                throw std::runtime_error("Could not make the backup durable.");
        }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not publish the encrypted backup.");
        mustDeleteTemporary = false;
    } catch (...) {
        if (mustDeleteTemporary)
            DeleteFileW(temporary.c_str());
        throw;
    }
}

} // namespace

static std::wstring loadString(int id)
{
    constexpr size_t length = 100;
    wchar_t buff[length];
    LoadString(hInst, id, buff, length);
    return buff;
}

void MainWindow::registerClass(HINSTANCE hInstance)
{
    static bool initialized = false;
    if (initialized)
        return;
    initialized = true;

    s_windowClass = loadString(IDC_MINIBROWSER);

    WNDCLASSEX wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MINIBROWSER));
    wcex.hCursor        = LoadCursor(0, IDC_ARROW);
    wcex.hbrBackground  = 0;
    wcex.lpszMenuName   = MAKEINTRESOURCE(IDC_MINIBROWSER);
    wcex.lpszClassName  = s_windowClass.c_str();
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    RegisterClassEx(&wcex);
}

bool MainWindow::isInstance(HWND hwnd)
{
    wchar_t buff[64];
    if (!GetClassName(hwnd, buff, _countof(buff)))
        return false;
    return s_windowClass == buff;
}

HWND MainWindow::routeFirefoxSyncCallback(std::wstring&& callback)
{
    auto target = std::find_if(s_instances.begin(), s_instances.end(),
        [](auto* window) {
            return window && window->browserWindow()
                && window->browserWindow()->canHandleFirefoxSyncCallback();
        });
    if (target == s_instances.end())
        return nullptr;
    if (!(*target)->browserWindow()->handleFirefoxSyncCallback(
            std::move(callback)))
        return nullptr;
    return (*target)->hwnd();
}

HWND MainWindow::findExistingInstance(HINSTANCE instance)
{
    wchar_t windowClass[100] { };
    if (!LoadStringW(instance, IDC_MINIBROWSER, windowClass,
            static_cast<int>(std::size(windowClass))))
        return nullptr;
    return FindWindowW(windowClass, nullptr);
}

MainWindow::MainWindow()
{
    s_numInstances++;
}

MainWindow::~MainWindow()
{
    if (m_isRegisteredInstance)
        s_instances.erase(std::remove(s_instances.begin(), s_instances.end(), this), s_instances.end());
    s_numInstances--;
}

Ref<MainWindow> MainWindow::create()
{
    return adoptRef(*new MainWindow());
}

void MainWindow::resetFeatureMenu(BrowserWindow::FeatureType featureType, bool resetsSettingsToDefaults)
{
    auto settingMenu = GetSubMenu(GetMenu(m_hMainWnd), 3);
    int index = featureType == BrowserWindow::FeatureType::Experimental ? 0 : 1;
    auto featureMenu = GetSubMenu(settingMenu, index);
    assert(GetMenuItemID(featureMenu, 0) == (featureType == BrowserWindow::FeatureType::Experimental ? IDM_EXPERIMENTAL_FEATURES_RESET_ALL_TO_DEFAULTS : IDM_INTERNAL_DEBUG_FEATURES_RESET_ALL_TO_DEFAULTS));
    m_browserWindow->resetFeatureMenu(featureType, featureMenu, resetsSettingsToDefaults);
}

void MainWindow::createToolbar(HINSTANCE hInstance)
{
    m_hToolbarWnd = CreateWindowEx(0, TOOLBARCLASSNAME, nullptr, 
        WS_CHILD | WS_BORDER | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS, 0, 0, 0, 0, 
        m_hMainWnd, nullptr, hInstance, nullptr);
        
    if (!m_hToolbarWnd)
        return;

    const int ImageListID = 0;

    HIMAGELIST hImageList;
    hImageList = ImageList_LoadImage(hInstance, MAKEINTRESOURCE(IDB_TOOLBAR), kToolbarImageSize, 0, CLR_DEFAULT, IMAGE_BITMAP, 0);

    SendMessage(m_hToolbarWnd, TB_SETIMAGELIST, ImageListID, reinterpret_cast<LPARAM>(hImageList));
    SendMessage(m_hToolbarWnd, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_MIXEDBUTTONS);

    const DWORD buttonStyles = BTNS_AUTOSIZE;

    TBBUTTON tbButtons[] = {
        { MAKELONG(0, ImageListID), IDM_HISTORY_BACKWARD, TBSTATE_ENABLED, buttonStyles | BTNS_DROPDOWN, { }, 0, (INT_PTR)L"Back" },
        { MAKELONG(1, ImageListID), IDM_HISTORY_FORWARD, TBSTATE_ENABLED, buttonStyles | BTNS_DROPDOWN, { }, 0, (INT_PTR)L"Forward"},
        { MAKELONG(2, ImageListID), IDM_RELOAD, TBSTATE_ENABLED, buttonStyles, { }, 0, (INT_PTR)L"Reload"},
        { MAKELONG(3, ImageListID), IDM_GO_HOME, TBSTATE_ENABLED, buttonStyles, { }, 0, (INT_PTR)L"Home"},
        { 0, 0, TBSTATE_ENABLED, BTNS_SEP, { }, 0, 0}, // URL bar
        { 0, 0, TBSTATE_ENABLED, BTNS_SEP, { }, 0, 0}, // Progress indicator
    };
    ASSERT(tbButtons[kToolbarURLBarIndex].fsStyle == BTNS_SEP);
    ASSERT(tbButtons[kToolbarProgressIndicatorIndex].fsStyle == BTNS_SEP);

    SendMessage(m_hToolbarWnd, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessage(m_hToolbarWnd, TB_ADDBUTTONS, _countof(tbButtons), reinterpret_cast<LPARAM>(&tbButtons));
    ShowWindow(m_hToolbarWnd, true);

    m_hURLBarWnd = CreateWindow(L"EDIT", 0, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 0, 0, m_hToolbarWnd, 0, hInstance, 0);
    m_hProgressIndicator = CreateWindow(L"STATIC", 0, WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0, 0, 0, m_hToolbarWnd, 0, hInstance, 0);

    DefEditProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(m_hURLBarWnd, GWLP_WNDPROC));
    SetWindowLongPtr(m_hURLBarWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc));
}

void MainWindow::resizeToolbar(int parentWidth)
{
    TBBUTTONINFO info { };
    info.cbSize = sizeof(TBBUTTONINFO);
    info.dwMask = TBIF_BYINDEX | TBIF_SIZE;
    info.cx = parentWidth - m_toolbarItemsWidth;
    SendMessage(m_hToolbarWnd, TB_SETBUTTONINFO, kToolbarURLBarIndex, reinterpret_cast<LPARAM>(&info));

    SendMessage(m_hToolbarWnd, TB_AUTOSIZE, 0, 0);

    RECT rect;
    SendMessage(m_hToolbarWnd, TB_GETITEMRECT, kToolbarURLBarIndex, reinterpret_cast<LPARAM>(&rect));
    MoveWindow(m_hURLBarWnd, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, true);

    SendMessage(m_hToolbarWnd, TB_GETITEMRECT, kToolbarProgressIndicatorIndex, reinterpret_cast<LPARAM>(&rect));
    MoveWindow(m_hProgressIndicator, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, true);
}

void MainWindow::rescaleToolbar()
{
    const float scaleFactor = WebCore::deviceScaleFactorForWindow(m_hMainWnd);
    const int scaledImageSize = kToolbarImageSize * scaleFactor;

    TBBUTTONINFO info { };
    info.cbSize = sizeof(TBBUTTONINFO);
    info.dwMask = TBIF_BYINDEX | TBIF_SIZE;
    info.cx = 0;
    SendMessage(m_hToolbarWnd, TB_SETBUTTONINFO, kToolbarURLBarIndex, reinterpret_cast<LPARAM>(&info));
    info.cx = scaledImageSize * 2;
    SendMessage(m_hToolbarWnd, TB_SETBUTTONINFO, kToolbarProgressIndicatorIndex, reinterpret_cast<LPARAM>(&info));

    SendMessage(m_hToolbarWnd, TB_AUTOSIZE, 0, 0);

    int numItems = SendMessage(m_hToolbarWnd, TB_BUTTONCOUNT, 0, 0);

    RECT rect;
    SendMessage(m_hToolbarWnd, TB_GETITEMRECT, numItems-1, reinterpret_cast<LPARAM>(&rect));
    m_toolbarItemsWidth = rect.right;
}

bool MainWindow::init(BrowserWindowFactory factory, HINSTANCE hInstance, bool usesLayeredWebView)
{
    registerClass(hInstance);

    auto title = loadString(IDS_APP_TITLE);

    m_hMainWnd = CreateWindow(s_windowClass.c_str(), title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, 0, 0, hInstance, this);

    if (!m_hMainWnd)
        return false;

    createToolbar(hInstance);
    if (!m_hToolbarWnd)
        return false;

    m_browserWindow = factory(*this, m_hMainWnd, usesLayeredWebView);
    if (!m_browserWindow)
        return false;
    HRESULT hr = m_browserWindow->init();
    if (FAILED(hr))
        return false;

    resetFeatureMenu(BrowserWindow::FeatureType::Experimental);
    resetFeatureMenu(BrowserWindow::FeatureType::InternalDebug);
    
    updateDeviceScaleFactor();
    resizeSubViews();
    SetFocus(m_hURLBarWnd);
    s_instances.push_back(this);
    m_isRegisteredInstance = true;
    return true;
}

void MainWindow::resizeSubViews()
{
    RECT rcClient;
    GetClientRect(m_hMainWnd, &rcClient);

    resizeToolbar(rcClient.right);

    if (m_browserWindow->usesLayeredWebView() || !m_browserWindow->hwnd())
        return;

    RECT rect;
    GetWindowRect(m_hToolbarWnd, &rect);
    POINT toolbarBottom = { 0, rect.bottom };
    ScreenToClient(m_hMainWnd, &toolbarBottom);
    auto height = toolbarBottom.y;
    MoveWindow(m_browserWindow->hwnd(), 0, height, rcClient.right, rcClient.bottom - height, true);
}

LRESULT CALLBACK MainWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    LRESULT result = 0;
    RefPtr<MainWindow> thisWindow = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    switch (message) {
    case XanhActivateExistingWindowMessage:
        if (thisWindow) {
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
        }
        return 0;
    case XanhFirefoxSyncResultMessage:
        if (thisWindow && thisWindow->browserWindow()
            && thisWindow->browserWindow()->handleFirefoxSyncPresentation(
                wParam, lParam))
            return 0;
        break;
    case WM_COPYDATA: {
        if (!thisWindow || !thisWindow->browserWindow())
            return FALSE;
        auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (!data || (data->dwData != XanhOAuthCallbackCopyData
                && data->dwData != XanhNavigationCopyData)
            || !data->lpData || !data->cbData
            || data->cbData % sizeof(wchar_t)
            || data->cbData > (XanhMaximumInterprocessURLCharacters + 1)
                * sizeof(wchar_t))
            return FALSE;
        auto characters = data->cbData / sizeof(wchar_t);
        auto* value = static_cast<const wchar_t*>(data->lpData);
        if (characters < 2 || value[characters - 1] != L'\0'
            || std::find(value, value + characters - 1, L'\0')
                != value + characters - 1)
            return FALSE;
        std::wstring incoming(value, characters - 1);
        HWND activationWindow = hWnd;
        if (data->dwData == XanhOAuthCallbackCopyData) {
            activationWindow = routeFirefoxSyncCallback(std::move(incoming));
            if (!activationWindow) {
                secureErase(incoming);
                return FALSE;
            }
        } else
            thisWindow->loadURL(std::move(incoming));
        secureErase(incoming);
        ShowWindow(activationWindow, SW_RESTORE);
        SetForegroundWindow(activationWindow);
        return TRUE;
    }
    case WM_ACTIVATE:
        thisWindow->browserWindow()->applicationActivationChanged(
            LOWORD(wParam) != WA_INACTIVE, reinterpret_cast<HWND>(lParam));
        switch (LOWORD(wParam)) {
        case WA_ACTIVE:
        case WA_CLICKACTIVE:
            SetFocus(thisWindow->browserWindow()->hwnd());
        }
        break;
    case WM_CREATE:
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams));
        break;
    case WM_APPCOMMAND: {
        auto cmd = GET_APPCOMMAND_LPARAM(lParam);
        switch (cmd) {
        case APPCOMMAND_BROWSER_BACKWARD:
            thisWindow->browserWindow()->navigateForwardOrBackward(false);
            result = 1;
            break;
        case APPCOMMAND_BROWSER_FORWARD:
            thisWindow->browserWindow()->navigateForwardOrBackward(true);
            result = 1;
            break;
        case APPCOMMAND_BROWSER_HOME:
            thisWindow->goHome();
            break;
        case APPCOMMAND_BROWSER_REFRESH:
            thisWindow->browserWindow()->reload();
            result = 1;
            break;
        case APPCOMMAND_BROWSER_STOP:
            break;
        }
        break;
    }
    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        int wmEvent = HIWORD(wParam);
        switch (wmEvent) {
        case 0: // Menu or BN_CLICKED
        case 1: // Accelerator
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        if (wmId >= IDM_HISTORY_LINK0 && wmId <= IDM_HISTORY_LINK9) {
            thisWindow->browserWindow()->navigateToHistory(wmId);
            break;
        }
        // Parse the menu selections:
        switch (wmId) {
        case IDC_URL_BAR:
            thisWindow->onURLBarEnter();
            break;
        case IDM_NEW_WEBKIT_WINDOW: {
            auto& newWindow = MainWindow::create().leakRef();
            newWindow.init(WebKitBrowserWindow::create, hInst);
            ShowWindow(newWindow.hwnd(), SW_SHOW);
            break;
        }
        case IDM_CLOSE_WINDOW:
            PostMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_GO_HOME:
            thisWindow->goHome();
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        case IDM_PRINT:
            thisWindow->browserWindow()->print();
            break;
        case IDM_EXPORT_PORTABLE_BACKUP:
            thisWindow->exportPortableBackup();
            break;
        case IDM_IMPORT_PORTABLE_BACKUP:
            thisWindow->importPortableBackup();
            break;
        case IDM_FIREFOX_SYNC_SIGN_IN:
            thisWindow->browserWindow()->beginFirefoxSync();
            break;
        case IDM_FIREFOX_SYNC_NOW:
            thisWindow->browserWindow()->syncFirefoxSync();
            break;
        case IDM_FIREFOX_SYNC_DISCONNECT:
            if (MessageBoxW(hWnd,
                    L"Disconnect Firefox Sync and keep local browser data?",
                    L"Xanh Browser", MB_YESNO | MB_ICONQUESTION)
                == IDYES)
                thisWindow->browserWindow()->disconnectFirefoxSync(false);
            break;
        case IDM_FIREFOX_SYNC_REMOVE_DEVICE:
            if (MessageBoxW(hWnd,
                    L"Remove the Firefox account, Sync databases and local password key from this device? This cannot be undone.",
                    L"Xanh Browser", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING)
                == IDYES)
                thisWindow->browserWindow()->disconnectFirefoxSync(true);
            break;
        case IDM_WEB_INSPECTOR:
            thisWindow->browserWindow()->launchInspector();
            break;
        case IDM_PROXY_SETTINGS:
            thisWindow->browserWindow()->openProxySettings();
            break;
        case IDM_EXPERIMENTAL_FEATURES_RESET_ALL_TO_DEFAULTS:
            thisWindow->resetFeatureMenu(BrowserWindow::FeatureType::Experimental, true);
            break;
        case IDM_INTERNAL_DEBUG_FEATURES_RESET_ALL_TO_DEFAULTS:
            thisWindow->resetFeatureMenu(BrowserWindow::FeatureType::InternalDebug, true);
            break;
        case IDM_SET_DEFAULT_URL:
            thisWindow->setDefaultURLToCurrentURL();
            break;
        case IDM_CACHES:
            if (!::IsWindow(thisWindow->m_hCacheWnd)) {
                thisWindow->m_hCacheWnd = CreateDialogParam(hInst, MAKEINTRESOURCE(IDD_CACHES), hWnd, cachesDialogProc, reinterpret_cast<LPARAM>(thisWindow.get()));
                ::ShowWindow(thisWindow->m_hCacheWnd, SW_SHOW);
            }
            break;
        case IDM_HISTORY_BACKWARD:
        case IDM_HISTORY_FORWARD:
            thisWindow->browserWindow()->navigateForwardOrBackward(wmId == IDM_HISTORY_FORWARD);
            break;
        case IDM_UA_OTHER:
            DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_USER_AGENT), hWnd, customUserAgentDialogProc, reinterpret_cast<LPARAM>(thisWindow.get()));
            break;
        case IDM_ACTUAL_SIZE:
            thisWindow->browserWindow()->resetZoom();
            break;
        case IDM_RELOAD:
            thisWindow->browserWindow()->reload();
            break;
        case IDM_ZOOM_IN:
            thisWindow->browserWindow()->zoomIn();
            break;
        case IDM_ZOOM_OUT:
            thisWindow->browserWindow()->zoomOut();
            break;
        case IDM_SHOW_LAYER_TREE:
            thisWindow->browserWindow()->showLayerTree();
            break;
        case IDM_CLEAR_COOKIES:
            thisWindow->browserWindow()->clearCookies();
            break;
        case IDM_CLEAR_WEBSITE_DATA:
            thisWindow->browserWindow()->clearWebsiteData();
            break;
        default:
            if (!thisWindow->toggleMenuItem(wmId))
                return DefWindowProc(hWnd, message, wParam, lParam);
        }
        }
        break;
    case WM_DESTROY:
        SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
        thisWindow->deref();
        if (s_numInstances > 1)
            return 0;
        PostQuitMessage(0);
        break;
    case WM_SIZE:
        thisWindow->resizeSubViews();
        break;
    case WM_DPICHANGED: {
        thisWindow->updateDeviceScaleFactor();
        thisWindow->browserWindow()->adjustScaleFactors();
        auto& rect = *reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hWnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        break;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return result;
}

static bool menuItemIsChecked(const MENUITEMINFO& info)
{
    return info.fState & MFS_CHECKED;
}

INT_PTR CALLBACK MainWindow::backupPasswordDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<BackupPasswordDialogState*>(GetWindowLongPtr(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        state = reinterpret_cast<BackupPasswordDialogState*>(lParam);
        SetWindowLongPtr(dialog, DWLP_USER, lParam);
        SendDlgItemMessage(dialog, IDC_BACKUP_PASSWORD, EM_SETLIMITTEXT, 1024, 0);
        SendDlgItemMessage(dialog, IDC_BACKUP_CONFIRMATION, EM_SETLIMITTEXT, 1024, 0);
        if (state->requiresConfirmation) {
            SetDlgItemText(dialog, IDC_BACKUP_PROMPT, L"Create a unique password with at least 8 characters.");
            SetWindowText(dialog, L"Export encrypted backup");
        } else {
            SetDlgItemText(dialog, IDC_BACKUP_PROMPT, L"Enter the password used to create this backup.");
            ShowWindow(GetDlgItem(dialog, IDC_BACKUP_CONFIRMATION_LABEL), SW_HIDE);
            ShowWindow(GetDlgItem(dialog, IDC_BACKUP_CONFIRMATION), SW_HIDE);
            SetWindowText(dialog, L"Import encrypted backup");
        }
        SetFocus(GetDlgItem(dialog, IDC_BACKUP_PASSWORD));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            try {
                auto password = readDialogText(dialog, IDC_BACKUP_PASSWORD);
                if (password.size() < 8 || password.size() > 1024) {
                    secureErase(password);
                    MessageBoxW(dialog, L"The password must contain between 8 and 1,024 characters.", L"Xanh Browser", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                if (state->requiresConfirmation) {
                    auto confirmation = readDialogText(dialog, IDC_BACKUP_CONFIRMATION);
                    bool matches = password == confirmation;
                    secureErase(confirmation);
                    if (!matches) {
                        secureErase(password);
                        MessageBoxW(dialog, L"The passwords do not match.", L"Xanh Browser", MB_OK | MB_ICONWARNING);
                        return TRUE;
                    }
                }
                state->password->clear();
                state->password->value = password;
                secureErase(password);
                EndDialog(dialog, IDOK);
            } catch (const std::exception&) {
                MessageBoxW(dialog, L"The password could not be read securely.", L"Xanh Browser", MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void turnOffOtherUserAgents(HMENU menu)
{
    MENUITEMINFO info;
    ::memset(&info, 0x00, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STATE;

    // Must unset the other menu items:
    for (UINT menuToClear = IDM_UA_DEFAULT; menuToClear <= IDM_UA_OTHER; ++menuToClear) {
        if (!::GetMenuItemInfo(menu, menuToClear, FALSE, &info))
            continue;
        if (!menuItemIsChecked(info))
            continue;

        info.fState = MFS_UNCHECKED;
        ::SetMenuItemInfo(menu, menuToClear, FALSE, &info);
    }
}

void MainWindow::setDefaultURLToCurrentURL()
{
    wchar_t url[INTERNET_MAX_URL_LENGTH];
    GetWindowText(m_hURLBarWnd, url, INTERNET_MAX_URL_LENGTH);
    auto length = wcslen(url);
    if (!length)
        return;
    RegSetKeyValue(HKEY_CURRENT_USER, kMiniBrowserRegistryKey, L"DefaultURL", REG_SZ, url, (length + 1) * sizeof(wchar_t));
}

bool MainWindow::toggleMenuItem(UINT menuID)
{
    HMENU menu = ::GetMenu(hwnd());

    switch (menuID) {
    case IDM_UA_DEFAULT:
    case IDM_UA_SAFARI:
    case IDM_UA_SAFARI_IOS_IPHONE:
    case IDM_UA_SAFARI_IOS_IPAD:
    case IDM_UA_EDGE:
    case IDM_UA_IE_11:
    case IDM_UA_CHROME_MAC:
    case IDM_UA_CHROME_WIN:
    case IDM_UA_FIREFOX_MAC:
    case IDM_UA_FIREFOX_WIN:
        m_browserWindow->setUserAgent(menuID);
        turnOffOtherUserAgents(menu);
        break;
    case IDM_UA_OTHER:
        // The actual user agent string will be set by the custom user agent dialog
        turnOffOtherUserAgents(menu);
        break;
    }

    MENUITEMINFO info = { };
    info.cbSize = sizeof(info);
    info.fMask = MIIM_STATE;

    if (!::GetMenuItemInfo(menu, menuID, FALSE, &info))
        return false;

    BOOL newState = !menuItemIsChecked(info);
    info.fState = (newState) ? MFS_CHECKED : MFS_UNCHECKED;
    ::SetMenuItemInfo(menu, menuID, FALSE, &info);

    m_browserWindow->setPreference(menuID, newState);

    return true;
}

LRESULT CALLBACK EditProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SETFOCUS:
        PostMessage(hWnd, EM_SETSEL, 0, -1);
        break;
    case WM_CHAR:
        if (wParam == 13) {
            // Enter Key
            ::PostMessage(GetParent(hWnd), static_cast<UINT>(WM_COMMAND), MAKELPARAM(IDC_URL_BAR, 0), 0);
            return 0;
        }
        break;
    }
    return CallWindowProc(DefEditProc, hWnd, message, wParam, lParam);
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK MainWindow::cachesDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    MainWindow& thisWindow = *reinterpret_cast<MainWindow*>(GetWindowLongPtr(hDlg, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        SetWindowLongPtr(hDlg, DWLP_USER, lParam);
        ::SetTimer(hDlg, IDT_UPDATE_STATS, 1000, nullptr);
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            ::KillTimer(hDlg, IDT_UPDATE_STATS);
            ::DestroyWindow(hDlg);
            thisWindow.m_hCacheWnd = 0;
            return (INT_PTR)TRUE;
        }
        break;

    case IDT_UPDATE_STATS:
        ::InvalidateRect(hDlg, nullptr, FALSE);
        return (INT_PTR)TRUE;

    case WM_PAINT:
        thisWindow.browserWindow()->updateStatistics(hDlg);
        break;
    }

    return (INT_PTR)FALSE;
}

INT_PTR CALLBACK MainWindow::customUserAgentDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    MainWindow& thisWindow = *reinterpret_cast<MainWindow*>(GetWindowLongPtr(hDlg, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG: {
        MainWindow& thisWindow = *reinterpret_cast<MainWindow*>(lParam);
        SetWindowLongPtr(hDlg, DWLP_USER, lParam);
        HWND edit = ::GetDlgItem(hDlg, IDC_USER_AGENT_INPUT);
        _bstr_t userAgent;
        userAgent = thisWindow.browserWindow()->userAgent();

        ::SetWindowText(edit, static_cast<LPCTSTR>(userAgent));
        return (INT_PTR)TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            HWND edit = ::GetDlgItem(hDlg, IDC_USER_AGENT_INPUT);

            TCHAR buffer[1024];
            int strLen = ::GetWindowText(edit, buffer, 1024);
            buffer[strLen] = 0;

            _bstr_t bstr(buffer);
            if (bstr.length()) {
                thisWindow.browserWindow()->setUserAgent(bstr);
                thisWindow.toggleMenuItem(IDM_UA_OTHER);
            }
        }

        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            ::EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

void MainWindow::loadURL(std::wstring url)
{
    if (url.find(L"://") == url.npos)
        url = L"https://" + url;

    if (!XanhNavigationPolicy::isAllowedWebURL(url))
        return;

    if (FAILED(m_browserWindow->loadURL(_bstr_t(url.c_str()))))
        return;

    SetFocus(m_browserWindow->hwnd());
}

void MainWindow::exportPortableBackup()
{
    try {
        auto destination = chooseBackupPath(m_hMainWnd, true);
        if (!destination)
            return;

        SecurePassword password;
        BackupPasswordDialogState passwordState { &password, true };
        auto dialogResult = DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_PORTABLE_BACKUP_PASSWORD), m_hMainWnd, backupPasswordDialogProc, reinterpret_cast<LPARAM>(&passwordState));
        if (dialogResult == IDCANCEL)
            return;
        if (dialogResult != IDOK)
            throw std::runtime_error("Could not collect the backup password.");

        XanhPortableBackup::Payload payload;
        auto createdAt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (createdAt < 0)
            throw std::runtime_error("The system clock cannot be used for a backup.");
        payload.createdAtEpochMilliseconds = static_cast<uint64_t>(createdAt);
        payload.sourceEdition = L"windows-wincairo";
        payload.desktopSite = false;

        auto appendWindow = [&](MainWindow* window) {
            if (!window || payload.urls.size() >= XanhPortableBackup::maximumURLs)
                return;
            auto canonicalURL = XanhPortableBackup::canonicalizeURL(window->m_activeURL, XanhNavigationPolicy::isAllowedWebURL);
            if (canonicalURL)
                payload.urls.push_back(std::move(*canonicalURL));
        };
        appendWindow(this);
        for (auto* window : s_instances) {
            if (window != this)
                appendWindow(window);
        }
        if (payload.urls.empty())
            throw std::runtime_error("There are no regular HTTP(S) pages to export.");
        payload.selectedIndex = 0;

        auto encoded = XanhPortableBackup::encode(payload, password.value, XanhNavigationPolicy::isAllowedWebURL);
        password.clear();
        writeBackupFileAtomically(*destination, encoded);
        MessageBoxW(m_hMainWnd, L"The encrypted backup was exported successfully.", L"Xanh Browser", MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception&) {
        MessageBoxW(m_hMainWnd, L"The encrypted backup could not be exported. The destination was not replaced.", L"Xanh Browser", MB_OK | MB_ICONERROR);
    }
}

void MainWindow::importPortableBackup()
{
    try {
        auto source = chooseBackupPath(m_hMainWnd, false);
        if (!source)
            return;
        auto encoded = readBackupFile(*source);

        SecurePassword password;
        BackupPasswordDialogState passwordState { &password, false };
        auto dialogResult = DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_PORTABLE_BACKUP_PASSWORD), m_hMainWnd, backupPasswordDialogProc, reinterpret_cast<LPARAM>(&passwordState));
        if (dialogResult == IDCANCEL)
            return;
        if (dialogResult != IDOK)
            throw std::runtime_error("Could not collect the backup password.");
        auto payload = XanhPortableBackup::decode(encoded, password.value, XanhNavigationPolicy::isAllowedWebURL);
        password.clear();

        std::vector<MainWindow*> importedWindows;
        importedWindows.reserve(payload.urls.size());
        try {
            for (const auto& url : payload.urls) {
                auto canonicalURL = XanhPortableBackup::canonicalizeURL(url, XanhNavigationPolicy::isAllowedWebURL);
                if (!canonicalURL)
                    throw std::runtime_error("Backup URL validation changed before navigation.");
                auto window = MainWindow::create();
                if (!window->init(WebKitBrowserWindow::create, hInst)) {
                    auto& failedWindow = window.leakRef();
                    if (failedWindow.hwnd())
                        DestroyWindow(failedWindow.hwnd());
                    else
                        failedWindow.deref();
                    throw std::runtime_error("Could not create an imported browser window.");
                }
                window->loadURL(std::move(*canonicalURL));
                importedWindows.push_back(&window.leakRef());
            }
        } catch (...) {
            for (auto* window : importedWindows)
                DestroyWindow(window->hwnd());
            throw;
        }
        for (auto* window : importedWindows)
            ShowWindow(window->hwnd(), SW_SHOW);
        SetForegroundWindow(importedWindows[payload.selectedIndex]->hwnd());
        MessageBoxW(importedWindows[payload.selectedIndex]->hwnd(), L"The encrypted backup was imported into new regular windows.", L"Xanh Browser", MB_OK | MB_ICONINFORMATION);
    } catch (const std::exception&) {
        MessageBoxW(m_hMainWnd, L"The backup could not be imported. Check the password, file integrity and URL contents.", L"Xanh Browser", MB_OK | MB_ICONERROR);
    }
}

void MainWindow::goHome()
{
    std::wstring defaultURL = L"https://duckduckgo.com/";
    wchar_t url[INTERNET_MAX_URL_LENGTH];
    DWORD urlLength = sizeof(url);
    if (!RegGetValue(HKEY_CURRENT_USER, kMiniBrowserRegistryKey, L"DefaultURL", RRF_RT_REG_SZ, nullptr, &url, &urlLength))
        defaultURL = url;
    loadURL(defaultURL);
}

void MainWindow::onURLBarEnter()
{
    wchar_t url[INTERNET_MAX_URL_LENGTH];
    GetWindowText(m_hURLBarWnd, url, INTERNET_MAX_URL_LENGTH);
    loadURL(url);
}

void MainWindow::updateDeviceScaleFactor()
{
    if (m_hURLBarFont)
        ::DeleteObject(m_hURLBarFont);

    rescaleToolbar();

    RECT rect;
    GetClientRect(m_hToolbarWnd, &rect);
    int fontHeight = rect.bottom * 3. / 4;

    m_hURLBarFont = ::CreateFont(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Tahoma");
    ::SendMessage(m_hURLBarWnd, static_cast<UINT>(WM_SETFONT), reinterpret_cast<WPARAM>(m_hURLBarFont), TRUE);
}

void MainWindow::progressChanged(double progress)
{
    std::wostringstream text;
    text << static_cast<int>(progress * 100) << L'%';
    SetWindowText(m_hProgressIndicator, text.str().c_str());
}

void MainWindow::progressFinished()
{
    SetWindowText(m_hProgressIndicator, L"");
}

void MainWindow::activeURLChanged(std::wstring url)
{
    m_activeURL = url;
    SetWindowText(m_hURLBarWnd, url.c_str());
}

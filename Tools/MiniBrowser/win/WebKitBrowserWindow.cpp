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
#include "WebKitBrowserWindow.h"

#include "Common.h"
#include "Common2.h"
#include "MiniBrowserLibResource.h"
#include "XanhCredentialBridge.h"
#include "XanhNativeCredentialPicker.h"
#include "XanhNavigationPolicy.h"
#include "XanhProcessRecoveryPolicy.h"
#include <WebCore/GDIUtilities.h>
#include <WebKit/WKAuthenticationChallenge.h>
#include <WebKit/WKAuthenticationDecisionListener.h>
#include <WebKit/WKCertificateInfoCurl.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKCredential.h>
#include <WebKit/WKDownloadClient.h>
#include <WebKit/WKDownloadRef.h>
#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKHTTPCookieStoreRef.h>
#include <WebKit/WKInspector.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKNavigationResponseRef.h>
#include <WebKit/WKPagePrivate.h>
#include <WebKit/WKPreferencesRefPrivate.h>
#include <WebKit/WKProtectionSpace.h>
#include <WebKit/WKProtectionSpaceCurl.h>
#include <WebKit/WKSecurityOriginRef.h>
#include <WebKit/WKURLResponse.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKWebsiteDataStoreConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>
#include <WebKit/WKWebsiteDataStoreRefCurl.h>
#include <shellapi.h>
#include <atomic>
#include <sstream>
#include <thread>
#include <vector>

static std::uint64_t nextSyncPresentationToken()
{
    static std::atomic<std::uint64_t> next { 1 };
    auto value = next.fetch_add(1, std::memory_order_relaxed);
    if (!value)
        value = next.fetch_add(1, std::memory_order_relaxed);
    return value;
}

std::wstring createString(WKURLRef wkURL)
{
    if (!wkURL)
        return { };
    WKRetainPtr<WKStringRef> url = adoptWK(WKURLCopyString(wkURL));
    return createString(url.get());
}

std::string createUTF8String(const wchar_t* src, size_t srcLength)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, src, srcLength, 0, 0, nullptr, nullptr);
    std::vector<char> buffer(length);
    size_t actualLength = WideCharToMultiByte(CP_UTF8, 0, src, srcLength, buffer.data(), length, nullptr, nullptr);
    return { buffer.data(), actualLength };
}

std::wstring createPEMString(WKProtectionSpaceRef protectionSpace)
{
    auto chain = adoptWK(WKProtectionSpaceCopyCertificateChain(protectionSpace));

    std::wstring pems;

    for (size_t i = 0; i < WKArrayGetSize(chain.get()); i++) {
        auto item = WKArrayGetItemAtIndex(chain.get(), i);
        assert(WKGetTypeID(item) == WKDataGetTypeID());
        auto certificate = static_cast<WKDataRef>(item);
        auto size = WKDataGetSize(certificate);
        auto data = WKDataGetBytes(certificate);

        for (size_t i = 0; i < size; i++)
            pems.push_back(data[i]);
    }

    return replaceString(pems, L"\n", L"\r\n");
}

WKRetainPtr<WKStringRef> createWKString(_bstr_t str)
{
    auto utf8 = createUTF8String(str, str.length());
    return adoptWK(WKStringCreateWithUTF8CString(utf8.data()));
}

WKRetainPtr<WKStringRef> createWKString(const std::wstring& str)
{
    auto utf8 = createUTF8String(str.c_str(), str.length());
    return adoptWK(WKStringCreateWithUTF8CString(utf8.data()));
}

WKRetainPtr<WKStringRef> createWKString(const wchar_t* str)
{
    return createWKString(std::wstring(str));
}

WKRetainPtr<WKURLRef> createWKURL(_bstr_t str)
{
    auto utf8 = createUTF8String(str, str.length());
    return adoptWK(WKURLCreateWithUTF8CString(utf8.data()));
}

WKRetainPtr<WKURLRef> createWKURL(const std::wstring& str)
{
    auto utf8 = createUTF8String(str.c_str(), str.length());
    return adoptWK(WKURLCreateWithUTF8CString(utf8.data()));
}

Ref<BrowserWindow> WebKitBrowserWindow::create(BrowserWindowClient& client, HWND mainWnd, bool)
{
    auto websiteDataStoreConf = adoptWK(WKWebsiteDataStoreConfigurationCreate());

    auto websiteDataStore = adoptWK(WKWebsiteDataStoreCreateWithConfiguration(websiteDataStoreConf.get()));

    auto contextConf = adoptWK(WKContextConfigurationCreate());

    auto context = adoptWK(WKContextCreateWithConfiguration(contextConf.get()));

    auto preferences = adoptWK(WKPreferencesCreate());
    WKPreferencesSetMediaCapabilitiesEnabled(preferences.get(), false);
    WKPreferencesSetDeveloperExtrasEnabled(preferences.get(), false);

    auto pageConf = adoptWK(WKPageConfigurationCreate());
    WKPageConfigurationSetWebsiteDataStore(pageConf.get(), websiteDataStore.get());
    WKPageConfigurationSetContext(pageConf.get(), context.get());
    WKPageConfigurationSetPreferences(pageConf.get(), preferences.get());

    return adoptRef(*new WebKitBrowserWindow(client, pageConf.get(), mainWnd));
}

WebKitBrowserWindow::WebKitBrowserWindow(BrowserWindowClient& client, WKPageConfigurationRef pageConf, HWND mainWnd)
    : m_client(client)
    , m_syncPresentationToken(nextSyncPresentationToken())
    , m_hMainWnd(mainWnd)
{
    m_nativeCredentialPicker = XanhNativeCredentialPicker::shared();
    m_credentialBridge = std::make_unique<XanhCredentialBridge>(
        pageConf,
        false,
        [picker = m_nativeCredentialPicker, mainWnd](
            XanhCredentialBridgePolicy::Request request,
            XanhCredentialBridge::PickerCompletion completion) {
            picker->pick(mainWnd, std::move(request), std::move(completion));
        },
        [this] {
            return GetForegroundWindow() == m_hMainWnd && IsWindowVisible(m_hMainWnd) && !IsIconic(m_hMainWnd);
        },
        [picker = m_nativeCredentialPicker, mainWnd] {
            picker->cancel(mainWnd);
        });
    RECT rect = { };
    m_view = adoptWK(WKViewCreate(rect, pageConf, mainWnd));
    WKViewSetIsInWindow(m_view.get(), true);

    auto page = WKViewGetPage(m_view.get());
    m_credentialBridge->attachPage(page);
    WKPageNavigationClientV3 navigationClient = { };
    navigationClient.base.version = 3;
    navigationClient.base.clientInfo = this;
    navigationClient.decidePolicyForNavigationAction = decidePolicyForNavigationAction;
    navigationClient.decidePolicyForNavigationResponse = decidePolicyForNavigationResponse;
    navigationClient.didStartProvisionalNavigation = didStartProvisionalNavigation;
    navigationClient.didCommitNavigation = didCommitNavigation;
    navigationClient.didFinishNavigation = didFinishNavigation;
    navigationClient.didSameDocumentNavigation = didSameDocumentNavigation;
    navigationClient.didFailProvisionalNavigation = didFailProvisionalNavigation;
    navigationClient.didReceiveAuthenticationChallenge = didReceiveAuthenticationChallenge;
    navigationClient.navigationActionDidBecomeDownload = navigationActionDidBecomeDownload;
    navigationClient.navigationResponseDidBecomeDownload = navigationResponseDidBecomeDownload;
    navigationClient.contextMenuDidCreateDownload = contextMenuDidCreateDownload;
    navigationClient.webProcessDidTerminate = webProcessDidTerminate;
    WKPageSetPageNavigationClient(page, &navigationClient.base);

    WKPageUIClientV13 uiClient = { };
    uiClient.base.version = 13;
    uiClient.base.clientInfo = this;
    uiClient.createNewPage = createNewPage;
    uiClient.close = close;
    uiClient.didNotHandleKeyEvent = didNotHandleKeyEvent;
    uiClient.getWindowFrame = getWindowFrame;
    uiClient.runJavaScriptAlert = runJavaScriptAlert;
    uiClient.runJavaScriptConfirm = runJavaScriptConfirm;
    uiClient.runJavaScriptPrompt = runJavaScriptPrompt;
    WKPageSetPageUIClient(page, &uiClient.base);

    WKPageStateClientV0 stateClient = { };
    stateClient.base.version = 0;
    stateClient.base.clientInfo = this;
    stateClient.didChangeTitle = didChangeTitle;
    stateClient.didChangeIsLoading = didChangeIsLoading;
    stateClient.didChangeEstimatedProgress = didChangeEstimatedProgress;
    stateClient.didChangeActiveURL = didChangeActiveURL;
    WKPageSetPageStateClient(page, &stateClient.base);

    updateProxySettings();

    adjustScaleFactors();
}

WebKitBrowserWindow::~WebKitBrowserWindow()
{
    m_syncPresentationToken = 0;
    m_nativeCredentialPicker->windowClosed(m_hMainWnd);
}

void WebKitBrowserWindow::applicationActivationChanged(bool isActive, HWND otherWindow)
{
    m_nativeCredentialPicker->applicationActivationChanged(
        m_hMainWnd, isActive, otherWindow);
}

static void postSyncPresentation(HWND ownerWindow, std::uint64_t token,
    XanhFirefoxSyncPresentation presentation)
{
    if (ownerWindow && token)
        PostMessageW(ownerWindow, XanhFirefoxSyncResultMessage,
            static_cast<WPARAM>(token), static_cast<LPARAM>(presentation));
}

static XanhFirefoxSyncPresentation syncPresentation(
    XanhNativeCredentialPicker::SyncStatus status)
{
    switch (status) {
    case XanhNativeCredentialPicker::SyncStatus::success:
        return XanhFirefoxSyncPresentation::syncCompleted;
    case XanhNativeCredentialPicker::SyncStatus::partial:
        return XanhFirefoxSyncPresentation::syncPartial;
    case XanhNativeCredentialPicker::SyncStatus::networkError:
        return XanhFirefoxSyncPresentation::syncNetworkError;
    case XanhNativeCredentialPicker::SyncStatus::authError:
        return XanhFirefoxSyncPresentation::syncAuthError;
    case XanhNativeCredentialPicker::SyncStatus::backedOff:
        return XanhFirefoxSyncPresentation::syncBackedOff;
    }
    return XanhFirefoxSyncPresentation::syncFailed;
}

static bool registerFirefoxSyncProtocol()
{
    std::vector<wchar_t> executablePath(32768);
    auto pathLength = GetModuleFileNameW(nullptr, executablePath.data(),
        static_cast<DWORD>(executablePath.size()));
    if (!pathLength || pathLength >= executablePath.size())
        return false;

    constexpr auto protocolKey =
        L"Software\\Classes\\xanh-browser-wincairo";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, protocolKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr)
        != ERROR_SUCCESS)
        return false;
    constexpr wchar_t description[] = L"URL:Xanh Browser Firefox Sync";
    constexpr wchar_t empty[] = L"";
    bool success = RegSetValueExW(key, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(description),
                       sizeof(description))
            == ERROR_SUCCESS
        && RegSetValueExW(key, L"URL Protocol", 0, REG_SZ,
               reinterpret_cast<const BYTE*>(empty), sizeof(empty))
            == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!success)
        return false;

    HKEY commandKey = nullptr;
    std::wstring command = L"\"";
    command.append(executablePath.data(), pathLength);
    command += L"\" \"%1\"";
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\xanh-browser-wincairo\\shell\\open\\command",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &commandKey, nullptr)
        != ERROR_SUCCESS)
        return false;
    success = RegSetValueExW(commandKey, nullptr, 0, REG_SZ,
                  reinterpret_cast<const BYTE*>(command.c_str()),
                  static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
        == ERROR_SUCCESS;
    RegCloseKey(commandKey);
    SecureZeroMemory(command.data(), command.size() * sizeof(wchar_t));
    return success;
}

void WebKitBrowserWindow::beginFirefoxSync()
{
    if (!m_nativeCredentialPicker->canBeginOAuth(m_hMainWnd)) {
        MessageBoxW(m_hMainWnd,
            L"Firefox Sync is unavailable in this build. Install a configured signed native Sync package before signing in.",
            L"Xanh Browser", MB_OK | MB_ICONERROR);
        return;
    }
    if (!registerFirefoxSyncProtocol()) {
        MessageBoxW(m_hMainWnd,
            L"The per-user Firefox Sync callback handler could not be registered.",
            L"Xanh Browser", MB_OK | MB_ICONERROR);
        return;
    }
    auto url = m_nativeCredentialPicker->beginOAuth(m_hMainWnd);
    if (!url) {
        MessageBoxW(m_hMainWnd,
            L"Firefox Sync sign-in could not be started. Finish or retry the current Sync operation, then try again.",
            L"Xanh Browser", MB_OK | MB_ICONERROR);
        return;
    }
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(m_hMainWnd, L"open",
        url->view().data(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        auto message = m_nativeCredentialPicker->abandonOAuth(m_hMainWnd)
            ? L"The system browser could not be opened. The sign-in flow was discarded; try again."
            : L"The system browser could not be opened. Restart Xanh before retrying sign-in.";
        MessageBoxW(m_hMainWnd, message, L"Xanh Browser",
            MB_OK | MB_ICONERROR);
    }
}

bool WebKitBrowserWindow::handleFirefoxSyncCallback(std::wstring&& callbackURL)
{
    auto callback = XanhSensitiveWide::take(std::move(callbackURL));
    auto ownerWindow = m_hMainWnd;
    auto token = m_syncPresentationToken;
    return m_nativeCredentialPicker->completeOAuth(
        ownerWindow, callback.view(), [ownerWindow, token](bool success) {
        postSyncPresentation(ownerWindow, token,
            success ? XanhFirefoxSyncPresentation::signInCompleted
                    : XanhFirefoxSyncPresentation::signInFailed);
    });
}

bool WebKitBrowserWindow::canHandleFirefoxSyncCallback()
{
    return m_nativeCredentialPicker->canHandleOAuthCallback(m_hMainWnd);
}

void WebKitBrowserWindow::syncFirefoxSync()
{
    auto ownerWindow = m_hMainWnd;
    auto token = m_syncPresentationToken;
    m_nativeCredentialPicker->syncNow(ownerWindow,
        [ownerWindow, token](std::optional<XanhNativeCredentialPicker::SyncStatus> status) {
            postSyncPresentation(ownerWindow, token,
                status ? syncPresentation(*status)
                       : XanhFirefoxSyncPresentation::syncFailed);
        });
}

void WebKitBrowserWindow::disconnectFirefoxSync(bool deleteLocal)
{
    auto picker = m_nativeCredentialPicker;
    auto ownerWindow = m_hMainWnd;
    auto token = m_syncPresentationToken;
    // Thread construction must never unwind through the Win32 window procedure.
    try {
        std::thread([picker = std::move(picker), ownerWindow, token, deleteLocal] {
            bool success = picker->disconnect(deleteLocal);
            postSyncPresentation(ownerWindow, token,
                success
                    ? (deleteLocal
                        ? XanhFirefoxSyncPresentation::disconnectRemovedLocal
                        : XanhFirefoxSyncPresentation::disconnectKeptLocal)
                    : XanhFirefoxSyncPresentation::disconnectFailed);
        }).detach();
    } catch (...) {
        postSyncPresentation(ownerWindow, token,
            XanhFirefoxSyncPresentation::disconnectFailed);
    }
}

bool WebKitBrowserWindow::handleFirefoxSyncPresentation(
    WPARAM token, LPARAM rawPresentation)
{
    if (!m_syncPresentationToken
        || token != static_cast<WPARAM>(m_syncPresentationToken))
        return false;
    auto presentation = static_cast<XanhFirefoxSyncPresentation>(rawPresentation);
    const wchar_t* message = nullptr;
    UINT icon = MB_ICONINFORMATION;
    switch (presentation) {
    case XanhFirefoxSyncPresentation::signInCompleted:
        message = L"Firefox Sync sign-in completed. Choose Firefox Sync Now to verify Windows Hello, unlock the local password vault, and sync all four engines.";
        break;
    case XanhFirefoxSyncPresentation::signInFailed:
        message = L"Firefox Sync sign-in could not be completed. The callback was rejected or the account session is no longer valid.";
        icon = MB_ICONERROR;
        break;
    case XanhFirefoxSyncPresentation::syncCompleted:
        message = L"Firefox Sync completed for bookmarks, history, tabs and passwords.";
        break;
    case XanhFirefoxSyncPresentation::syncPartial:
        message = L"Firefox Sync completed partially. Retry later for the remaining engines.";
        icon = MB_ICONWARNING;
        break;
    case XanhFirefoxSyncPresentation::syncNetworkError:
        message = L"Firefox Sync could not reach the server. Check the network and retry later.";
        icon = MB_ICONWARNING;
        break;
    case XanhFirefoxSyncPresentation::syncAuthError:
        message = L"Firefox Sync needs account authentication. Sign in again before retrying.";
        icon = MB_ICONWARNING;
        break;
    case XanhFirefoxSyncPresentation::syncBackedOff:
        message = L"Firefox Sync is temporarily backed off by the server. Retry later.";
        icon = MB_ICONWARNING;
        break;
    case XanhFirefoxSyncPresentation::syncFailed:
        message = L"Firefox Sync could not run. Keep Xanh in the foreground, finish any current operation, and try again.";
        icon = MB_ICONERROR;
        break;
    case XanhFirefoxSyncPresentation::disconnectKeptLocal:
        message = L"Firefox Sync was disconnected; local browser data was kept.";
        break;
    case XanhFirefoxSyncPresentation::disconnectRemovedLocal:
        message = L"Firefox Sync and its local device data were removed.";
        break;
    case XanhFirefoxSyncPresentation::disconnectFailed:
        message = L"Firefox Sync could not finish disconnecting. Retry the same choice so the durable cleanup can resume.";
        icon = MB_ICONERROR;
        break;
    default:
        return false;
    }
    MessageBoxW(m_hMainWnd, message, L"Xanh Browser", MB_OK | icon);
    return true;
}

void WebKitBrowserWindow::updateProxySettings()
{
    auto page = WKViewGetPage(m_view.get());
    auto websiteDataStore = WKPageGetWebsiteDataStore(page);

    if (!m_proxy.enable) {
        WKWebsiteDataStoreDisableNetworkProxySettings(websiteDataStore);
        return;
    }

    if (!m_proxy.custom) {
        WKWebsiteDataStoreEnableDefaultNetworkProxySettings(websiteDataStore);
        return;
    }

    auto url = createWKURL(m_proxy.url);
    auto excludeHosts = createWKString(m_proxy.excludeHosts);
    WKWebsiteDataStoreEnableCustomNetworkProxySettings(websiteDataStore, url.get(), excludeHosts.get());
}

// FIXME: The current design of WebKit produces too many noises on fractional device scale factor.
// This rounds device scale factor for tantative workarond.
void WebKitBrowserWindow::adjustScaleFactors()
{
    WKPageRef page = WKViewGetPage(m_view.get());
    WKPageSetCustomBackingScaleFactor(page, std::round(WebCore::deviceScaleFactorForWindow(hwnd())));
}

HRESULT WebKitBrowserWindow::init()
{
    return S_OK;
}

void WebKitBrowserWindow::resetFeatureMenu(FeatureType featureType, HMENU menu, bool resetsSettingsToDefaults)
{
    auto page = WKViewGetPage(m_view.get());
    auto configuration = adoptWK(WKPageCopyPageConfiguration(page));
    auto pref = WKPageConfigurationGetPreferences(configuration.get());
    switch (featureType) {
    case FeatureType::Experimental: {
        auto features = adoptWK(WKPreferencesCopyExperimentalFeatures(pref));
        auto size = WKArrayGetSize(features.get());
        for (size_t i = 0; i < size; i++) {
            auto item = WKArrayGetItemAtIndex(features.get(), i);
            assert(WKGetTypeID(item) == WKFeatureGetTypeID());
            auto feature = static_cast<WKFeatureRef>(item);
            auto name = createString(adoptWK(WKFeatureCopyName(feature)).get());
            auto key = adoptWK(WKFeatureCopyKey(feature));
            bool defaultValue = WKFeatureDefaultValue(feature);
            if (resetsSettingsToDefaults) {
                auto flag = MF_BYCOMMAND | (defaultValue ? MF_CHECKED : MF_UNCHECKED);
                CheckMenuItem(menu, IDM_EXPERIMENTAL_FEATURES_BEGIN + i, flag);
                WKPreferencesSetExperimentalFeatureForKey(pref, defaultValue, key.get());
            } else {
                auto flag = MF_STRING | (defaultValue ? MF_CHECKED : MF_UNCHECKED);
                AppendMenu(menu, flag, IDM_EXPERIMENTAL_FEATURES_BEGIN + i, name.c_str());
                m_experimentalFeatureKeys.push_back(std::move(key));
            }
        }
        break;
    }
    case FeatureType::InternalDebug: {
        auto features = adoptWK(WKPreferencesCopyInternalDebugFeatures(pref));
        auto size = WKArrayGetSize(features.get());
        for (size_t i = 0; i < size; i++) {
            auto item = WKArrayGetItemAtIndex(features.get(), i);
            assert(WKGetTypeID(item) == WKFeatureGetTypeID());
            auto feature = static_cast<WKFeatureRef>(item);
            auto name = createString(adoptWK(WKFeatureCopyName(feature)).get());
            auto key = adoptWK(WKFeatureCopyKey(feature));
            bool defaultValue = WKFeatureDefaultValue(feature);
            if (resetsSettingsToDefaults) {
                auto flag = MF_BYCOMMAND | (defaultValue ? MF_CHECKED : MF_UNCHECKED);
                CheckMenuItem(menu, IDM_INTERNAL_DEBUG_FEATURES_BEGIN + i, flag);
                WKPreferencesSetInternalDebugFeatureForKey(pref, defaultValue, key.get());
            } else {
                auto flag = MF_STRING | (defaultValue ? MF_CHECKED : MF_UNCHECKED);
                AppendMenu(menu, flag, IDM_INTERNAL_DEBUG_FEATURES_BEGIN + i, name.c_str());
                m_internalDebugFeatureKeys.push_back(std::move(key));
            }
        }
        break;
    }
    }
}

HWND WebKitBrowserWindow::hwnd()
{
    return WKViewGetWindow(m_view.get());
}

HRESULT WebKitBrowserWindow::loadURL(const BSTR& url)
{
    m_processRecoveryPolicy.userRequestedNavigation();
    auto page = WKViewGetPage(m_view.get());
    WKPageLoadURL(page, createWKURL(_bstr_t(url)).get());
    return true;
}

void WebKitBrowserWindow::reload()
{
    m_processRecoveryPolicy.userRequestedNavigation();
    auto page = WKViewGetPage(m_view.get());
    WKPageReload(page);
}

void WebKitBrowserWindow::navigateForwardOrBackward(bool forward)
{
    m_processRecoveryPolicy.userRequestedNavigation();
    auto page = WKViewGetPage(m_view.get());
    if (forward)
        WKPageGoForward(page);
    else
        WKPageGoBack(page);
}

void WebKitBrowserWindow::navigateToHistory(UINT)
{
    // Not implemented
}

void WebKitBrowserWindow::setPreference(UINT menuID, bool enable)
{
    auto page = WKViewGetPage(m_view.get());
    auto configuration = adoptWK(WKPageCopyPageConfiguration(page));
    auto pref = WKPageConfigurationGetPreferences(configuration.get());
    if (IDM_EXPERIMENTAL_FEATURES_BEGIN <= menuID && menuID <= IDM_EXPERIMENTAL_FEATURES_END) {
        int index = menuID - IDM_EXPERIMENTAL_FEATURES_BEGIN;
        WKPreferencesSetExperimentalFeatureForKey(pref, enable, m_experimentalFeatureKeys[index].get());
        return;
    }
    if (IDM_INTERNAL_DEBUG_FEATURES_BEGIN <= menuID && menuID <= IDM_INTERNAL_DEBUG_FEATURES_END) {
        int index = menuID - IDM_INTERNAL_DEBUG_FEATURES_BEGIN;
        WKPreferencesSetInternalDebugFeatureForKey(pref, enable, m_internalDebugFeatureKeys[index].get());
        return;
    }
    switch (menuID) {
    case IDM_ACC_COMPOSITING:
        WKPreferencesSetAcceleratedCompositingEnabled(pref, enable);
        break;
    case IDM_COMPOSITING_BORDERS:
        WKPreferencesSetCompositingBordersVisible(pref, enable);
        WKPreferencesSetCompositingRepaintCountersVisible(pref, enable);
        break;
    case IDM_DEBUG_INFO_LAYER:
        WKPreferencesSetTiledScrollingIndicatorVisible(pref, enable);
        break;
    case IDM_DISABLE_IMAGES:
        WKPreferencesSetLoadsImagesAutomatically(pref, !enable);
        break;
    case IDM_DISABLE_JAVASCRIPT:
        WKPreferencesSetJavaScriptEnabled(pref, !enable);
        break;
    }
}

void WebKitBrowserWindow::print()
{
    // Not implemented
}

void WebKitBrowserWindow::launchInspector()
{
    auto page = WKViewGetPage(m_view.get());
    auto inspector = WKPageGetInspector(page);
    WKInspectorShow(inspector);
}

void WebKitBrowserWindow::openProxySettings()
{
    if (askProxySettings(m_hMainWnd, m_proxy))
        updateProxySettings();

}

void WebKitBrowserWindow::setUserAgent(_bstr_t& customUAString)
{
    auto page = WKViewGetPage(m_view.get());
    auto ua = createWKString(customUAString);
    WKPageSetCustomUserAgent(page, ua.get());
}

_bstr_t WebKitBrowserWindow::userAgent()
{
    auto page = WKViewGetPage(m_view.get());
    auto ua = adoptWK(WKPageCopyUserAgent(page));
    return createString(ua.get()).c_str();
}

void WebKitBrowserWindow::showLayerTree()
{
    auto page = WKViewGetPage(m_view.get());
    auto name = createWKString("DumpLayerTree");
    WKPagePostMessageToInjectedBundle(page, name.get(), nullptr);
}

void WebKitBrowserWindow::updateStatistics(HWND)
{
    // Not implemented
}


void WebKitBrowserWindow::resetZoom()
{
    auto page = WKViewGetPage(m_view.get());
    WKPageSetPageZoomFactor(page, 1);
}

void WebKitBrowserWindow::zoomIn()
{
    auto page = WKViewGetPage(m_view.get());
    double s = WKPageGetPageZoomFactor(page);
    WKPageSetPageZoomFactor(page, s * 1.25);
}

void WebKitBrowserWindow::zoomOut()
{
    auto page = WKViewGetPage(m_view.get());
    double s = WKPageGetPageZoomFactor(page);
    WKPageSetPageZoomFactor(page, s * 0.8);
}

static void deleteAllCookiesCallback(void*)
{

}

void WebKitBrowserWindow::clearCookies()
{
    auto page = WKViewGetPage(m_view.get());
    auto websiteDataStore = WKPageGetWebsiteDataStore(page);

    WKHTTPCookieStoreRef cookieStore = WKWebsiteDataStoreGetHTTPCookieStore(websiteDataStore);
    if (cookieStore)
        WKHTTPCookieStoreDeleteAllCookies(cookieStore, this, deleteAllCookiesCallback);
}

static void removeAllFetchCachesCallback(void*)
{
}

static void removeNetworkCacheCallback(void*)
{
}

static void removeMemoryCachesCallback(void*)
{
}

static void removeAllServiceWorkerRegistrationsCallback(void*)
{
}

static void removeAllIndexedDatabasesCallback(void*)
{
}

static void removeLocalStorageCallback(void*)
{
}
    
void WebKitBrowserWindow::clearWebsiteData()
{
    auto page = WKViewGetPage(m_view.get());
    auto websiteDataStore = WKPageGetWebsiteDataStore(page);

    WKWebsiteDataStoreRemoveAllFetchCaches(websiteDataStore, this, removeAllFetchCachesCallback);
    WKWebsiteDataStoreRemoveNetworkCache(websiteDataStore, this, removeNetworkCacheCallback);
    WKWebsiteDataStoreRemoveMemoryCaches(websiteDataStore, this, removeMemoryCachesCallback);
    WKWebsiteDataStoreRemoveAllServiceWorkerRegistrations(websiteDataStore, this, removeAllServiceWorkerRegistrationsCallback);
    WKWebsiteDataStoreRemoveAllIndexedDatabases(websiteDataStore, this, removeAllIndexedDatabasesCallback);
    WKWebsiteDataStoreRemoveLocalStorage(websiteDataStore, this, removeLocalStorageCallback);
}

static WebKitBrowserWindow& toWebKitBrowserWindow(const void *clientInfo)
{
    return *const_cast<WebKitBrowserWindow*>(static_cast<const WebKitBrowserWindow*>(clientInfo));
}

void WebKitBrowserWindow::didChangeTitle(const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    auto page = WKViewGetPage(thisWindow.m_view.get());
    WKRetainPtr<WKStringRef> title = adoptWK(WKPageCopyTitle(page));
    std::wstring titleString = createString(title.get()) + L" [Xanh Browser WebKit]";
    SetWindowText(thisWindow.m_hMainWnd, titleString.c_str());
}

void WebKitBrowserWindow::didChangeIsLoading(const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    thisWindow.m_client.progressFinished();
}

void WebKitBrowserWindow::didChangeEstimatedProgress(const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    auto page = WKViewGetPage(thisWindow.m_view.get());
    thisWindow.m_client.progressChanged(WKPageGetEstimatedProgress(page));
}

void WebKitBrowserWindow::didChangeActiveURL(const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    auto page = WKViewGetPage(thisWindow.m_view.get());
    WKRetainPtr<WKURLRef> url = adoptWK(WKPageCopyActiveURL(page));
    thisWindow.m_client.activeURLChanged(createString(url.get()));
}

void WebKitBrowserWindow::decidePolicyForNavigationAction(WKPageRef, WKNavigationActionRef navigationAction, WKFramePolicyListenerRef listener, WKTypeRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    WKRetainPtr<WKURLRequestRef> request = adoptWK(WKNavigationActionCopyRequest(navigationAction));
    WKRetainPtr<WKURLRef> url;
    if (request)
        url = adoptWK(WKURLRequestCopyURL(request.get()));
    WKRetainPtr<WKStringRef> urlString;
    if (url)
        urlString = adoptWK(WKURLCopyString(url.get()));
    if (!urlString || WKStringGetLength(urlString.get()) > XanhNavigationPolicy::maximumURLCharacters) {
        WKFramePolicyListenerIgnore(listener);
        return;
    }

    auto urlValue = createString(urlString.get());
    bool hasUnconsumedUserGesture = WKNavigationActionHasUnconsumedUserGesture(navigationAction);
    bool isTrustedLinkClick = WKXanhNavigationActionIsTrustedLinkClick(navigationAction);
    bool isRedirect = WKXanhNavigationActionIsRedirect(navigationAction);
    bool isMainFrame = false;
    auto targetFrame = adoptWK(WKNavigationActionCopyTargetFrameInfo(navigationAction));
    if (targetFrame)
        isMainFrame = WKFrameInfoGetIsMainFrame(targetFrame.get());
    auto decision = XanhNavigationPolicy::decide(urlValue,
        hasUnconsumedUserGesture,
        isTrustedLinkClick,
        isRedirect,
        WKNavigationActionGetShouldOpenExternalSchemes(navigationAction));
    if (decision == XanhNavigationPolicy::Decision::allowInWebView) {
        if (isMainFrame && hasUnconsumedUserGesture && isTrustedLinkClick && !isRedirect)
            thisWindow.m_processRecoveryPolicy.userRequestedNavigation();
        WKFramePolicyListenerUse(listener);
        return;
    }

    bool shouldOpenExternal = decision == XanhNavigationPolicy::Decision::openExternal && WKXanhNavigationActionTakeUserGesture(navigationAction);
    WKFramePolicyListenerIgnore(listener);
    if (!shouldOpenExternal)
        return;

    ShellExecuteW(thisWindow.m_hMainWnd, L"open", urlValue.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void WebKitBrowserWindow::decidePolicyForNavigationResponse(WKPageRef, WKNavigationResponseRef navigationResponse, WKFramePolicyListenerRef listener, WKTypeRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    auto response = adoptWK(WKNavigationResponseCopyResponse(navigationResponse));
    auto code = WKURLResponseHTTPStatusCode(response.get());
    auto expectedContentLength = WKURLResponseGetExpectedContentLength(response.get());
    if (code > 300 && !expectedContentLength) {
        std::wstringstream text;
        text << L"Error Code: " << code << std::endl;
        text << L"MIME type: " << createString(adoptWK(WKURLResponseCopyMIMEType(response.get())).get()) << std::endl;
        text << L"URL: " << createString(adoptWK(WKURLResponseCopyURL(response.get())).get());
        MessageBox(thisWindow.m_hMainWnd, text.str().c_str(), L"No Content", MB_OK | MB_ICONWARNING);
    }

    auto isMainFrame = WKFrameInfoGetIsMainFrame(adoptWK(WKNavigationResponseCopyFrameInfo(navigationResponse)).get());
    if (WKNavigationResponseCanShowMIMEType(navigationResponse) || !isMainFrame)
        WKFramePolicyListenerUse(listener);
    else {
        std::wstringstream text;
        text << L"Do you want to save this file?" << std::endl;
        text << std::endl;
        text << L"MIME type: " << createString(adoptWK(WKURLResponseCopyMIMEType(response.get())).get()) << std::endl;
        text << L"URL: " << createString(adoptWK(WKURLResponseCopyURL(response.get())).get());

        if (MessageBox(thisWindow.hwnd(), text.str().c_str(), L"Unsupported MIME type", MB_OKCANCEL | MB_ICONWARNING) == IDOK)
            WKFramePolicyListenerDownload(listener);
        else
            WKFramePolicyListenerIgnore(listener);
    }
}

void WebKitBrowserWindow::didFinishNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    thisWindow.m_processRecoveryPolicy.navigationFinished();
}

void WebKitBrowserWindow::didStartProvisionalNavigation(WKPageRef page, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    thisWindow.m_credentialBridge->navigationStarted(page);
}

void WebKitBrowserWindow::didCommitNavigation(WKPageRef page, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    thisWindow.m_credentialBridge->navigationFinished(page);
}

void WebKitBrowserWindow::didSameDocumentNavigation(WKPageRef page, WKNavigationRef, WKSameDocumentNavigationType, WKTypeRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    thisWindow.m_credentialBridge->activeURLChanged(page);
}

void WebKitBrowserWindow::webProcessDidTerminate(WKPageRef page, WKProcessTerminationReason reason, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    thisWindow.m_credentialBridge->rendererTerminated(page);
    auto action = thisWindow.m_processRecoveryPolicy.processTerminated(
        reason == kWKProcessTerminationReasonRequestedByClient,
        WKXanhPageCanSafelyRestoreCurrentItem(page));
    if (action == XanhProcessRecovery::Action::ignoreRequestedTermination)
        return;
    if (action == XanhProcessRecovery::Action::restorePage) {
        WKPageReload(page);
        return;
    }

    if (WKPageGetIsControlledByAutomation(page))
        return;

    const wchar_t* detail = action == XanhProcessRecovery::Action::refuseUnsafeRestore
        ? L"The web process stopped during a navigation that cannot be replayed safely. Automatic reload was blocked to avoid resubmitting data. Use the address bar to continue."
        : L"The web process stopped repeatedly. Automatic recovery was paused to avoid a crash loop. Use Reload to try again.";
    if (action == XanhProcessRecovery::Action::refuseUnsafeRestore) {
        MessageBox(thisWindow.m_hMainWnd, detail, L"Web Process Stopped", MB_OK | MB_ICONWARNING);
        return;
    }
    switch (reason) {
    case kWKProcessTerminationReasonExceededMemoryLimit:
        detail = L"The web process repeatedly exceeded its memory limit. Automatic recovery was paused. Use Reload to try again.";
        break;
    case kWKProcessTerminationReasonExceededCPULimit:
        detail = L"The web process repeatedly exceeded its CPU limit. Automatic recovery was paused. Use Reload to try again.";
        break;
    case kWKProcessTerminationReasonCrash:
        detail = L"The web process crashed repeatedly. Automatic recovery was paused. Use Reload to try again.";
        break;
    default:
        break;
    }
    MessageBox(thisWindow.m_hMainWnd, detail, L"Web Process Stopped", MB_OK | MB_ICONWARNING);
}

void WebKitBrowserWindow::didFailProvisionalNavigation(WKPageRef page, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    thisWindow.m_credentialBridge->activeURLChanged(page);
    std::wstringstream text;
    text << createString(adoptWK(WKErrorCopyLocalizedDescription(error)).get()) << std::endl;
    text << L"Error Code: " << WKErrorGetErrorCode(error) << std::endl;
    text << L"Domain: " << createString(adoptWK(WKErrorCopyDomain(error)).get()) << std::endl;
    text << L"Failing URL: " << createString(adoptWK(WKErrorCopyFailingURL(error)).get());
    MessageBox(thisWindow.m_hMainWnd, text.str().c_str(), L"Provisional Navigation Failure", MB_OK | MB_ICONWARNING);
}

std::wstring toAuthText(WKProtectionSpaceRef protectionSpace)
{
    auto scheme = [&] {
        switch (WKProtectionSpaceGetAuthenticationScheme(protectionSpace)) {
        case kWKProtectionSpaceAuthenticationSchemeDefault:
            return L"Default";
        case kWKProtectionSpaceAuthenticationSchemeHTTPBasic:
            return L"HTTPBasic";
        case kWKProtectionSpaceAuthenticationSchemeHTTPDigest:
            return L"HTTPDigest";
        case kWKProtectionSpaceAuthenticationSchemeHTMLForm:
            return L"HTMLForm";
        case kWKProtectionSpaceAuthenticationSchemeNTLM:
            return L"NTLM";
        case kWKProtectionSpaceAuthenticationSchemeNegotiate:
            return L"Negotiate";
        case kWKProtectionSpaceAuthenticationSchemeClientCertificateRequested:
            return L"ClientCertificateRequested";
        case kWKProtectionSpaceAuthenticationSchemeServerTrustEvaluationRequested:
            return L"ServerTrustEvaluationRequested";
        case kWKProtectionSpaceAuthenticationSchemeOAuth:
            return L"OAuth";
        case kWKProtectionSpaceAuthenticationSchemeUnknown:
            return L"Unknown";
        }
        return L"Invalid Value";
    }();

    auto serverType = [&] {
        switch (WKProtectionSpaceGetServerType(protectionSpace)) {
        case kWKProtectionSpaceServerTypeHTTP:
            return L"ServerTypeHTTP";
        case kWKProtectionSpaceServerTypeHTTPS:
            return L"ServerTypeHTTPS";
        case kWKProtectionSpaceServerTypeFTP:
            return L"ServerTypeFTP";
        case kWKProtectionSpaceServerTypeFTPS:
            return L"ServerTypeFTPS";
        case kWKProtectionSpaceProxyTypeHTTP:
            return L"ProxyTypeHTTP";
        case kWKProtectionSpaceProxyTypeHTTPS:
            return L"ProxyTypeHTTPS";
        case kWKProtectionSpaceProxyTypeFTP:
            return L"ProxyTypeFTP";
        case kWKProtectionSpaceProxyTypeSOCKS:
            return L"ProxyTypeSOCKS";
        }
        return L"Invalid Value";
    }();

    auto host = createString(adoptWK(WKProtectionSpaceCopyHost(protectionSpace)).get());
    auto realm = createString(adoptWK(WKProtectionSpaceCopyRealm(protectionSpace)).get());
    auto isProxy = WKProtectionSpaceGetIsProxy(protectionSpace);

    std::wstringstream text;
    text << L"Scheme: " << scheme << L"\r\n";
    text << L"Host: " << host << L"\r\n";
    text << L"Port: " << WKProtectionSpaceGetPort(protectionSpace) << L"\r\n";
    text << L"Server Type: " << serverType << L"\r\n";
    text << L"isProxy: " << isProxy << L"\r\n";
    text << L"Realm: " << realm << L"\r\n";
    return text.str();
}

void WebKitBrowserWindow::didReceiveAuthenticationChallenge(WKPageRef page, WKAuthenticationChallengeRef challenge, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    auto protectionSpace = WKAuthenticationChallengeGetProtectionSpace(challenge);
    auto decisionListener = WKAuthenticationChallengeGetDecisionListener(challenge);
    auto authenticationScheme = WKProtectionSpaceGetAuthenticationScheme(protectionSpace);

    if (WKPageGetIsControlledByAutomation(page)) {
        WKRetainPtr<WKStringRef> username = createWKString("accept server trust");
        WKRetainPtr<WKStringRef> password = createWKString("");
        WKRetainPtr<WKCredentialRef> wkCredential = adoptWK(WKCredentialCreate(username.get(), password.get(), kWKCredentialPersistenceForSession));
        WKAuthenticationDecisionListenerUseCredential(decisionListener, wkCredential.get());
        return;
    }

    if (authenticationScheme == kWKProtectionSpaceAuthenticationSchemeServerTrustEvaluationRequested) {
        if (thisWindow.canTrustServerCertificate(protectionSpace)) {
            WKRetainPtr<WKStringRef> username = createWKString("accept server trust");
            WKRetainPtr<WKStringRef> password = createWKString("");
            WKRetainPtr<WKCredentialRef> wkCredential = adoptWK(WKCredentialCreate(username.get(), password.get(), kWKCredentialPersistenceForSession));
            WKAuthenticationDecisionListenerUseCredential(decisionListener, wkCredential.get());
            return;
        }
    } else {
        if (auto credential = askCredential(thisWindow.hwnd(), toAuthText(protectionSpace))) {
            WKRetainPtr<WKStringRef> username = createWKString(credential->username);
            WKRetainPtr<WKStringRef> password = createWKString(credential->password);
            WKRetainPtr<WKCredentialRef> wkCredential = adoptWK(WKCredentialCreate(username.get(), password.get(), kWKCredentialPersistenceForSession));
            WKAuthenticationDecisionListenerUseCredential(decisionListener, wkCredential.get());
            return;
        }
    }

    WKAuthenticationDecisionListenerUseCredential(decisionListener, nullptr);
}

bool WebKitBrowserWindow::canTrustServerCertificate(WKProtectionSpaceRef protectionSpace)
{
    auto host = createString(adoptWK(WKProtectionSpaceCopyHost(protectionSpace)).get());
    auto verificationError = WKProtectionSpaceGetCertificateVerificationError(protectionSpace);
    auto description = createString(adoptWK(WKProtectionSpaceCopyCertificateVerificationErrorDescription(protectionSpace)).get());
    auto pem = createPEMString(protectionSpace);

    auto it = m_acceptedServerTrustCerts.find(host);
    if (it != m_acceptedServerTrustCerts.end() && it->second == pem)
        return true;

    std::wstring textString = L"[HOST] " + host + L"\r\n";
    textString.append(L"[ERROR] " + std::to_wstring(verificationError) + L"\r\n");
    textString.append(L"[DESCRIPTION] " + description + L"\r\n");
    textString.append(pem);

    if (askServerTrustEvaluation(hwnd(), textString)) {
        m_acceptedServerTrustCerts.emplace(host, pem);
        return true;
    }

    return false;
}

void WebKitBrowserWindow::navigationActionDidBecomeDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef download, const void* clientInfo)
{
    setDownloadClient(download, clientInfo);
}

void WebKitBrowserWindow::navigationResponseDidBecomeDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef download, const void* clientInfo)
{
    setDownloadClient(download, clientInfo);
}

void WebKitBrowserWindow::contextMenuDidCreateDownload(WKPageRef, WKDownloadRef download, const void* clientInfo)
{
    setDownloadClient(download, clientInfo);
}

void WebKitBrowserWindow::setDownloadClient(WKDownloadRef download, const void* clientInfo)
{
    WKDownloadClientV0 client = { };
    client.base = { 0, clientInfo };
    client.decideDestinationWithResponse = downloadDecideDestinationWithResponse;
    client.didFinish = downloadDidFinish;
    client.didFailWithError = downloadDidFailWithError;

    WKDownloadSetClient(download, &client.base);
}

WKStringRef WebKitBrowserWindow::downloadDecideDestinationWithResponse(WKDownloadRef, WKURLResponseRef response, WKStringRef suggestedFilename, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);

    auto filename = createString(suggestedFilename);
    if (filename.empty()) {
        auto urlRef = adoptWK(WKURLResponseCopyURL(response));
        auto hostRef = adoptWK(WKURLCopyHostName(urlRef.get()));
        filename = createString(hostRef.get());
    }

    std::wstring folderPath;
    if (!getKnownFolderPath(FOLDERID_Downloads, folderPath)) {
        MessageBox(thisWindow.hwnd(), L"Could not get Downloads folder path.", L"Download Failure", MB_OK | MB_ICONWARNING);
        return nullptr;
    }

    return createWKString(folderPath + L"\\" + filename).leakRef();
}

void WebKitBrowserWindow::downloadDidFinish(WKDownloadRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    MessageBox(thisWindow.hwnd(), L"File has been downloaded successfully.", L"Download", MB_OK);
}

void WebKitBrowserWindow::downloadDidFailWithError(WKDownloadRef, WKErrorRef error, WKDataRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    std::wstringstream text;
    text << createString(adoptWK(WKErrorCopyLocalizedDescription(error)).get()) << std::endl;
    text << L"Error Code: " << WKErrorGetErrorCode(error) << std::endl;
    text << L"Domain: " << createString(adoptWK(WKErrorCopyDomain(error)).get()) << std::endl;
    text << L"Failing URL: " << createString(adoptWK(WKErrorCopyFailingURL(error)).get());
    MessageBox(thisWindow.hwnd(), text.str().c_str(), L"Download Failure", MB_OK | MB_ICONWARNING);
}

void WebKitBrowserWindow::close(WKPageRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    PostMessage(thisWindow.m_hMainWnd, WM_CLOSE, 0, 0);
}

WKPageRef WebKitBrowserWindow::createNewPage(WKPageRef, WKPageConfigurationRef pageConf, WKNavigationActionRef, WKWindowFeaturesRef, const void*)
{
    auto& newWindow = MainWindow::create().leakRef();
    auto factory = [pageConf](BrowserWindowClient& client, HWND mainWnd, bool) -> auto {
        return adoptRef(*new WebKitBrowserWindow(client, pageConf, mainWnd));
    };
    bool ok = newWindow.init(factory, hInst);
    if (!ok)
        return nullptr;
    ShowWindow(newWindow.hwnd(), SW_SHOW);
    auto& newBrowserWindow = *static_cast<WebKitBrowserWindow*>(newWindow.browserWindow());
    WKRetainPtr<WKPageRef> newPage = WKViewGetPage(newBrowserWindow.m_view.get());
    return newPage.leakRef();
}

void WebKitBrowserWindow::didNotHandleKeyEvent(WKPageRef, WKNativeEventPtr event, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    PostMessage(thisWindow.m_hMainWnd, event->message, event->wParam, event->lParam);
}

WKRect WebKitBrowserWindow::getWindowFrame(WKPageRef, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    RECT rect;
    GetWindowRect(thisWindow.hwnd(), &rect);
    return WKRectMake(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

void WebKitBrowserWindow::runJavaScriptAlert(WKPageRef, WKStringRef alertText, WKFrameRef, WKSecurityOriginRef securityOrigin, WKPageRunJavaScriptAlertResultListenerRef listener, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    std::wstring title = L"Alert: ";
    title += createString(adoptWK(WKSecurityOriginCopyToString(securityOrigin)).get());
    auto text = createString(alertText);
    MessageBox(thisWindow.m_hMainWnd, text.c_str(), title.c_str(), MB_OK);
    WKPageRunJavaScriptAlertResultListenerCall(listener);
}

void WebKitBrowserWindow::runJavaScriptConfirm(WKPageRef, WKStringRef message, WKFrameRef, WKSecurityOriginRef securityOrigin, WKPageRunJavaScriptConfirmResultListenerRef listener, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    std::wstring title = L"Confirm: ";
    title += createString(adoptWK(WKSecurityOriginCopyToString(securityOrigin)).get());
    auto text = createString(message);
    bool result = MessageBox(thisWindow.m_hMainWnd, text.c_str(), title.c_str(), MB_OKCANCEL) == IDOK;
    WKPageRunJavaScriptConfirmResultListenerCall(listener, result);
}

void WebKitBrowserWindow::runJavaScriptPrompt(WKPageRef, WKStringRef message, WKStringRef defaultValue, WKFrameRef, WKSecurityOriginRef securityOrigin, WKPageRunJavaScriptPromptResultListenerRef listener, const void* clientInfo)
{
    auto& thisWindow = toWebKitBrowserWindow(clientInfo);
    std::wstring title = L"Prompt: ";
    title += createString(adoptWK(WKSecurityOriginCopyToString(securityOrigin)).get());
    auto text = createString(message);
    text += L"\nDefault Value: " + createString(defaultValue);
    MessageBox(thisWindow.m_hMainWnd, text.c_str(), title.c_str(), MB_OK);
    WKPageRunJavaScriptPromptResultListenerCall(listener, defaultValue);
}

set(MiniBrowser_PRIVATE_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
)

set(MiniBrowser_SOURCES
    BrowserWindow.cpp
    Common.cpp
    Common2.cpp
    MainWindow.cpp
    MiniBrowser.exe.manifest
    MiniBrowserLib.rc
    WebKitBrowserWindow.cpp
    WinMain.cpp
    XanhCredentialBridge.cpp
    XanhCredentialRecords.cpp
    XanhDpapiSecretStore.cpp
    XanhNativeCredentialPicker.cpp
    XanhNativeSyncLibrary.cpp
    XanhNativeSyncRuntime.cpp
    XanhOAuthCallback.cpp
    XanhWindowsHello.cpp
    XanhPortableBackup.cpp
    stdafx.cpp
)

set(MiniBrowser_PRIVATE_DEFINITIONS _UNICODE)
set(MiniBrowser_PRIVATE_LIBRARIES
    advapi32
    comctl32
    bcrypt
    crypt32
    normaliz
    ole32
    shell32
    shlwapi
    user32
    uuid
    wintrust
    windowsapp
)
set(MiniBrowser_FRAMEWORKS
    JavaScriptCore
    PAL
    WTF
    WebCore
    WebKit
)

add_library(MiniBrowserInjectedBundle SHARED
    Common2.cpp
    InjectedBundle.cpp
)
target_link_libraries(MiniBrowserInjectedBundle WebKit::WebKit)
target_include_directories(MiniBrowserInjectedBundle PRIVATE ${CMAKE_BINARY_DIR})

WEBKIT_EXECUTABLE_DECLARE(MiniBrowser)
WEBKIT_EXECUTABLE(MiniBrowser)

set_target_properties(MiniBrowser PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME "XanhBrowser.WebKit")

/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "WindowsSDKAssistant/Common.h"

void setupProjectTemplates()
{
    auto documentsPath = std::array<wchar_t, MAX_PATH>();

    if (FAILED(SHGetFolderPath(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documentsPath.data())))
        return;

    auto templateDirectory =
        std::wstring(documentsPath.data()) + L"\\Visual Studio 18\\Templates\\ProjectTemplates\\Carbon";

    auto installedTemplate = templateDirectory + L"\\CarbonApplication.zip";

    // Remove any existing template during both installation and uninstallation
    shellOperation(FO_DELETE, installedTemplate, L"");

    if (deleteMode)
        return;

    // Create the destination directory and install the modern Visual Studio project template
    auto result = SHCreateDirectoryEx(nullptr, templateDirectory.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
        return;

    shellOperation(
        FO_COPY,
        sdkPath + L"\\ProjectTemplate\\CarbonApplication.zip",
        templateDirectory);
}

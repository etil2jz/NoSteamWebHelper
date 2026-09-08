#include "config.h"
#include <shlobj.h>

// Global configuration instance
CONFIG g_Config = {0};

// Default configuration values
const CONFIG g_DefaultConfig = {
    .mode = MODE_AGGRESSIVE,
    .enableTrayTooltip = TRUE,
    .showRamUsage = TRUE,
    .autoDetectSiSR = TRUE,
    .iniPath = {0}
};

// Get configuration directory (AppData\Roaming\NoSteamWebHelper)
WCHAR* GetConfigDirectory(WCHAR* buffer, DWORD size)
{
    if (!buffer || size == 0)
        return NULL;
    
    // Get AppData\Roaming directory
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, buffer)))
    {
        // Append \NoSteamWebHelper
        wcsncat_s(buffer, size, L"\\NoSteamWebHelper", _TRUNCATE);
        
        // Create directory if it doesn't exist
        CreateDirectoryW(buffer, NULL);
        
        return buffer;
    }
    
    // Fallback to current directory
    GetCurrentDirectoryW(size, buffer);
    return buffer;
}

// Simple INI parser helper
static BOOL GetPrivateProfileIntW_Custom(const WCHAR* lpAppName, const WCHAR* lpKeyName, INT nDefault, const WCHAR* lpFileName)
{
    WCHAR szBuffer[32] = {0};
    
    if (GetPrivateProfileStringW(lpAppName, lpKeyName, NULL, szBuffer, 32, lpFileName) > 0)
    {
        return _wtoi(szBuffer);
    }
    
    return nDefault;
}

// Load configuration from INI file
VOID LoadConfig(VOID)
{
    // Reset to defaults first
    g_Config = g_DefaultConfig;
    
    // Build INI file path
    WCHAR szDir[MAX_PATH] = {0};
    GetConfigDirectory(szDir, MAX_PATH);
    wcsncat_s(szDir, MAX_PATH, L"\\NoSteamWebHelper.ini", _TRUNCATE);
    wcsncpy_s(g_Config.iniPath, MAX_PATH, szDir, _TRUNCATE);
    
    // Check if file exists
    if (GetFileAttributesW(szDir) == INVALID_FILE_ATTRIBUTES)
    {
        // File doesn't exist, use defaults
        return;
    }
    
    // Read configuration values
    INT mode = GetPrivateProfileIntW_Custom(L"Settings", L"Mode", -1, szDir);
    if (mode >= MODE_AGGRESSIVE && mode <= MODE_DISABLED)
    {
        g_Config.mode = (SUPPRESSION_MODE)mode;
    }
    
    INT enableTooltip = GetPrivateProfileIntW_Custom(L"Settings", L"EnableTrayTooltip", -1, szDir);
    if (enableTooltip != -1)
    {
        g_Config.enableTrayTooltip = enableTooltip ? TRUE : FALSE;
    }
    
    INT showRam = GetPrivateProfileIntW_Custom(L"Settings", L"ShowRamUsage", -1, szDir);
    if (showRam != -1)
    {
        g_Config.showRamUsage = showRam ? TRUE : FALSE;
    }
    
    INT autoSiSR = GetPrivateProfileIntW_Custom(L"Settings", L"AutoDetectSiSR", -1, szDir);
    if (autoSiSR != -1)
    {
        g_Config.autoDetectSiSR = autoSiSR ? TRUE : FALSE;
    }
}

// Save configuration to INI file
VOID SaveConfig(VOID)
{
    WCHAR szDir[MAX_PATH] = {0};
    GetConfigDirectory(szDir, MAX_PATH);
    wcsncat_s(szDir, MAX_PATH, L"\\NoSteamWebHelper.ini", _TRUNCATE);
    
    // Write configuration values
    WCHAR szBuffer[32];
    
    swprintf(szBuffer, 32, L"%d", g_Config.mode);
    WritePrivateProfileStringW(L"Settings", L"Mode", szBuffer, szDir);
    
    swprintf(szBuffer, 32, L"%d", g_Config.enableTrayTooltip ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"EnableTrayTooltip", szBuffer, szDir);
    
    swprintf(szBuffer, 32, L"%d", g_Config.showRamUsage ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"ShowRamUsage", szBuffer, szDir);
    
    swprintf(szBuffer, 32, L"%d", g_Config.autoDetectSiSR ? 1 : 0);
    WritePrivateProfileStringW(L"Settings", L"AutoDetectSiSR", szBuffer, szDir);
}

// Get current suppression mode
SUPPRESSION_MODE GetSuppressionMode(VOID)
{
    return g_Config.mode;
}

// Check if selective mode is active
BOOL IsSelectiveMode(VOID)
{
    return g_Config.mode == MODE_SELECTIVE;
}

// Check if process should be killed based on mode and command line
BOOL ShouldKillProcess(const WCHAR* cmdLine, SUPPRESSION_MODE mode)
{
    if (mode == MODE_DISABLED)
        return FALSE;
    
    if (mode == MODE_AGGRESSIVE)
        return TRUE;
    
    // Selective mode: analyze command line
    if (mode == MODE_SELECTIVE)
    {
        if (!cmdLine || cmdLine[0] == L'\0')
            return FALSE; // Keep broker (no --type flag)
        
        // Keep GPU process for Steam Input UI
        if (wcsstr(cmdLine, L"--type=gpu-process"))
            return FALSE;
        
        // Keep broker (no --type flag)
        if (wcsstr(cmdLine, L"--type=") == NULL)
            return FALSE;
        
        // Kill renderer and utility processes
        return TRUE;
    }
    
    return FALSE;
}

// Get total RAM usage of steamwebhelper processes in KB
DWORD GetSteamWebHelperRamUsage(VOID)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return 0;
    
    DWORD totalRamKB = 0;
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    
    if (Process32FirstW(hSnapshot, &pe32))
    {
        do
        {
            if (CompareStringOrdinal(pe32.szExeFile, -1, L"steamwebhelper.exe", -1, TRUE) == CSTR_EQUAL)
            {
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
                if (hProcess)
                {
                    PROCESS_MEMORY_COUNTERS pmc;
                    if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc)))
                    {
                        totalRamKB += (DWORD)(pmc.WorkingSetSize / 1024);
                    }
                    CloseHandle(hProcess);
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return totalRamKB;
}

// Detect SiSR or ViGEmBus running
BOOL IsSiSRRunning(VOID)
{
    if (!g_Config.autoDetectSiSR)
        return FALSE;
    
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return FALSE;
    
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    BOOL found = FALSE;
    
    if (Process32FirstW(hSnapshot, &pe32))
    {
        do
        {
            if (CompareStringOrdinal(pe32.szExeFile, -1, L"SiSR.exe", -1, TRUE) == CSTR_EQUAL ||
                CompareStringOrdinal(pe32.szExeFile, -1, L"ViGEmBus.exe", -1, TRUE) == CSTR_EQUAL)
            {
                found = TRUE;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return found;
}

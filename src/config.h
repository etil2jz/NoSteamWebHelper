#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>

// Configuration modes
typedef enum
{
    MODE_AGGRESSIVE = 0,    // Kill all steamwebhelper processes
    MODE_SELECTIVE = 1,     // Keep broker/GPU, kill renderers
    MODE_DISABLED = 2       // No intervention
} SUPPRESSION_MODE;

// Configuration structure
typedef struct
{
    SUPPRESSION_MODE mode;
    BOOL enableTrayTooltip;
    BOOL showRamUsage;
    BOOL autoDetectSiSR;
    WCHAR iniPath[MAX_PATH];
} CONFIG;

// Global configuration
extern CONFIG g_Config;

// Default configuration
extern const CONFIG g_DefaultConfig;

// Function declarations
VOID LoadConfig(VOID);
SUPPRESSION_MODE GetSuppressionMode(VOID);
BOOL IsSelectiveMode(VOID);
BOOL ShouldKillProcess(const WCHAR* cmdLine, SUPPRESSION_MODE mode);
DWORD GetSteamWebHelperRamUsage(VOID);
BOOL IsSiSRRunning(VOID);
VOID SaveConfig(VOID);
WCHAR* GetConfigDirectory(WCHAR* buffer, DWORD size);

#endif // CONFIG_H

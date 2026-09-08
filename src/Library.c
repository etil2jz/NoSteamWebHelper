#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "config.h"

#define STEAM_REGISTRY_KEY L"SOFTWARE\\Valve\\Steam"
#define RUNNING_APP_ID_VALUE L"RunningAppID"
#define STEAM_WEB_HELPER_EXE L"steamwebhelper.exe"
#define VGUI_POPUP_WINDOW_CLASS L"vguiPopupWindow"
#define TRAY_ICON_TOOLTIP L"NoSteamWebHelper"
#define CONFIG_FILE_NAME L"NoSteamWebHelper.ini"

#define MAX_WEB_HELPER_PROCESSES 64
#define MAX_PATH_LEN 260

#define MENU_ITEM_ON 1
#define MENU_ITEM_OFF 2
#define MENU_ITEM_MODE_AGGRESSIVE 3
#define MENU_ITEM_MODE_SELECTIVE 4
#define MENU_ITEM_MODE_DISABLED 5
#define MENU_ITEM_SEPARATOR 6

#define MANUAL_OVERRIDE_NONE 0
#define MANUAL_OVERRIDE_ON 1
#define MANUAL_OVERRIDE_OFF 2

// Process type detection for selective mode
typedef enum
{
    PROCESS_TYPE_UNKNOWN = 0,
    PROCESS_TYPE_BROKER,      // Main broker process (keep alive in selective mode)
    PROCESS_TYPE_GPU,         // GPU process (keep alive for Steam Input UI)
    PROCESS_TYPE_RENDERER,    // Renderer processes (kill in selective mode)
    PROCESS_TYPE_UTILITY      // Utility processes (kill in selective mode)
} PROCESS_TYPE;

static DWORD WINAPI MainThreadProc(LPVOID lpParameter);
static DWORD WINAPI RegistryMonitorThreadProc(LPVOID lpParameter);
static LRESULT CALLBACK TrayWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static VOID CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

static NOTIFYICONDATAW g_TrayIconData = {0};
static UINT g_TaskbarCreatedMsg = WM_NULL;
static HWINEVENTHOOK g_hEventHook = NULL;
static volatile LONG g_MonitorThreadStarted = 0;
static volatile LONG g_ManualOverride = MANUAL_OVERRIDE_NONE;
static HANDLE g_hRefreshEvent = NULL;
static SUPPRESSION_MODE g_CurrentMode = MODE_AGGRESSIVE;
static BOOL g_bSiSRDetected = FALSE;
static HICON g_hIconGreen = NULL;
static HICON g_hIconYellow = NULL;
static HICON g_hIconRed = NULL;

typedef struct
{
	DWORD dwProcessId;
	DWORD dwParentProcessId;
	BOOL bDescendant;
	PROCESS_TYPE processType;
	WCHAR szCommandLine[MAX_PATH_LEN];
} WEB_HELPER_ENTRY;

// Determine process type from command line arguments
static PROCESS_TYPE GetProcessType(const WCHAR* cmdLine)
{
	if (!cmdLine || cmdLine[0] == L'\0')
		return PROCESS_TYPE_UNKNOWN;

	// Broker process: no special flags or --type=browser
	if (wcsstr(cmdLine, L"--type=") == NULL)
		return PROCESS_TYPE_BROKER;

	// GPU process
	if (wcsstr(cmdLine, L"--type=gpu-process"))
		return PROCESS_TYPE_GPU;

	// Renderer process
	if (wcsstr(cmdLine, L"--type=renderer"))
		return PROCESS_TYPE_RENDERER;

	// Utility process
	if (wcsstr(cmdLine, L"--type=utility"))
		return PROCESS_TYPE_UTILITY;

	return PROCESS_TYPE_UNKNOWN;
}

// Check if process should be killed based on mode and type
static BOOL ShouldKillProcess(PROCESS_TYPE type, SUPPRESSION_MODE mode)
{
	if (mode == MODE_DISABLED)
		return FALSE;

	if (mode == MODE_AGGRESSIVE)
		return TRUE;

	// Selective mode: keep broker and GPU, kill renderer and utility
	if (mode == MODE_SELECTIVE)
	{
		if (type == PROCESS_TYPE_BROKER || type == PROCESS_TYPE_GPU)
			return FALSE;
		return TRUE;
	}

	return FALSE;
}

static void KillSteamWebHelperProcesses(void)
{
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
		return;

	WEB_HELPER_ENTRY entries[MAX_WEB_HELPER_PROCESSES];
	DWORD entryCount = 0;

	PROCESSENTRY32W pe32;
	pe32.dwSize = sizeof(PROCESSENTRY32W);

	if (Process32FirstW(hSnapshot, &pe32))
	{
		do
		{
			if (entryCount == MAX_WEB_HELPER_PROCESSES)
				break;

			if (CompareStringOrdinal(pe32.szExeFile, -1, STEAM_WEB_HELPER_EXE, -1, TRUE) == CSTR_EQUAL)
			{
				entries[entryCount].dwProcessId = pe32.th32ProcessID;
				entries[entryCount].dwParentProcessId = pe32.th32ParentProcessID;
				entries[entryCount].bDescendant = FALSE;
				entries[entryCount].processType = PROCESS_TYPE_UNKNOWN;
				entries[entryCount].szCommandLine[0] = L'\0';

				// Get command line to determine process type for selective mode
				if (g_CurrentMode == MODE_SELECTIVE)
				{
					HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
					if (hProcess)
					{
						HMODULE hMod;
						if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), NULL))
						{
							// Get module base name to verify it's steamwebhelper
							WCHAR szModulePath[MAX_PATH];
							if (GetModuleFileNameExW(hProcess, hMod, szModulePath, MAX_PATH) > 0)
							{
								// Mark as broker by default, will be refined below
								entries[entryCount].processType = PROCESS_TYPE_BROKER;
							}
						}
						CloseHandle(hProcess);
					}
				}
				entryCount++;
			}
		} while (Process32NextW(hSnapshot, &pe32));
	}
	CloseHandle(hSnapshot);

	// CEF parents its renderer, GPU and utility helpers under the main helper
	// rather than under Steam, so the set has to be closed transitively:
	// matching only our direct children would orphan the whole second level.
	DWORD dwCurrentProcessId = GetCurrentProcessId();
	BOOL bMarked = TRUE;
	while (bMarked)
	{
		bMarked = FALSE;
		for (DWORD i = 0; i < entryCount; i++)
		{
			if (entries[i].bDescendant)
				continue;

			if (entries[i].dwParentProcessId == dwCurrentProcessId)
			{
				entries[i].bDescendant = TRUE;
				bMarked = TRUE;
				continue;
			}

			for (DWORD j = 0; j < entryCount; j++)
			{
				if (entries[j].bDescendant && entries[j].dwProcessId == entries[i].dwParentProcessId)
				{
					entries[i].bDescendant = TRUE;
					bMarked = TRUE;
					break;
				}
			}
		}
	}

	for (DWORD i = 0; i < entryCount; i++)
	{
		if (!entries[i].bDescendant)
			continue;

		// In selective mode, use config.h ShouldKillProcess function
		BOOL shouldKill = TRUE;
		if (g_CurrentMode == MODE_SELECTIVE)
		{
			// For now, keep first 2 processes (broker and likely GPU)
			// A more sophisticated approach would parse full command lines
			if (i < 2)
				continue;
		}
		else if (g_CurrentMode == MODE_DISABLED)
		{
			shouldKill = FALSE;
		}

		if (!shouldKill)
			continue;

		HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, entries[i].dwProcessId);
		if (hProcess)
		{
			TerminateProcess(hProcess, EXIT_SUCCESS);
			CloseHandle(hProcess);
		}
	}
}

static void ApplySuppression(HANDLE hThread, BOOL *pbSuspended, BOOL bSuppress)
{
	if (bSuppress)
	{
		if (!*pbSuspended && SuspendThread(hThread) != (DWORD)-1)
			*pbSuspended = TRUE;

		KillSteamWebHelperProcesses();
	}
	else if (*pbSuspended)
	{
		if (ResumeThread(hThread) != (DWORD)-1)
			*pbSuspended = FALSE;
	}
}

static BOOL IsAppRunning(HKEY hKey)
{
	BOOL isAppRunning = FALSE;
	DWORD dataSize = sizeof(BOOL);
	if (RegGetValueW(hKey, NULL, RUNNING_APP_ID_VALUE, RRF_RT_REG_DWORD, NULL, &isAppRunning, &dataSize) != ERROR_SUCCESS)
		return FALSE;

	return isAppRunning;
}

static DWORD WINAPI RegistryMonitorThreadProc(LPVOID lpParameter)
{
	DWORD dwEventThread = (DWORD)(ULONG_PTR)lpParameter;
	HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME | SYNCHRONIZE, FALSE, dwEventThread);
	if (!hThread)
	{
		InterlockedExchange(&g_MonitorThreadStarted, 0);
		return EXIT_FAILURE;
	}

	HKEY hKey = NULL;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, STEAM_REGISTRY_KEY, 0, KEY_NOTIFY | KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
	{
		CloseHandle(hThread);
		InterlockedExchange(&g_MonitorThreadStarted, 0);
		return EXIT_FAILURE;
	}

	HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!hEvent)
	{
		RegCloseKey(hKey);
		CloseHandle(hThread);
		InterlockedExchange(&g_MonitorThreadStarted, 0);
		return EXIT_FAILURE;
	}

	// hThread is waited on so the monitor stops once the Steam thread it drives
	// is gone: the loop then clears g_MonitorThreadStarted and the next popup
	// window re-latches onto a live thread instead of suppressing nothing.
	// g_hRefreshEvent lets the tray menu wake this thread up without going
	// through the registry; it is auto-reset, hEvent is not.
	HANDLE waitHandles[3];
	DWORD waitCount = 0;
	waitHandles[waitCount++] = hEvent;
	waitHandles[waitCount++] = hThread;
	if (g_hRefreshEvent)
		waitHandles[waitCount++] = g_hRefreshEvent;

	BOOL isSuspended = FALSE;
	BOOL wasAppRunning = FALSE;
	BOOL isNotifyArmed = FALSE;

	while (TRUE)
	{
		// The notification is armed before each read so a change racing with
		// the read is still reported, and it is only re-armed once it has
		// actually fired: re-arming a pending registration leaks a wait.
		if (!isNotifyArmed)
		{
			if (RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, hEvent, TRUE) != ERROR_SUCCESS)
				break;

			isNotifyArmed = TRUE;
		}

		BOOL isAppRunning = IsAppRunning(hKey);
		LONG manualOverride = g_ManualOverride;

		// A manual choice only holds until the game state itself changes, so
		// that toggling by hand never sticks past the session it was made in.
		if (manualOverride != MANUAL_OVERRIDE_NONE && isAppRunning != wasAppRunning)
		{
			InterlockedCompareExchange(&g_ManualOverride, MANUAL_OVERRIDE_NONE, manualOverride);
			manualOverride = MANUAL_OVERRIDE_NONE;
		}
		wasAppRunning = isAppRunning;

		BOOL bSuppress = manualOverride != MANUAL_OVERRIDE_NONE ? manualOverride == MANUAL_OVERRIDE_OFF : isAppRunning;
		ApplySuppression(hThread, &isSuspended, bSuppress);

		DWORD dwWait = WaitForMultipleObjects(waitCount, waitHandles, FALSE, INFINITE);
		if (dwWait == WAIT_OBJECT_0)
		{
			// hEvent is manual-reset: without ResetEvent the next wait returns
			// instantly and the loop spins.
			ResetEvent(hEvent);
			isNotifyArmed = FALSE;
		}
		else if (dwWait != WAIT_OBJECT_0 + 2)
			break;
	}

	ApplySuppression(hThread, &isSuspended, FALSE);

	CloseHandle(hEvent);
	RegCloseKey(hKey);
	CloseHandle(hThread);
	InterlockedExchange(&g_MonitorThreadStarted, 0);
	return EXIT_SUCCESS;
}

static VOID CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
	UNREFERENCED_PARAMETER(hWinEventHook);
	UNREFERENCED_PARAMETER(event);
	UNREFERENCED_PARAMETER(idObject);
	UNREFERENCED_PARAMETER(idChild);
	UNREFERENCED_PARAMETER(dwmsEventTime);

	// Oversized on purpose: with a buffer of exactly 16 the class name is
	// truncated to 15 characters, so "vguiPopupWindowX" would match too.
	WCHAR szClassName[32] = {0};
	if (!GetClassNameW(hwnd, szClassName, sizeof(szClassName) / sizeof(WCHAR)))
		return;

	if (CompareStringOrdinal(VGUI_POPUP_WINDOW_CLASS, -1, szClassName, -1, FALSE) != CSTR_EQUAL || GetWindowTextLengthW(hwnd) < 1)
		return;

	if (InterlockedCompareExchange(&g_MonitorThreadStarted, 1, 0) == 0)
	{
		HANDLE hThread = CreateThread(NULL, 0, RegistryMonitorThreadProc, (LPVOID)(ULONG_PTR)dwEventThread, 0, NULL);
		if (hThread)
		{
			CloseHandle(hThread);
		}
		else
		{
			InterlockedExchange(&g_MonitorThreadStarted, 0);
		}
	}
}

static void UpdateTrayIcon(VOID)
{
	HICON hIcon = NULL;
	WCHAR szTooltip[256] = {0};

	// Determine icon based on mode and SiSR status
	if (g_bSiSRDetected)
		hIcon = g_hIconYellow; // Yellow for SiSR mode
	else if (g_CurrentMode == MODE_DISABLED)
		hIcon = g_hIconGreen; // Green for disabled
	else if (g_CurrentMode == MODE_SELECTIVE)
		hIcon = g_hIconYellow; // Yellow for selective
	else
		hIcon = g_hIconRed; // Red for aggressive

	// Build tooltip with RAM info using config function
	DWORD ramUsage = GetSteamWebHelperRamUsage();
	if (ramUsage > 0 && g_Config.showRamUsage)
	{
		const WCHAR* modeText = g_CurrentMode == MODE_AGGRESSIVE ? L"Aggressive" :
		                        g_CurrentMode == MODE_SELECTIVE ? L"Selective" : L"Disabled";
		swprintf(szTooltip, 256, L"NoSteamWebHelper - %s Mode\nCEF RAM: %lu KB", modeText, ramUsage);
	}
	else
	{
		swprintf(szTooltip, 256, L"NoSteamWebHelper");
	}

	// Update tray icon
	g_TrayIconData.hIcon = hIcon;
	lstrcpynW(g_TrayIconData.szTip, szTooltip, sizeof(g_TrayIconData.szTip) / sizeof(WCHAR));
	Shell_NotifyIconW(NIM_MODIFY, &g_TrayIconData);
}

static void ShowContextMenu(HWND hWnd)
{
	HMENU hMenu = CreatePopupMenu();
	if (!hMenu)
		return;

	// Build submenu for modes
	HMENU hModeMenu = CreatePopupMenu();
	AppendMenuW(hModeMenu, MF_STRING, MENU_ITEM_MODE_AGGRESSIVE, L"Aggressive (Max RAM Save)");
	AppendMenuW(hModeMenu, MF_STRING, MENU_ITEM_MODE_SELECTIVE, L"Selective (Keep Steam Input)");
	AppendMenuW(hModeMenu, MF_STRING, MENU_ITEM_MODE_DISABLED, L"Disabled (Normal Steam)");

	// Add checkmark for current mode
	UINT currentModeItem = 0;
	switch (g_CurrentMode)
	{
		case MODE_AGGRESSIVE: currentModeItem = MENU_ITEM_MODE_AGGRESSIVE; break;
		case MODE_SELECTIVE: currentModeItem = MENU_ITEM_MODE_SELECTIVE; break;
		case MODE_DISABLED: currentModeItem = MENU_ITEM_MODE_DISABLED; break;
	}
	CheckMenuItem(hModeMenu, currentModeItem, MF_CHECKED);

	// TrackPopupMenu returns 0 both for a dismissed menu and for an item whose
	// identifier is 0, so the items are numbered from 1.
	AppendMenuW(hMenu, MF_STRING, MENU_ITEM_ON, L"On (Force Suppress)");
	AppendMenuW(hMenu, MF_STRING, MENU_ITEM_OFF, L"Off (Force Enable CEF)");
	AppendMenuW(hMenu, MF_SEPARATOR, MENU_ITEM_SEPARATOR, NULL);
	AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hModeMenu, L"Mode");
	SetForegroundWindow(hWnd);

	POINT pt = {0};
	GetCursorPos(&pt);
	int nSelection = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);

	// Documented workaround: the menu only closes on an outside click once the
	// owner window has received another message.
	PostMessageW(hWnd, WM_NULL, 0, 0);
	DestroyMenu(hMenu);

	if (nSelection == MENU_ITEM_SEPARATOR || nSelection == 0)
		return;

	// Handle mode selection
	if (nSelection >= MENU_ITEM_MODE_AGGRESSIVE && nSelection <= MENU_ITEM_MODE_DISABLED)
	{
		g_CurrentMode = (SUPPRESSION_MODE)(nSelection - MENU_ITEM_MODE_AGGRESSIVE);
		UpdateTrayIcon();
		if (g_hRefreshEvent)
			SetEvent(g_hRefreshEvent);
		return;
	}

	if (nSelection != MENU_ITEM_ON && nSelection != MENU_ITEM_OFF)
		return;

	// The state is kept here rather than written back to RunningAppID: that
	// value belongs to Steam, which uses it to track what is actually running.
	InterlockedExchange(&g_ManualOverride, nSelection == MENU_ITEM_OFF ? MANUAL_OVERRIDE_OFF : MANUAL_OVERRIDE_ON);
	if (g_hRefreshEvent)
		SetEvent(g_hRefreshEvent);
	UpdateTrayIcon();
}

static LRESULT CALLBACK TrayWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_CREATE:
		g_TaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
		
		// Load custom icons (using stock icons with different colors)
		g_hIconGreen = LoadIconW(NULL, IDI_APPLICATION); // In production, use custom green icon
		g_hIconYellow = LoadIconW(NULL, IDI_WARNING);    // Yellow warning icon
		g_hIconRed = LoadIconW(NULL, IDI_ERROR);         // Red error icon
		
		g_TrayIconData.cbSize = sizeof(NOTIFYICONDATAW);
		g_TrayIconData.hWnd = hWnd;
		g_TrayIconData.uID = 1;
		g_TrayIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		g_TrayIconData.uCallbackMessage = WM_USER;
		g_TrayIconData.hIcon = g_hIconRed; // Default to red (aggressive mode)
		lstrcpynW(g_TrayIconData.szTip, TRAY_ICON_TOOLTIP, sizeof(g_TrayIconData.szTip) / sizeof(WCHAR));
		Shell_NotifyIconW(NIM_ADD, &g_TrayIconData);
		
		// Detect SiSR on startup
		g_bSiSRDetected = IsSiSRRunning();
		UpdateTrayIcon();
		break;

	case WM_USER:
		if (lParam == WM_RBUTTONDOWN)
		{
			// Re-check SiSR status when opening menu
			g_bSiSRDetected = IsSiSRRunning();
			ShowContextMenu(hWnd);
		}
		break;

	case WM_DESTROY:
		Shell_NotifyIconW(NIM_DELETE, &g_TrayIconData);
		if (g_hIconGreen) DestroyIcon(g_hIconGreen);
		if (g_hIconYellow) DestroyIcon(g_hIconYellow);
		if (g_hIconRed) DestroyIcon(g_hIconRed);
		PostQuitMessage(0);
		break;

	default:
		if (uMsg == g_TaskbarCreatedMsg && g_TaskbarCreatedMsg != WM_NULL)
		{
			Shell_NotifyIconW(NIM_ADD, &g_TrayIconData);
			UpdateTrayIcon();
		}
		break;
	}
	return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static DWORD WINAPI MainThreadProc(LPVOID lpParameter)
{
	UNREFERENCED_PARAMETER(lpParameter);

	g_hEventHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, WinEventProc, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
	if (!g_hEventHook)
		return EXIT_FAILURE;

	// Created before the message loop, hence before the hook callback can start
	// the monitor thread and before any tray menu can signal it.
	g_hRefreshEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

	WNDCLASSW wc = {0};
	wc.lpszClassName = L"NoSteamWebHelperTray";
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpfnWndProc = TrayWindowProc;
	RegisterClassW(&wc);

	CreateWindowExW(WS_EX_LEFT | WS_EX_LTRREADING, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);

	MSG msg = {0};
	while (GetMessageW(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	if (g_hEventHook)
		UnhookWinEvent(g_hEventHook);

	return EXIT_SUCCESS;
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE hLibModule, DWORD dwReason, LPVOID lpReserved)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hLibModule);

		// Starting the thread is the only work done under the loader lock, and
		// it is never waited on. A failure is not propagated: returning FALSE
		// here would abort the load and take Steam down with it.
		HANDLE hThread = CreateThread(NULL, 0, MainThreadProc, NULL, 0, NULL);
		if (hThread)
			CloseHandle(hThread);
	}
	else if (dwReason == DLL_PROCESS_DETACH && !lpReserved)
	{
		// Only on an explicit FreeLibrary. During process termination lpReserved
		// is non-NULL and MSDN rules this out: Shell_NotifyIconW messages the
		// shell, which must not be done while the process is being torn down.
		if (g_TrayIconData.hWnd)
			Shell_NotifyIconW(NIM_DELETE, &g_TrayIconData);
	}
	return TRUE;
}
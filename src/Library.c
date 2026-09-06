#include <windows.h>
#include <tlhelp32.h>

#define STEAM_REGISTRY_KEY L"SOFTWARE\\Valve\\Steam"
#define RUNNING_APP_ID_VALUE L"RunningAppID"
#define STEAM_WEB_HELPER_EXE L"steamwebhelper.exe"
#define VGUI_POPUP_WINDOW_CLASS L"vguiPopupWindow"
#define TRAY_ICON_TOOLTIP L"Steam WebHelper"

#define MAX_WEB_HELPER_PROCESSES 64

#define MENU_ITEM_ON 1
#define MENU_ITEM_OFF 2

#define MANUAL_OVERRIDE_NONE 0
#define MANUAL_OVERRIDE_ON 1
#define MANUAL_OVERRIDE_OFF 2

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

typedef struct
{
	DWORD dwProcessId;
	DWORD dwParentProcessId;
	BOOL bDescendant;
} WEB_HELPER_ENTRY;

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

static void ShowContextMenu(HWND hWnd)
{
	HMENU hMenu = CreatePopupMenu();
	if (!hMenu)
		return;

	// TrackPopupMenu returns 0 both for a dismissed menu and for an item whose
	// identifier is 0, so the items are numbered from 1.
	AppendMenuW(hMenu, MF_STRING, MENU_ITEM_ON, L"On");
	AppendMenuW(hMenu, MF_STRING, MENU_ITEM_OFF, L"Off");
	SetForegroundWindow(hWnd);

	POINT pt = {0};
	GetCursorPos(&pt);
	int nSelection = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);

	// Documented workaround: the menu only closes on an outside click once the
	// owner window has received another message.
	PostMessageW(hWnd, WM_NULL, 0, 0);
	DestroyMenu(hMenu);

	if (nSelection != MENU_ITEM_ON && nSelection != MENU_ITEM_OFF)
		return;

	// The state is kept here rather than written back to RunningAppID: that
	// value belongs to Steam, which uses it to track what is actually running.
	InterlockedExchange(&g_ManualOverride, nSelection == MENU_ITEM_OFF ? MANUAL_OVERRIDE_OFF : MANUAL_OVERRIDE_ON);
	if (g_hRefreshEvent)
		SetEvent(g_hRefreshEvent);
}

static LRESULT CALLBACK TrayWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_CREATE:
		g_TaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
		g_TrayIconData.cbSize = sizeof(NOTIFYICONDATAW);
		g_TrayIconData.hWnd = hWnd;
		g_TrayIconData.uID = 1;
		g_TrayIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		g_TrayIconData.uCallbackMessage = WM_USER;
		g_TrayIconData.hIcon = LoadIconW(NULL, IDI_APPLICATION);
		lstrcpynW(g_TrayIconData.szTip, TRAY_ICON_TOOLTIP, sizeof(g_TrayIconData.szTip) / sizeof(WCHAR));
		Shell_NotifyIconW(NIM_ADD, &g_TrayIconData);
		break;

	case WM_USER:
		if (lParam == WM_RBUTTONDOWN)
		{
			ShowContextMenu(hWnd);
		}
		break;

	case WM_DESTROY:
		Shell_NotifyIconW(NIM_DELETE, &g_TrayIconData);
		PostQuitMessage(0);
		break;

	default:
		if (uMsg == g_TaskbarCreatedMsg && g_TaskbarCreatedMsg != WM_NULL)
		{
			Shell_NotifyIconW(NIM_ADD, &g_TrayIconData);
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
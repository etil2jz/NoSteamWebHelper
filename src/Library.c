#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#define STEAM_REGISTRY_KEY L"SOFTWARE\\Valve\\Steam"
#define RUNNING_APP_ID_VALUE L"RunningAppID"
#define STEAM_WEB_HELPER_EXE L"steamwebhelper.exe"
#define VGUI_POPUP_WINDOW_CLASS L"vguiPopupWindow"
#define TRAY_ICON_TOOLTIP L"Steam WebHelper"

static DWORD WINAPI MainThreadProc(LPVOID lpParameter);
static DWORD WINAPI RegistryMonitorThreadProc(LPVOID lpParameter);
static LRESULT CALLBACK TrayWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static VOID CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

static NOTIFYICONDATAW g_TrayIconData = {0};
static UINT g_TaskbarCreatedMsg = WM_NULL;
static HWINEVENTHOOK g_hEventHook = NULL;
static volatile LONG g_MonitorThreadStarted = 0;

static void KillSteamWebHelperProcesses(void)
{
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
		return;

	PROCESSENTRY32W pe32;
	pe32.dwSize = sizeof(PROCESSENTRY32W);

	if (Process32FirstW(hSnapshot, &pe32))
	{
		do
		{
			if (CompareStringOrdinal(pe32.szExeFile, -1, STEAM_WEB_HELPER_EXE, -1, TRUE) == CSTR_EQUAL)
			{
				HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
				if (hProcess)
				{
					PROCESS_BASIC_INFORMATION pbi = {0};
					if (NT_SUCCESS(NtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(PROCESS_BASIC_INFORMATION), NULL)))
					{
						if (pbi.InheritedFromUniqueProcessId == (ULONG_PTR)GetCurrentProcessId())
						{
							TerminateProcess(hProcess, EXIT_SUCCESS);
						}
					}
					CloseHandle(hProcess);
				}
			}
		} while (Process32NextW(hSnapshot, &pe32));
	}
	CloseHandle(hSnapshot);
}

static DWORD WINAPI RegistryMonitorThreadProc(LPVOID lpParameter)
{
	DWORD dwEventThread = (DWORD)(ULONG_PTR)lpParameter;
	HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, dwEventThread);
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

	BOOL isAppRunning = FALSE;
	DWORD dataSize = sizeof(BOOL);
	if (RegGetValueW(hKey, NULL, RUNNING_APP_ID_VALUE, RRF_RT_REG_DWORD, NULL, &isAppRunning, &dataSize) == ERROR_SUCCESS)
	{
		if (isAppRunning)
		{
			SuspendThread(hThread);
			KillSteamWebHelperProcesses();
		}
	}

	while (TRUE)
	{
		if (RegNotifyChangeKeyValue(hKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, hEvent, TRUE) != ERROR_SUCCESS)
			break;

		if (WaitForSingleObject(hEvent, INFINITE) != WAIT_OBJECT_0)
			break;

		isAppRunning = FALSE;
		dataSize = sizeof(BOOL);
		if (RegGetValueW(hKey, NULL, RUNNING_APP_ID_VALUE, RRF_RT_REG_DWORD, NULL, &isAppRunning, &dataSize) == ERROR_SUCCESS)
		{
			if (isAppRunning)
			{
				SuspendThread(hThread);
				KillSteamWebHelperProcesses();
			}
			else
			{
				ResumeThread(hThread);
			}
		}
	}

	CloseHandle(hEvent);
	RegCloseKey(hKey);
	CloseHandle(hThread);
	InterlockedExchange(&g_MonitorThreadStarted, 0);
	return EXIT_SUCCESS;
}

static VOID CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
	WCHAR szClassName[16] = {0};
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
	if (hMenu)
	{
		AppendMenuW(hMenu, MF_STRING, FALSE, L"On");
		AppendMenuW(hMenu, MF_STRING, TRUE, L"Off");
		SetForegroundWindow(hWnd);

		POINT pt = {0};
		GetCursorPos(&pt);
		BOOL bOff = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);

		DWORD val = bOff ? TRUE : FALSE;
		RegSetKeyValueW(HKEY_CURRENT_USER, STEAM_REGISTRY_KEY, RUNNING_APP_ID_VALUE, REG_DWORD, &val, sizeof(DWORD));

		DestroyMenu(hMenu);
	}
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
	g_hEventHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, WinEventProc, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);

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
		CloseHandle(CreateThread(NULL, 0, MainThreadProc, NULL, 0, NULL));
	}
	return TRUE;
}
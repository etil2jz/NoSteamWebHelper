//! `umpdc.dll`: side-loaded by Steam, it suspends Steam's web helper (CEF)
//! while a game is running and restores it once the game exits.

#![no_std]

use core::ffi::c_void;
use core::mem::{size_of, zeroed};
use core::num::NonZeroU32;
use core::ptr::{null, null_mut};
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, AtomicUsize, Ordering};

use windows_sys::Win32::Foundation::{
    CloseHandle, ERROR_SUCCESS, FALSE, HANDLE, HINSTANCE, HWND, INVALID_HANDLE_VALUE, LPARAM,
    LRESULT, POINT, TRUE, WAIT_OBJECT_0, WPARAM,
};
use windows_sys::Win32::Globalization::{CSTR_EQUAL, CompareStringOrdinal};
use windows_sys::Win32::Graphics::Gdi::{
    BI_RGB, BITMAPINFO, BITMAPINFOHEADER, CreateBitmap, CreateDIBSection, DIB_RGB_COLORS,
    DeleteObject, HGDIOBJ,
};
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW, TH32CS_SNAPPROCESS,
};
use windows_sys::Win32::System::LibraryLoader::{
    DisableThreadLibraryCalls, GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
    GET_MODULE_HANDLE_EX_FLAG_PIN, GetModuleHandleExW,
};
use windows_sys::Win32::System::ProcessStatus::{
    K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS_EX2,
};
use windows_sys::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, KEY_NOTIFY, KEY_QUERY_VALUE, REG_DWORD, REG_NOTIFY_CHANGE_LAST_SET,
    RRF_RT_REG_DWORD, RegCloseKey, RegGetValueW, RegNotifyChangeKeyValue, RegOpenKeyExW,
    RegSetKeyValueW,
};
use windows_sys::Win32::System::SystemServices::DLL_PROCESS_ATTACH;
use windows_sys::Win32::System::Threading::{
    CreateEventW, CreateThread, ExitThread, GetCurrentProcessId, INFINITE, OpenProcess, OpenThread,
    PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_TERMINATE,
    QueryFullProcessImageNameW, ResetEvent, ResumeThread, SetEvent, SuspendThread,
    THREAD_SUSPEND_RESUME, THREAD_SYNCHRONIZE, TerminateProcess, WaitForMultipleObjects,
};
use windows_sys::Win32::UI::Accessibility::{HWINEVENTHOOK, SetWinEventHook, UnhookWinEvent};
use windows_sys::Win32::UI::Shell::{
    NIF_ICON, NIF_MESSAGE, NIF_TIP, NIM_ADD, NIM_DELETE, NIM_MODIFY, NOTIFYICONDATAW,
    Shell_NotifyIconW,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    AppendMenuW, CreateIconIndirect, CreatePopupMenu, CreateWindowExW, DefWindowProcW, DestroyIcon,
    DestroyMenu, DispatchMessageW, EVENT_OBJECT_CREATE, GetClassNameW, GetCursorPos, GetMessageW,
    GetSystemMetrics, GetWindowTextLengthW, HICON, ICONINFO, IDI_APPLICATION, LoadIconW,
    MF_CHECKED, MF_STRING, MF_UNCHECKED, MSG, PostMessageW, PostQuitMessage, RegisterClassW,
    RegisterWindowMessageW, SM_CXSMICON, SetForegroundWindow, TPM_LEFTALIGN, TPM_RETURNCMD,
    TPM_RIGHTBUTTON, TPM_TOPALIGN, TrackPopupMenu, TranslateMessage, WINEVENT_OUTOFCONTEXT, WM_APP,
    WM_CREATE, WM_DESTROY, WM_NULL, WM_RBUTTONDOWN, WM_USER, WNDCLASSW, WS_EX_LEFT,
    WS_EX_LTRREADING, WS_OVERLAPPED,
};

/// Encodes an ASCII literal as a NUL-terminated UTF-16 array at compile time.
macro_rules! wide {
    ($s:literal) => {{
        const WIDE: [u16; $s.len() + 1] = $crate::ascii_to_wide($s);
        &WIDE
    }};
}

const STEAM_REGISTRY_KEY: &[u16] = wide!("SOFTWARE\\Valve\\Steam");
const RUNNING_APP_ID_VALUE: &[u16] = wide!("RunningAppID");
const STEAM_WEB_HELPER_EXE: &[u16] = wide!("steamwebhelper.exe");
const VGUI_POPUP_WINDOW_CLASS: &[u16] = wide!("vguiPopupWindow");
const SETTINGS_REGISTRY_KEY: &[u16] = wide!("SOFTWARE\\NoSteamWebHelper");
const ENABLED_VALUE: &[u16] = wide!("Enabled");
const TRAY_WINDOW_CLASS: &[u16] = wide!("NoSteamWebHelperTray");
const TASKBAR_CREATED: &[u16] = wide!("TaskbarCreated");

const MAX_WEB_HELPER_PROCESSES: usize = 64;

/// In UTF-16 units: well past `MAX_PATH`, since Steam may live under a long
/// path. An image path that does not fit fails the check.
const IMAGE_PATH_CAPACITY: u32 = 2048;

const MENU_ITEM_TOGGLE: usize = 1;

/// The shell tells tray icons apart by executable and ID, and the executable
/// here is steam.exe, whose own icon uses ID 1: sharing it would make hiding or
/// showing either icon apply to both. "NSWH" in ASCII.
const TRAY_ICON_ID: u32 = 0x4E53_5748;

/// Posted to the tray window when `SUPPRESSED` or `SAVED_BYTES` changes.
const WM_TRAY_STATE_CHANGED: u32 = WM_APP;

const BYTES_PER_MEGABYTE: usize = 1024 * 1024;

const EXIT_SUCCESS: u32 = 0;
const EXIT_FAILURE: u32 = 1;

static TRAY_WINDOW: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
static TASKBAR_CREATED_MSG: AtomicU32 = AtomicU32::new(WM_NULL);
static MONITOR_THREAD_STARTED: AtomicBool = AtomicBool::new(false);
static REFRESH_EVENT: AtomicPtr<c_void> = AtomicPtr::new(null_mut());
/// The tray menu's "Toggle": whether a running game suppresses the web helper.
static ENABLED: AtomicBool = AtomicBool::new(true);
static SUPPRESSED: AtomicBool = AtomicBool::new(false);
static SAVED_BYTES: AtomicUsize = AtomicUsize::new(0);
/// The icon last handed to the shell, destroyed once it has been replaced.
static TRAY_ICON: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

#[expect(
    clippy::indexing_slicing,
    reason = "only evaluated in const context, where an out-of-bounds index fails the build"
)]
const fn ascii_to_wide<const N: usize>(s: &str) -> [u16; N] {
    let bytes = s.as_bytes();
    assert!(bytes.len() + 1 == N);

    let mut wide = [0; N];
    let mut i = 0;
    while i < bytes.len() {
        assert!(bytes[i].is_ascii());
        wide[i] = bytes[i] as u16;
        i += 1;
    }
    wide
}

#[expect(
    clippy::cast_possible_truncation,
    reason = "Win32 structure sizes are far below u32::MAX"
)]
const fn struct_size<T>() -> u32 {
    size_of::<T>() as u32
}

/// Kernel handle closed on drop.
struct OwnedHandle(HANDLE);

impl OwnedHandle {
    fn new(handle: HANDLE) -> Option<Self> {
        (!handle.is_null()).then_some(Self(handle))
    }
}

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        unsafe { CloseHandle(self.0) };
    }
}

/// Registry key closed on drop.
struct OwnedKey(HKEY);

impl Drop for OwnedKey {
    fn drop(&mut self) {
        unsafe { RegCloseKey(self.0) };
    }
}

/// Clears `MONITOR_THREAD_STARTED` on drop, so that whichever way the monitor
/// thread exits, the next popup window can start a new one.
struct MonitorThreadGuard;

impl Drop for MonitorThreadGuard {
    fn drop(&mut self) {
        MONITOR_THREAD_STARTED.store(false, Ordering::Release);
    }
}

#[derive(Clone, Copy)]
struct WebHelperEntry {
    process_id: u32,
    parent_process_id: u32,
    descendant: bool,
}

/// Returns the private working set of a process, the figure Task Manager shows
/// as its memory: shared pages are left out, since terminating one process
/// does not free them. `PROCESS_MEMORY_COUNTERS_EX2` needs Windows 10 1809;
/// before that the query fails and the process counts for nothing.
fn private_working_set(process: HANDLE) -> usize {
    let mut counters: PROCESS_MEMORY_COUNTERS_EX2 = unsafe { zeroed() };
    counters.cb = struct_size::<PROCESS_MEMORY_COUNTERS_EX2>();
    let queried =
        unsafe { K32GetProcessMemoryInfo(process, (&raw mut counters).cast(), counters.cb) };
    if queried == FALSE {
        return 0;
    }

    counters.PrivateWorkingSetSize
}

/// Checks that an opened process still runs steamwebhelper.exe. Its PID comes
/// from a snapshot taken earlier: a helper exiting in between could hand the
/// PID over to an unrelated process, which must not be killed in its place.
fn is_steam_web_helper(process: HANDLE) -> bool {
    let mut path = [0u16; IMAGE_PATH_CAPACITY as usize];
    let mut length = IMAGE_PATH_CAPACITY;
    let queried = unsafe {
        QueryFullProcessImageNameW(
            process,
            PROCESS_NAME_WIN32,
            path.as_mut_ptr(),
            &raw mut length,
        )
    };
    if queried == FALSE {
        return false;
    }

    let path = path.get(..length as usize).unwrap_or_default();
    let file_name = path
        .rsplit(|&unit| unit == u16::from(b'\\'))
        .next()
        .unwrap_or_default();
    let Ok(file_name_length) = i32::try_from(file_name.len()) else {
        return false;
    };

    let comparison = unsafe {
        CompareStringOrdinal(
            file_name.as_ptr(),
            file_name_length,
            STEAM_WEB_HELPER_EXE.as_ptr(),
            -1,
            TRUE,
        )
    };
    comparison == CSTR_EQUAL
}

/// Terminates the web helper process tree and returns the memory it held.
fn kill_steam_web_helper_processes() -> usize {
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    if snapshot == INVALID_HANDLE_VALUE {
        return 0;
    }
    let snapshot = OwnedHandle(snapshot);

    let mut entries = [WebHelperEntry {
        process_id: 0,
        parent_process_id: 0,
        descendant: false,
    }; MAX_WEB_HELPER_PROCESSES];
    let mut entry_count = 0;

    let mut pe32: PROCESSENTRY32W = unsafe { zeroed() };
    pe32.dwSize = struct_size::<PROCESSENTRY32W>();

    if unsafe { Process32FirstW(snapshot.0, &raw mut pe32) } != FALSE {
        while let Some(entry) = entries.get_mut(entry_count) {
            let matches = unsafe {
                CompareStringOrdinal(
                    pe32.szExeFile.as_ptr(),
                    -1,
                    STEAM_WEB_HELPER_EXE.as_ptr(),
                    -1,
                    TRUE,
                )
            } == CSTR_EQUAL;
            if matches {
                *entry = WebHelperEntry {
                    process_id: pe32.th32ProcessID,
                    parent_process_id: pe32.th32ParentProcessID,
                    descendant: false,
                };
                entry_count += 1;
            }

            if unsafe { Process32NextW(snapshot.0, &raw mut pe32) } == FALSE {
                break;
            }
        }
    }
    drop(snapshot);

    let entries = entries.get_mut(..entry_count).unwrap_or_default();

    // CEF parents its renderer, GPU and utility helpers under the main helper
    // rather than under Steam, so the set has to be closed transitively:
    // matching only our direct children would orphan the whole second level.
    let current_process_id = unsafe { GetCurrentProcessId() };
    let mut marked = true;
    while marked {
        marked = false;
        for i in 0..entries.len() {
            let Some(&entry) = entries.get(i) else {
                break;
            };
            if entry.descendant {
                continue;
            }

            let descendant = entry.parent_process_id == current_process_id
                || entries.iter().any(|parent| {
                    parent.descendant && parent.process_id == entry.parent_process_id
                });
            if descendant && let Some(entry) = entries.get_mut(i) {
                entry.descendant = true;
                marked = true;
            }
        }
    }

    let mut freed_bytes: usize = 0;
    for entry in entries.iter().filter(|entry| entry.descendant) {
        // The query right serves both the image check and the tooltip: a
        // process that would not grant it is left alone rather than killed
        // unverified.
        let process = OwnedHandle::new(unsafe {
            OpenProcess(
                PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                entry.process_id,
            )
        });
        let Some(process) = process.filter(|process| is_steam_web_helper(process.0)) else {
            continue;
        };

        let process_bytes = private_working_set(process.0);
        if unsafe { TerminateProcess(process.0, EXIT_SUCCESS) } != FALSE {
            freed_bytes = freed_bytes.saturating_add(process_bytes);
        }
    }
    freed_bytes
}

/// What the monitor thread has done to Steam, mirrored into the tray tooltip.
struct Suppression {
    thread: HANDLE,
    suspended: bool,
    saved_bytes: usize,
}

impl Suppression {
    fn apply(&mut self, suppress: bool) {
        if suppress {
            if !self.suspended && unsafe { SuspendThread(self.thread) } != u32::MAX {
                self.suspended = true;
            }

            // Whatever a later pass finds, missed or respawned, is added: the
            // figure is what the kills freed over the whole game session.
            self.saved_bytes = self
                .saved_bytes
                .saturating_add(kill_steam_web_helper_processes());
        } else {
            if self.suspended && unsafe { ResumeThread(self.thread) } != u32::MAX {
                self.suspended = false;
            }

            self.saved_bytes = 0;
        }

        publish_tray_state(suppress, self.saved_bytes);
    }
}

fn publish_tray_state(suppressed: bool, saved_bytes: usize) {
    let suppressed_changed = SUPPRESSED.swap(suppressed, Ordering::SeqCst) != suppressed;
    let saved_bytes_changed = SAVED_BYTES.swap(saved_bytes, Ordering::SeqCst) != saved_bytes;
    if !suppressed_changed && !saved_bytes_changed {
        return;
    }

    // Both values are stored before posting, so the last message handled
    // always sees the final pair. Before the tray window exists there is no
    // one to tell: the icon reads the state when it is first added. That
    // hand-off is why these accesses and the tray thread's are SeqCst: with
    // acquire/release alone, this load could miss the window just created
    // while the tray thread still reads the previous state.
    let tray_window = TRAY_WINDOW.load(Ordering::SeqCst);
    if !tray_window.is_null() {
        unsafe { PostMessageW(tray_window, WM_TRAY_STATE_CHANGED, 0, 0) };
    }
}

fn load_enabled() -> bool {
    let mut enabled: u32 = 0;
    let mut data_size = struct_size::<u32>();
    let status = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            SETTINGS_REGISTRY_KEY.as_ptr(),
            ENABLED_VALUE.as_ptr(),
            RRF_RT_REG_DWORD,
            null_mut(),
            (&raw mut enabled).cast(),
            &raw mut data_size,
        )
    };

    // The value only exists once the setting has been changed: until then,
    // the default applies.
    status != ERROR_SUCCESS || enabled != 0
}

fn save_enabled(enabled: bool) {
    let value = u32::from(enabled);
    unsafe {
        RegSetKeyValueW(
            HKEY_CURRENT_USER,
            SETTINGS_REGISTRY_KEY.as_ptr(),
            ENABLED_VALUE.as_ptr(),
            REG_DWORD,
            (&raw const value).cast(),
            struct_size::<u32>(),
        )
    };
}

fn is_app_running(key: HKEY) -> bool {
    let mut running_app_id: u32 = 0;
    let mut data_size = struct_size::<u32>();
    let status = unsafe {
        RegGetValueW(
            key,
            null(),
            RUNNING_APP_ID_VALUE.as_ptr(),
            RRF_RT_REG_DWORD,
            null_mut(),
            (&raw mut running_app_id).cast(),
            &raw mut data_size,
        )
    };
    status == ERROR_SUCCESS && running_app_id != 0
}

unsafe extern "system" fn registry_monitor_thread_proc(parameter: *mut c_void) -> u32 {
    let _started = MonitorThreadGuard;

    let Ok(event_thread_id) = u32::try_from(parameter.addr()) else {
        return EXIT_FAILURE;
    };
    let thread = unsafe {
        OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_SYNCHRONIZE,
            FALSE,
            event_thread_id,
        )
    };
    let Some(thread) = OwnedHandle::new(thread) else {
        return EXIT_FAILURE;
    };

    let mut key: HKEY = null_mut();
    let status = unsafe {
        RegOpenKeyExW(
            HKEY_CURRENT_USER,
            STEAM_REGISTRY_KEY.as_ptr(),
            0,
            KEY_NOTIFY | KEY_QUERY_VALUE,
            &raw mut key,
        )
    };
    if status != ERROR_SUCCESS {
        return EXIT_FAILURE;
    }
    let key = OwnedKey(key);

    let event = unsafe { CreateEventW(null(), TRUE, FALSE, null()) };
    let Some(event) = OwnedHandle::new(event) else {
        return EXIT_FAILURE;
    };

    // The thread is waited on so the monitor stops once the Steam thread it
    // drives is gone: the guard then clears MONITOR_THREAD_STARTED and the next
    // popup window re-latches onto a live thread instead of suppressing
    // nothing. REFRESH_EVENT lets the tray menu wake this thread up without
    // going through the registry; it is auto-reset, `event` is not.
    let refresh_event = REFRESH_EVENT.load(Ordering::Acquire);
    let wait_handles = [event.0, thread.0, refresh_event];
    let wait_count = if refresh_event.is_null() { 2 } else { 3 };

    let mut suppression = Suppression {
        thread: thread.0,
        suspended: false,
        saved_bytes: 0,
    };
    let mut notify_armed = false;

    loop {
        // The notification is armed before each read so a change racing with
        // the read is still reported, and it is only re-armed once it has
        // actually fired: re-arming a pending registration leaks a wait.
        if !notify_armed {
            let status = unsafe {
                RegNotifyChangeKeyValue(key.0, FALSE, REG_NOTIFY_CHANGE_LAST_SET, event.0, TRUE)
            };
            if status != ERROR_SUCCESS {
                break;
            }

            notify_armed = true;
        }

        // The setting is read on every pass: unticking it mid-game wakes this
        // thread through REFRESH_EVENT and restores the web helper at once.
        let suppress = ENABLED.load(Ordering::Acquire) && is_app_running(key.0);
        suppression.apply(suppress);

        let wait =
            unsafe { WaitForMultipleObjects(wait_count, wait_handles.as_ptr(), FALSE, INFINITE) };
        if wait == WAIT_OBJECT_0 {
            // `event` is manual-reset: without ResetEvent the next wait returns
            // instantly and the loop spins.
            unsafe { ResetEvent(event.0) };
            notify_armed = false;
        } else if wait != WAIT_OBJECT_0 + 2 {
            break;
        }
    }

    suppression.apply(false);
    EXIT_SUCCESS
}

unsafe extern "system" fn win_event_proc(
    _hook: HWINEVENTHOOK,
    _event: u32,
    hwnd: HWND,
    _id_object: i32,
    _id_child: i32,
    event_thread_id: u32,
    _event_time: u32,
) {
    // Checked first, and not only to save work: while the monitor runs, the
    // thread owning this window may be suspended, and GetWindowTextLengthW
    // below sends it a message and waits, which would hang this thread for
    // the rest of the game.
    if MONITOR_THREAD_STARTED.load(Ordering::Acquire) {
        return;
    }

    // Oversized on purpose: with a buffer of exactly 16 the class name is
    // truncated to 15 characters, so "vguiPopupWindowX" would match too.
    let mut class_name = [0u16; 32];
    if unsafe { GetClassNameW(hwnd, class_name.as_mut_ptr(), 32) } == 0 {
        return;
    }

    // Compared through Win32 rather than as slices, which would pull memcmp in
    // from a CRT this DLL does not otherwise depend on.
    let matches = unsafe {
        CompareStringOrdinal(
            VGUI_POPUP_WINDOW_CLASS.as_ptr(),
            -1,
            class_name.as_ptr(),
            -1,
            FALSE,
        )
    } == CSTR_EQUAL;
    if !matches || unsafe { GetWindowTextLengthW(hwnd) } < 1 {
        return;
    }

    if MONITOR_THREAD_STARTED
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_ok()
    {
        let thread = unsafe {
            CreateThread(
                null(),
                0,
                Some(registry_monitor_thread_proc),
                core::ptr::without_provenance(event_thread_id as usize),
                0,
                null_mut(),
            )
        };
        match OwnedHandle::new(thread) {
            Some(thread) => drop(thread),
            None => MONITOR_THREAD_STARTED.store(false, Ordering::Release),
        }
    }
}

/// Identifies the tray icon: enough on its own for `NIM_DELETE`.
fn tray_icon_data(hwnd: HWND) -> NOTIFYICONDATAW {
    let mut data: NOTIFYICONDATAW = unsafe { zeroed() };
    data.cbSize = struct_size::<NOTIFYICONDATAW>();
    data.hWnd = hwnd;
    data.uID = TRAY_ICON_ID;
    data
}

/// Appends ASCII text to a zeroed UTF-16 buffer, dropping whatever does not
/// fit before the terminating NUL.
struct WideWriter<'a> {
    buffer: &'a mut [u16],
    length: usize,
}

impl WideWriter<'_> {
    fn push(&mut self, unit: u16) {
        // The last slot is never written: it keeps the buffer NUL-terminated.
        if self.length + 1 < self.buffer.len()
            && let Some(slot) = self.buffer.get_mut(self.length)
        {
            *slot = unit;
            self.length += 1;
        }
    }

    fn push_str(&mut self, text: &str) {
        for byte in text.bytes() {
            self.push(u16::from(byte));
        }
    }

    fn push_decimal(&mut self, mut value: usize) {
        let mut digits = [0u8; 20];
        let mut count = 0;
        while let Some(slot) = digits.get_mut(count) {
            *slot = b"0123456789".get(value % 10).copied().unwrap_or(b'0');
            count += 1;
            value /= 10;
            if value == 0 {
                break;
            }
        }

        for &digit in digits.get(..count).unwrap_or_default().iter().rev() {
            self.push(u16::from(digit));
        }
    }
}

fn write_tray_tip(tip: &mut [u16]) {
    let mut writer = WideWriter {
        buffer: tip,
        length: 0,
    };
    if SUPPRESSED.load(Ordering::SeqCst) {
        writer.push_str("Steam WebHelper: suspended\nRAM saved: ");
        writer.push_decimal(SAVED_BYTES.load(Ordering::SeqCst) / BYTES_PER_MEGABYTE);
        writer.push_str(" MB");
    } else {
        writer.push_str("Steam WebHelper: running");
    }
}

/// The badge is drawn on a 16×16 grid: a rounded square holding "NS" over
/// "WH", each glyph 5×5 with a two-unit gutter and margin.
const BADGE_GRID: u32 = 16;
const BADGE_CORNER_RADIUS: u32 = 4;
const BADGE_COLOR: u32 = 0x001A_9FFF;
const GLYPH_COLOR: u32 = 0x00FF_FFFF;
const GLYPH_ORIGINS: [u32; 2] = [2, 9];
const GLYPH_SIZE: u32 = 5;

/// One row per byte, leftmost pixel in the highest of the five low bits.
const GLYPHS: [[u8; 5]; 4] = [
    // N
    [0b10001, 0b11001, 0b10101, 0b10011, 0b10001],
    // S
    [0b01111, 0b10000, 0b01110, 0b00001, 0b11110],
    // W
    [0b10001, 0b10001, 0b10101, 0b11011, 0b10001],
    // H
    [0b10001, 0b10001, 0b11111, 0b10001, 0b10001],
];

/// Subsamples per pixel along each axis: sizes that are not a multiple of the
/// grid, such as 24 pixels at 150 %, are antialiased rather than distorted.
const SUBSAMPLES: u32 = 4;
/// Fixed-point scale of grid coordinates.
const GRID_ONE: u32 = 256;

/// Largest tray icon drawn, the small icon size at 400 %.
const MAX_ICON_SIZE: i32 = 64;
/// A monochrome mask's rows are padded to 16 bits.
const MASK_BYTES: usize = (MAX_ICON_SIZE as usize).div_ceil(16) * 2 * MAX_ICON_SIZE as usize;

/// GDI object deleted on drop.
struct OwnedGdiObject(HGDIOBJ);

impl OwnedGdiObject {
    fn new(object: HGDIOBJ) -> Option<Self> {
        (!object.is_null()).then_some(Self(object))
    }
}

impl Drop for OwnedGdiObject {
    fn drop(&mut self) {
        unsafe { DeleteObject(self.0) };
    }
}

fn is_glyph_pixel(x: u32, y: u32) -> bool {
    let locate = |position: u32| {
        GLYPH_ORIGINS
            .iter()
            .position(|&origin| (origin..origin + GLYPH_SIZE).contains(&position))
            .map(|cell| {
                (
                    cell,
                    position - GLYPH_ORIGINS.get(cell).copied().unwrap_or(0),
                )
            })
    };
    let (Some((column, x)), Some((row, y))) = (locate(x), locate(y)) else {
        return false;
    };

    GLYPHS
        .get(row * 2 + column)
        .and_then(|glyph| glyph.get(y as usize))
        .is_some_and(|bits| bits >> (GLYPH_SIZE - 1 - x) & 1 != 0)
}

fn is_inside_badge(x: u32, y: u32) -> bool {
    // Distance to the nearest point of the square shrunk by the radius.
    let radius = BADGE_CORNER_RADIUS * GRID_ONE;
    let far_edge = BADGE_GRID * GRID_ONE - radius;
    let dx = radius.saturating_sub(x).max(x.saturating_sub(far_edge));
    let dy = radius.saturating_sub(y).max(y.saturating_sub(far_edge));
    dx * dx + dy * dy <= radius * radius
}

/// Maps the center of a subsample to fixed-point grid coordinates.
fn subsample_to_grid(pixel: u32, subsample: u32, size: NonZeroU32) -> u32 {
    let offset = (pixel * SUBSAMPLES + subsample) * 2 + 1;
    offset * BADGE_GRID * GRID_ONE / (2 * SUBSAMPLES) / size
}

/// Renders the badge into top-down BGRA pixels, with straight alpha as icons
/// expect.
fn draw_badge(pixels: &mut [u32], size: NonZeroU32) {
    let mut rows = pixels.chunks_exact_mut(size.get() as usize);
    for (y, row) in (0..size.get()).zip(&mut rows) {
        for (x, pixel) in (0..size.get()).zip(row.iter_mut()) {
            let (mut covered, mut red, mut green, mut blue) = (0, 0, 0, 0);
            for sub_y in 0..SUBSAMPLES {
                let grid_y = subsample_to_grid(y, sub_y, size);
                for sub_x in 0..SUBSAMPLES {
                    let grid_x = subsample_to_grid(x, sub_x, size);
                    if !is_inside_badge(grid_x, grid_y) {
                        continue;
                    }

                    let glyph = is_glyph_pixel(grid_x / GRID_ONE, grid_y / GRID_ONE);
                    let color = if glyph { GLYPH_COLOR } else { BADGE_COLOR };
                    covered += 1;
                    red += color >> 16 & 0xFF;
                    green += color >> 8 & 0xFF;
                    blue += color & 0xFF;
                }
            }

            *pixel = match NonZeroU32::new(covered) {
                Some(count) => {
                    let alpha = covered * 0xFF / (SUBSAMPLES * SUBSAMPLES);
                    (alpha << 24) | ((red / count) << 16) | ((green / count) << 8) | (blue / count)
                }
                None => 0,
            };
        }
    }
}

/// Draws the NSWH badge at the size the shell shows tray icons at.
fn create_tray_icon() -> HICON {
    let metric = unsafe { GetSystemMetrics(SM_CXSMICON) }.clamp(16, MAX_ICON_SIZE);
    let Some(size) = u32::try_from(metric).ok().and_then(NonZeroU32::new) else {
        return null_mut();
    };

    let mut info: BITMAPINFO = unsafe { zeroed() };
    info.bmiHeader.biSize = struct_size::<BITMAPINFOHEADER>();
    info.bmiHeader.biWidth = metric;
    // Negative: rows run top-down.
    info.bmiHeader.biHeight = -metric;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    let mut bits: *mut c_void = null_mut();
    let color = unsafe {
        CreateDIBSection(
            null_mut(),
            &raw const info,
            DIB_RGB_COLORS,
            &raw mut bits,
            null_mut(),
            0,
        )
    };
    let Some(color) = OwnedGdiObject::new(color) else {
        return null_mut();
    };
    if bits.is_null() {
        return null_mut();
    }

    // The section holds exactly size × size pixels of 32 bits, owned by
    // `color` and written by nothing else while this slice lives.
    let pixel_count = size.get() as usize * size.get() as usize;
    let pixels = unsafe { core::slice::from_raw_parts_mut(bits.cast::<u32>(), pixel_count) };
    draw_badge(pixels, size);

    // Alpha decides what is drawn: the mask only has to exist, all opaque.
    let mask_bits = [0u8; MASK_BYTES];
    let mask = unsafe { CreateBitmap(metric, metric, 1, 1, mask_bits.as_ptr().cast()) };
    let Some(mask) = OwnedGdiObject::new(mask) else {
        return null_mut();
    };

    // CreateIconIndirect copies both bitmaps, which are then freed on return.
    let icon_info = ICONINFO {
        fIcon: TRUE,
        xHotspot: 0,
        yHotspot: 0,
        hbmMask: mask.0,
        hbmColor: color.0,
    };
    unsafe { CreateIconIndirect(&raw const icon_info) }
}

fn add_tray_icon(hwnd: HWND) {
    // Drawn anew on every add, which also covers an Explorer restart; the
    // size is that of the DPI Steam runs at, which may lag a later change.
    let icon = create_tray_icon();

    let mut data = tray_icon_data(hwnd);
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_USER;
    data.hIcon = if icon.is_null() {
        unsafe { LoadIconW(null_mut(), IDI_APPLICATION) }
    } else {
        icon
    };
    write_tray_tip(&mut data.szTip);
    unsafe { Shell_NotifyIconW(NIM_ADD, &raw const data) };

    destroy_tray_icon(TRAY_ICON.swap(icon, Ordering::AcqRel));
}

fn destroy_tray_icon(icon: HICON) {
    if !icon.is_null() {
        unsafe { DestroyIcon(icon) };
    }
}

fn update_tray_tip(hwnd: HWND) {
    let mut data = tray_icon_data(hwnd);
    data.uFlags = NIF_TIP;
    write_tray_tip(&mut data.szTip);
    unsafe { Shell_NotifyIconW(NIM_MODIFY, &raw const data) };
}

fn delete_tray_icon(hwnd: HWND) {
    let data = tray_icon_data(hwnd);
    unsafe { Shell_NotifyIconW(NIM_DELETE, &raw const data) };
}

fn show_context_menu(hwnd: HWND) {
    let menu = unsafe { CreatePopupMenu() };
    if menu.is_null() {
        return;
    }

    // TrackPopupMenu returns 0 both for a dismissed menu and for an item whose
    // identifier is 0, so the items are numbered from 1.
    let check = if ENABLED.load(Ordering::Acquire) {
        MF_CHECKED
    } else {
        MF_UNCHECKED
    };
    let mut cursor = POINT { x: 0, y: 0 };
    let selection = unsafe {
        AppendMenuW(
            menu,
            MF_STRING | check,
            MENU_ITEM_TOGGLE,
            wide!("Toggle").as_ptr(),
        );
        SetForegroundWindow(hwnd);

        GetCursorPos(&raw mut cursor);
        let selection = TrackPopupMenu(
            menu,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
            cursor.x,
            cursor.y,
            0,
            hwnd,
            null(),
        );

        // Documented workaround: the menu only closes on an outside click once
        // the owner window has received another message.
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        selection
    };

    if usize::try_from(selection) != Ok(MENU_ITEM_TOGGLE) {
        return;
    }

    // Only this thread writes the setting, so flipping it cannot race.
    let enabled = !ENABLED.fetch_xor(true, Ordering::AcqRel);
    save_enabled(enabled);

    let refresh_event = REFRESH_EVENT.load(Ordering::Acquire);
    if !refresh_event.is_null() {
        unsafe { SetEvent(refresh_event) };
    }
}

unsafe extern "system" fn tray_window_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match msg {
        WM_CREATE => {
            let taskbar_created = unsafe { RegisterWindowMessageW(TASKBAR_CREATED.as_ptr()) };
            TASKBAR_CREATED_MSG.store(taskbar_created, Ordering::Release);
            TRAY_WINDOW.store(hwnd, Ordering::SeqCst);
            add_tray_icon(hwnd);
        }
        WM_USER => {
            if u32::try_from(lparam) == Ok(WM_RBUTTONDOWN) {
                show_context_menu(hwnd);
            }
        }
        WM_TRAY_STATE_CHANGED => update_tray_tip(hwnd),
        WM_DESTROY => {
            TRAY_WINDOW.store(null_mut(), Ordering::Release);
            delete_tray_icon(hwnd);
            destroy_tray_icon(TRAY_ICON.swap(null_mut(), Ordering::AcqRel));
            unsafe { PostQuitMessage(0) };
        }
        _ => {
            let taskbar_created = TASKBAR_CREATED_MSG.load(Ordering::Acquire);
            if taskbar_created != WM_NULL && msg == taskbar_created {
                add_tray_icon(hwnd);
            }
        }
    }
    unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) }
}

unsafe extern "system" fn main_thread_proc(parameter: *mut c_void) -> u32 {
    // Loaded before the hook is set, hence before any monitor thread reads it.
    ENABLED.store(load_enabled(), Ordering::Release);

    let hook = unsafe {
        SetWinEventHook(
            EVENT_OBJECT_CREATE,
            EVENT_OBJECT_CREATE,
            null_mut(),
            Some(win_event_proc),
            GetCurrentProcessId(),
            0,
            WINEVENT_OUTOFCONTEXT,
        )
    };
    if hook.is_null() {
        return EXIT_FAILURE;
    }

    // Created before the message loop, hence before the hook callback can start
    // the monitor thread and before any tray menu can signal it.
    let refresh_event = unsafe { CreateEventW(null(), FALSE, FALSE, null()) };
    REFRESH_EVENT.store(refresh_event, Ordering::Release);

    let instance: HINSTANCE = parameter;
    let mut wc: WNDCLASSW = unsafe { zeroed() };
    wc.lpszClassName = TRAY_WINDOW_CLASS.as_ptr();
    wc.hInstance = instance;
    wc.lpfnWndProc = Some(tray_window_proc);

    let mut msg: MSG = unsafe { zeroed() };
    unsafe {
        RegisterClassW(&raw const wc);
        CreateWindowExW(
            WS_EX_LEFT | WS_EX_LTRREADING,
            wc.lpszClassName,
            wide!("").as_ptr(),
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            null_mut(),
            null_mut(),
            instance,
            null(),
        );

        // GetMessageW returns -1 on error, which must end the loop as well.
        while GetMessageW(&raw mut msg, null_mut(), 0, 0) > 0 {
            TranslateMessage(&raw const msg);
            DispatchMessageW(&raw const msg);
        }

        UnhookWinEvent(hook);
    }
    EXIT_SUCCESS
}

#[unsafe(export_name = "DllMainCRTStartup")]
extern "system" fn dll_main(module: HINSTANCE, reason: u32, _reserved: *mut c_void) -> i32 {
    if reason == DLL_PROCESS_ATTACH {
        unsafe { DisableThreadLibraryCalls(module) };

        // Pinned, so that a FreeLibrary can never unmap the code that the
        // threads, the hook and the window procedure keep running; nothing
        // then has to be torn down on detach. Should pinning fail, the DLL
        // still loads, as it did before.
        let mut pinned: HINSTANCE = null_mut();
        unsafe {
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                module.cast_const().cast(),
                &raw mut pinned,
            )
        };

        // Starting the thread is the only other work done under the loader
        // lock, and it is never waited on. A failure is not propagated:
        // returning FALSE here would abort the load and take Steam down with
        // it.
        let thread =
            unsafe { CreateThread(null(), 0, Some(main_thread_proc), module, 0, null_mut()) };
        drop(OwnedHandle::new(thread));
    }
    TRUE
}

/// No code path is meant to panic, and a release build is checked for it: the
/// handler is then discarded and `ExitThread` never imported. Should one slip
/// through, only the faulting thread ends, rather than Steam with it.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { ExitThread(EXIT_FAILURE) }
}

/// The prebuilt `core` is compiled for unwinding and still references the
/// personality routine wherever a panic path survives, as it does without
/// optimization. Nothing ever unwinds under panic = "abort", so it is never
/// called.
#[cfg(debug_assertions)]
#[unsafe(no_mangle)]
extern "C" fn rust_eh_personality() {}

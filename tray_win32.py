"""A single native Windows notification-area dot, with no visible window."""
import ctypes as C
from ctypes import wintypes as W
import json
import time


def yellow_preview(request, now):
    """A bounded display-only test; never modifies the real task state."""
    if not isinstance(request, dict) or request.get("action") != "test-yellow":
        return False
    try:
        return 0 < float(request["expires_at"]) - now <= 10
    except (KeyError, TypeError, ValueError):
        return False


def run_tray(reader, config, publisher):
    from codex_light import RuntimeProbe, snapshot

    user = C.WinDLL("user32", use_last_error=True)
    shell = C.WinDLL("shell32", use_last_error=True)
    kernel = C.WinDLL("kernel32", use_last_error=True)
    callback_type = C.WINFUNCTYPE(C.c_ssize_t, W.HWND, W.UINT, W.WPARAM, W.LPARAM)

    class WindowClass(C.Structure):
        _fields_ = [("style", W.UINT), ("procedure", callback_type),
                    ("class_extra", C.c_int), ("window_extra", C.c_int),
                    ("instance", W.HINSTANCE), ("icon", W.HICON),
                    ("cursor", W.HANDLE), ("background", W.HANDLE),
                    ("menu_name", W.LPCWSTR), ("class_name", W.LPCWSTR)]

    class NotifyIcon(C.Structure):
        _fields_ = [("size", W.DWORD), ("window", W.HWND), ("id", W.UINT),
                    ("flags", W.UINT), ("message", W.UINT), ("icon", W.HICON),
                    ("tip", W.WCHAR * 128), ("state", W.DWORD), ("state_mask", W.DWORD),
                    ("info", W.WCHAR * 256), ("version", W.UINT),
                    ("info_title", W.WCHAR * 64), ("info_flags", W.DWORD),
                    ("guid", C.c_byte * 16), ("balloon_icon", W.HICON)]

    def api(library, name, result, *arguments):
        function = getattr(library, name)
        function.restype = result
        function.argtypes = list(arguments)
        return function

    api(kernel, "GetModuleHandleW", W.HMODULE, W.LPCWSTR)
    api(user, "RegisterClassW", W.ATOM, C.POINTER(WindowClass))
    api(user, "CreateWindowExW", W.HWND, W.DWORD, W.LPCWSTR, W.LPCWSTR,
        W.DWORD, C.c_int, C.c_int, C.c_int, C.c_int, W.HWND, W.HMENU, W.HINSTANCE, C.c_void_p)
    api(user, "DefWindowProcW", C.c_ssize_t, W.HWND, W.UINT, W.WPARAM, W.LPARAM)
    api(user, "RegisterWindowMessageW", W.UINT, W.LPCWSTR)
    api(user, "SetTimer", C.c_size_t, W.HWND, C.c_size_t, W.UINT, C.c_void_p)
    api(user, "KillTimer", W.BOOL, W.HWND, C.c_size_t)
    api(user, "DestroyWindow", W.BOOL, W.HWND)
    api(user, "PostQuitMessage", None, C.c_int)
    api(user, "GetMessageW", W.BOOL, C.POINTER(W.MSG), W.HWND, W.UINT, W.UINT)
    api(user, "TranslateMessage", W.BOOL, C.POINTER(W.MSG))
    api(user, "DispatchMessageW", C.c_ssize_t, C.POINTER(W.MSG))
    api(user, "CreateIcon", W.HICON, W.HINSTANCE, C.c_int, C.c_int,
        W.BYTE, W.BYTE, C.c_void_p, C.c_void_p)
    api(user, "DestroyIcon", W.BOOL, W.HICON)
    api(user, "CreatePopupMenu", W.HMENU)
    api(user, "AppendMenuW", W.BOOL, W.HMENU, W.UINT, C.c_size_t, W.LPCWSTR)
    api(user, "GetCursorPos", W.BOOL, C.POINTER(W.POINT))
    api(user, "SetForegroundWindow", W.BOOL, W.HWND)
    api(user, "TrackPopupMenu", W.UINT, W.HMENU, W.UINT, C.c_int, C.c_int,
        C.c_int, W.HWND, C.c_void_p)
    api(user, "DestroyMenu", W.BOOL, W.HMENU)
    api(user, "PostMessageW", W.BOOL, W.HWND, W.UINT, W.WPARAM, W.LPARAM)
    api(shell, "Shell_NotifyIconW", W.BOOL, W.DWORD, C.POINTER(NotifyIcon))

    colors = {"running": (239, 68, 68), "completed": (34, 197, 94),
              "waiting": (250, 204, 21), "interrupted": (100, 116, 139), "unknown": (148, 163, 184)}
    names = {"running": "运行中 · 1", "completed": "本轮已结束 · 0",
             "waiting": "等待确认", "interrupted": "已中断", "unknown": "状态未知"}
    icons = {}
    for state, (red, green, blue) in colors.items():
        mask = bytearray(128)
        pixels = bytearray()
        for y in range(32):
            for x in range(32):
                inside = (x - 15.5) ** 2 + (y - 15.5) ** 2 <= 13 ** 2
                if not inside:
                    mask[y * 4 + x // 8] |= 1 << (7 - x % 8)
                pixels.extend((blue, green, red, 255) if inside else (0, 0, 0, 0))
        icon = user.CreateIcon(None, 32, 32, 1, 32,
                               C.create_string_buffer(bytes(mask)), C.create_string_buffer(bytes(pixels)))
        if not icon:
            raise C.WinError(C.get_last_error())
        icons[state] = icon

    probe = RuntimeProbe()
    tray_message = 0x8001
    taskbar_created = user.RegisterWindowMessageW("TaskbarCreated")
    notification = NotifyIcon()
    notification.size = C.sizeof(NotifyIcon)
    notification.id = 1
    notification.flags = 1 | 2 | 4  # callback, icon, tooltip
    notification.message = tray_message
    notification.icon = icons["unknown"]
    notification.tip = "Codex · 正在连接"
    added = False
    previous = None
    latest = None

    def update():
        nonlocal added, previous, latest
        latest = snapshot(reader, probe.info)
        try:
            publisher.publish(latest)
            write_error = False
        except OSError:
            write_error = True
        state = latest["state"]
        tip = f"Codex · {names[state]}\n{config.get('label', '当前会话')}\n{latest['reason']}"
        try:
            request = json.loads((publisher.directory / "display-request.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            request = None
        testing = yellow_preview(request, time.time())
        if testing:
            state = "waiting"
            tip = "Codex · 等待确认（8秒演示）\n仅测试显示，真实任务状态未改变"
        if write_error:
            tip += "\n状态文件写入失败"
        key = (state, tip)
        if key != previous or not added:
            notification.icon = icons[state]
            notification.tip = tip[:127]
            if added:
                added = bool(shell.Shell_NotifyIconW(1, C.byref(notification)))
            if not added:
                added = bool(shell.Shell_NotifyIconW(0, C.byref(notification)))
            previous = key
            try:
                display = {"display_state": state, "is_test": testing,
                           "task_state": latest["state"], "icon_registered": added,
                           "updated_at": time.time()}
                temporary = publisher.directory / "tray-display.tmp"
                temporary.write_text(json.dumps(display, ensure_ascii=False, indent=2), encoding="utf-8")
                temporary.replace(publisher.directory / "tray-display.json")
                with (publisher.directory / "tray-display-events.jsonl").open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(display) + "\n")
            except OSError:
                pass

    @callback_type
    def procedure(window, message, wparam, lparam):
        nonlocal added
        if message == 0x0113:  # WM_TIMER
            update()
            return 0
        if message == taskbar_created:
            added = False
            update()
            return 0
        if message == tray_message and lparam in (0x0205, 0x007B):
            menu = user.CreatePopupMenu()
            label = names.get(latest["state"], "状态未知") if latest else "正在连接"
            user.AppendMenuW(menu, 0x0002, 0, "Codex · " + label)
            user.AppendMenuW(menu, 0x0800, 0, None)
            user.AppendMenuW(menu, 0, 1, "退出红绿灯")
            point = W.POINT()
            user.GetCursorPos(C.byref(point))
            user.SetForegroundWindow(window)
            choice = user.TrackPopupMenu(menu, 0x0100 | 0x0080 | 0x0002,
                                         point.x, point.y, 0, window, None)
            user.DestroyMenu(menu)
            user.PostMessageW(window, 0, 0, 0)
            if choice == 1:
                user.DestroyWindow(window)
            return 0
        if message == 0x0010:  # WM_CLOSE
            user.DestroyWindow(window)
            return 0
        if message == 0x0002:  # WM_DESTROY
            user.PostQuitMessage(0)
            return 0
        return user.DefWindowProcW(window, message, wparam, lparam)

    instance = kernel.GetModuleHandleW(None)
    cls = WindowClass()
    cls.procedure = procedure
    cls.instance = instance
    cls.class_name = "CodexTrafficLightTray"
    if not user.RegisterClassW(C.byref(cls)):
        raise C.WinError(C.get_last_error())
    # Invisible top-level window receives timers and Explorer restart broadcasts.
    window = user.CreateWindowExW(0, cls.class_name, "", 0, 0, 0, 0, 0,
                                   None, None, instance, None)
    if not window:
        raise C.WinError(C.get_last_error())
    notification.window = window
    try:
        update()
        if not added:
            raise RuntimeError("Windows 托盘图标注册失败")
        if not user.SetTimer(window, 1, 500, None):
            raise C.WinError(C.get_last_error())
        message = W.MSG()
        while True:
            result = user.GetMessageW(C.byref(message), None, 0, 0)
            if result == -1:
                raise C.WinError(C.get_last_error())
            if result == 0:
                break
            user.TranslateMessage(C.byref(message))
            user.DispatchMessageW(C.byref(message))
    finally:
        probe.stopping.set()
        user.KillTimer(window, 1)
        shell.Shell_NotifyIconW(2, C.byref(notification))
        user.DestroyWindow(window)
        for icon in icons.values():
            user.DestroyIcon(icon)
        if latest:
            latest.update(state="unknown", value=None, reason="监控器已关闭")
            publisher.publish(latest)

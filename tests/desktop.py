"""Send real WM_DELETE_WINDOW events on an isolated Xvfb display."""
import ctypes as c
import json
from pathlib import Path
import subprocess
import time


def check_desktop(binary, root, env, processes, start, eventually, default_source):
    display_server = subprocess.Popen(["Xvfb", "-displayfd", "1", "-screen", "0", "1280x800x24"],
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    processes.append(display_server)
    env["DISPLAY"] = ":" + display_server.stdout.readline().strip()
    x = c.CDLL("libX11.so.6")
    x.XOpenDisplay.argtypes = [c.c_char_p]
    x.XOpenDisplay.restype = c.c_void_p
    display = x.XOpenDisplay(env["DISPLAY"].encode())
    assert display, "Cannot open isolated Xvfb display"
    x.XDefaultRootWindow.argtypes = [c.c_void_p]
    x.XDefaultRootWindow.restype = c.c_ulong
    x.XQueryTree.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_ulong),
                            c.POINTER(c.POINTER(c.c_ulong)), c.POINTER(c.c_uint)]
    x.XFetchName.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_void_p)]
    x.XFree.argtypes = [c.c_void_p]
    x.XInternAtom.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
    x.XInternAtom.restype = c.c_ulong
    x.XSendEvent.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.c_void_p]
    x.XFlush.argtypes = [c.c_void_p]
    x.XCloseDisplay.argtypes = [c.c_void_p]
    root_window = x.XDefaultRootWindow(display)

    def window():
        root_id, parent, count = c.c_ulong(), c.c_ulong(), c.c_uint()
        nodes = c.POINTER(c.c_ulong)()
        x.XQueryTree(display, root_window, c.byref(root_id), c.byref(parent), c.byref(nodes), c.byref(count))
        found = 0
        for index in range(count.value):
            name = c.c_void_p()
            if x.XFetchName(display, nodes[index], c.byref(name)) and name:
                if c.string_at(name) == b"Soundux":
                    found = nodes[index]
                x.XFree(name)
        if nodes:
            x.XFree(nodes)
        return found

    class Data(c.Union):
        _fields_ = [("longs", c.c_long * 5), ("bytes", c.c_char * 20)]

    class Message(c.Structure):
        _fields_ = [("type", c.c_int), ("serial", c.c_ulong), ("send_event", c.c_int),
                    ("display", c.c_void_p), ("window", c.c_ulong), ("message_type", c.c_ulong),
                    ("format", c.c_int), ("data", Data)]

    def close_window():
        message = Message()
        message.type, message.display, message.window, message.format = 33, display, window(), 32
        assert message.window, "Soundux window disappeared"
        message.message_type = x.XInternAtom(display, b"WM_PROTOCOLS", 0)
        message.data.longs[0] = x.XInternAtom(display, b"WM_DELETE_WINDOW", 0)
        event = c.create_string_buffer(24 * c.sizeof(c.c_long))
        c.memmove(event, c.byref(message), c.sizeof(message))
        x.XSendEvent(display, message.window, 0, 0, event)
        x.XFlush(display)

    try:
        config = root / "config" / "Soundux" / "config.json"
        config.parent.mkdir(parents=True, exist_ok=True)
        for minimize in (True, False):
            config.write_text(json.dumps({
                "data": {"height": 720, "width": 1280, "tabs": [], "soundIdCounter": 0},
                "settings": {"audioBackend": 1, "minimizeToTray": minimize, "localVolume": 31,
                             "remoteVolume": 72, "stopHotkey": [9], "pushToTalkKeys": [65],
                             "useAsDefaultDevice": True}
            }))
            process = start(f"desktop-{minimize}", [str(Path(binary).resolve())])
            eventually(window, "Soundux desktop window")
            eventually(lambda: default_source() == "soundux_source", "persisted default mode applied at startup")
            # Allow the web process to finish setting up tray entries.
            time.sleep(3)
            close_window()
            if minimize:
                time.sleep(4)
                assert process.poll() is None, "Minimize-to-tray armed the shutdown watchdog"
                process.terminate()
                process.wait(timeout=5)
            else:
                assert process.wait(timeout=8) == 0, "Real window close failed"
                saved = json.loads(config.read_text())
                assert saved["settings"]["localVolume"] == 31 and saved["settings"]["stopHotkey"] == [9]
                assert saved["settings"]["useAsDefaultDevice"], "Default mode not preserved across restart"
                eventually(lambda: default_source() == "test_mic", "desktop shutdown restores microphone")
        print("Desktop close/minimize and config preservation passed")
    finally:
        x.XCloseDisplay(display)

"""Exercise the real backend against private PipeWire/WirePlumber instances, never host audio."""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


def eventually(predicate, description):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"Timed out: {description}")


with tempfile.TemporaryDirectory(prefix="soundux-pipewire-", dir=os.environ.get("SOUNDUX_TEST_TMPDIR")) as directory:
    root = Path(directory)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), PIPEWIRE_RUNTIME_DIR=str(runtime),
               PIPEWIRE_REMOTE="pipewire-0", XDG_CONFIG_HOME=str(root / "config"),
               XDG_STATE_HOME=str(root / "state"), XDG_CACHE_HOME=str(root / "cache"),
               PULSE_SERVER=f"unix:{runtime}/no-pulse", HOME=str(root))
    processes = []
    logs = []

    def start(name, command, **kwargs):
        log = open(root / f"{name}.log", "w+")
        logs.append(log)
        process = subprocess.Popen(command, env=env, stderr=log, stdout=log, **kwargs)
        processes.append(process)
        return process

    def run(*command):
        return subprocess.check_output(command, env=env, text=True, timeout=5)

    def graph():
        return json.loads(run("pw-dump"))

    def metadata():
        for obj in graph():
            if obj["type"] == "PipeWire:Interface:Metadata" and obj.get("props", {}).get("metadata.name") == "default":
                return {item["key"]: item.get("value") for item in obj.get("metadata", []) if item["subject"] == 0}
        return {}

    def source(key):
        value = metadata().get(key)
        if isinstance(value, str):
            value = json.loads(value)
        return value.get("name") if isinstance(value, dict) else None

    def set_default(name):
        run("pw-metadata", "-n", "default", "0", "default.configured.audio.source",
            json.dumps({"name": name}), "Spa:String:JSON")
        eventually(lambda: source("default.audio.source") == name, f"default source {name}")

    def no_internal_nodes():
        return not any(obj.get("info", {}).get("props", {}).get("node.name", "").startswith("soundux") for obj in graph())

    def audio_path_linked():
        objects = graph()
        names = {obj["id"]: obj.get("info", {}).get("props", {}).get("node.name") for obj in objects}
        links = {(names.get(obj["info"]["output-node-id"]), names.get(obj["info"]["input-node-id"]))
                 for obj in objects if obj["type"] == "PipeWire:Interface:Link"}
        return {("test_mic", "soundux_mic"), ("soundux_mic_playback", "soundux_sink"),
                ("soundux_sink", "soundux_source_capture")} <= links

    try:
        start("pipewire", ["pipewire", "-c", str(Path(__file__).with_name("pipewire.conf").resolve())])
        eventually(lambda: (runtime / "pipewire-0").exists(), "private server socket")
        # WirePlumber's policy-only profile never enumerates host hardware.
        start("wireplumber", ["wireplumber", "--profile=policy"])
        eventually(lambda: "default.audio.source" in metadata(), "default metadata")
        set_default("test_mic")

        def backend():
            process = subprocess.Popen([str(Path(sys.argv[1]).resolve()), "--pipewire"], env=env,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, bufsize=1)
            processes.append(process)
            lines = queue.Queue()

            def reader():
                for line in process.stdout:
                    print(line, end="", flush=True)
                    lines.put(line.strip())
                lines.put("EOF")

            threading.Thread(target=reader, daemon=True).start()

            def wait(marker):
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
                    if line == marker:
                        return
                    if line == "EOF":
                        raise AssertionError(f"Backend exited before {marker}")
                raise AssertionError(f"Backend did not emit {marker}")

            wait("TEST_READY")

            def command(text):
                process.stdin.write(text + "\n")
                process.stdin.flush()
                wait("TEST_DONE")

            return process, command

        process, command = backend()
        for _ in range(3):
            command("default")
            eventually(lambda: source("default.audio.source") == "soundux_source", "Soundux default")
            eventually(audio_path_linked, "microphone and sink-monitor links")
            command("default")  # idempotent
            command("revert")
            eventually(lambda: source("default.audio.source") == "test_mic", "original microphone restored")
            assert source("default.configured.audio.source") == "test_mic"

        command("default")
        eventually(lambda: source("default.audio.source") == "soundux_source", "default before external change")
        set_default("other_mic")
        command("revert")
        assert source("default.configured.audio.source") == "other_mic", "external choice overwritten"
        command("destroy")
        assert process.wait(timeout=5) == 0
        eventually(no_internal_nodes, "Soundux nodes removed after destroy")

        # Simulate a killed instance whose persistent configured default outlives its virtual source.
        set_default("test_mic")
        process, command = backend()
        command("default")
        eventually(lambda: source("default.audio.source") == "soundux_source", "default before crash")
        process.kill()
        process.wait(timeout=5)
        eventually(no_internal_nodes, "crashed instance cleaned by server")
        process, command = backend()
        command("default")
        eventually(lambda: source("default.audio.source") == "soundux_source", "default after crash")
        command("destroy")
        assert process.wait(timeout=5) == 0
        eventually(no_internal_nodes, "restarted instance cleaned up")
        assert source("default.configured.audio.source") != "soundux_source", "stale configured source restored"

        set_default("test_mic")
        process, command = backend()
        command("default")
        eventually(lambda: source("default.audio.source") == "soundux_source", "default before shutdown")
        command("destroy")
        assert process.wait(timeout=5) == 0
        eventually(lambda: source("default.audio.source") == "test_mic", "shutdown restores microphone")
        eventually(no_internal_nodes, "shutdown cleans graph")
        print("Native PipeWire default/revert, repeated toggles, external changes and cleanup passed")
        if len(sys.argv) > 2:
            from desktop import check_desktop
            check_desktop(sys.argv[2], root, env, processes, start, eventually,
                          lambda: source("default.audio.source"))
    except BaseException:
        for log in logs:
            log.flush()
            log.seek(0)
            print(f"--- {log.name} ---\n{log.read()}", file=sys.stderr)
        raise
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in logs:
            log.close()

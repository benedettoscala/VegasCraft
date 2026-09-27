"""Cross-process tests: production x86 native bridge versus production x64 Java bridge."""
import argparse
import ctypes
import mmap
import pathlib
import struct
import subprocess
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
SIZE = 0x20000 + (32 << 20) + 3840 * 2160 * 4 * 3 + (64 << 20)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--java-home", required=True, type=pathlib.Path)
    parser.add_argument("--native-dir", default=ROOT / "build/native", type=pathlib.Path)
    args = parser.parse_args()
    java = str(args.java_home / "bin/java.exe")
    javac = str(args.java_home / "bin/javac.exe")
    classes = ROOT / "build/java-tests"
    classes.mkdir(parents=True, exist_ok=True)
    subprocess.run([javac, "-d", str(classes),
                    str(ROOT / "fabric/src/main/java/dev/vegascraft/TelemetryBridge.java"),
                    str(ROOT / "tests/java/BridgePeer.java")], check=True)
    command = [java, "--enable-native-access=ALL-UNNAMED", "-cp", str(classes), "BridgePeer"]
    name = "Local\\VegasCraft_test_" + uuid.uuid4().hex
    host = subprocess.Popen([str(args.native_dir / "bridge_host.exe"), "3", name],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert host.stdout.readline().strip() == "READY x86 host"
        peer = subprocess.run(command + [name], text=True, capture_output=True, timeout=15)
        output, error = host.communicate(timeout=10)
        assert host.returncode == 0, (output, error, peer.stdout, peer.stderr)
        assert peer.returncode == 0, (peer.stdout, peer.stderr)
        assert "ACK Minecraft state received" in output, output
        print(peer.stdout.strip())
    finally:
        if host.poll() is None:
            host.terminate()
            host.wait(timeout=10)
    k32 = ctypes.windll.kernel32
    k32.GetTickCount64.restype = ctypes.c_uint64
    for case, size, version in [("wrong_version", SIZE, 99), ("truncated_mapping", 4096, 20)]:
        name = "Local\\VegasCraft_test_" + uuid.uuid4().hex
        with mmap.mmap(-1, size, tagname=name) as mapping:
            struct.pack_into("<IIIIQQ", mapping, 0, 0x43594B53, version, 1234, 0, k32.GetTickCount64(), 0)
            result = subprocess.run(command + [name, "reject"], capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, (case, result.stdout, result.stderr)
            print("PASS", case, result.stdout.strip())


if __name__ == "__main__":
    main()

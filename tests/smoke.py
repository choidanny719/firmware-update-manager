import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve()


def run(*args):
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True, timeout=10, check=True)
    return json.loads(result.stdout)


with tempfile.TemporaryDirectory() as directory:
    flash = Path(directory) / "device.flash"
    stream = Path(directory) / "update.packet"
    subprocess.run([sys.executable, str(root / "tools/sender.py"), str(root / "examples/v1.txt"),
                    "--version", "1", "--output", str(stream)], check=True, timeout=5)
    assert run("init", flash)["result"] == "ok"
    report = run("upload", flash, stream)
    assert report["phase"] == "ready"
    assert report["receiver_packets"] == report["controller_packets"] == 3
    assert report["spi_commands"] > 0
    assert report["received_bytes"] == (root / "examples/v1.txt").stat().st_size
    assert run("boot", flash)["trial"] is True
    assert run("confirm", flash, 1)["confirmed_version"] == 1
    assert run("boot", flash)["trial"] is False

import fcntl
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv.pop(1)).resolve()
SPEC = importlib.util.spec_from_file_location("sender", ROOT / "tools/sender.py")
sender = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sender)
CAPACITY = 256 * 1024
OLD = bytes((index * 17) % 256 for index in range(600))
NEW = bytes(value ^ 0xa5 for value in OLD)


def frame(kind, sequence, body=b""):
    prefix = b"\x55\xaa" + struct.pack("<BIH", kind, sequence, len(body)) + body
    return prefix + struct.pack("<I", zlib.crc32(prefix))


def frames(image, version, chunk=256, checksum=None):
    checksum = zlib.crc32(image) if checksum is None else checksum
    result = [frame(1, 0, struct.pack("<III", version, len(image), checksum))]
    for offset in range(0, len(image), chunk):
        result.append(frame(2, len(result), image[offset:offset + chunk]))
    result.append(frame(3, len(result)))
    return result


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.flash = self.root / "device.flash"
        self.stream = self.root / "update.packet"
        self.run_command("init", self.flash)

    def run_command(self, *args, expected="ok", timeout=20):
        result = subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True, timeout=timeout)
        self.assertEqual(result.returncode, 0 if expected == "ok" else 1, result.stderr + result.stdout)
        report = json.loads(result.stdout)
        self.assertEqual(report["result"], expected, report)
        return report

    def upload(self, image=NEW, version=2, chunk=256, options=()):
        self.stream.write_bytes(b"".join(frames(image, version, chunk)))
        return self.run_command("upload", self.flash, self.stream, *options)

    def baseline(self):
        self.upload(OLD, 1)
        self.run_command("boot", self.flash)
        self.run_command("confirm", self.flash, 1)

    def assert_selected_bytes(self, report, expected):
        with self.flash.open("rb") as source:
            source.seek(8192 + report["slot"] * CAPACITY)
            self.assertEqual(source.read(len(expected)), expected)

    def test_full_lifecycle_persists_across_separate_processes(self):
        self.baseline()
        uploaded = self.upload()
        self.assertEqual(uploaded["phase"], "ready")
        self.assertEqual(uploaded["received_bytes"], len(NEW))
        self.assertEqual(uploaded["receiver_packets"], uploaded["controller_packets"])
        self.assertEqual(uploaded["controller_packets"], 5)
        self.assertGreater(uploaded["spi_commands"], 0)
        self.assertLessEqual(uploaded["queue_high_water"], 4)
        trial = self.run_command("boot", self.flash)
        self.assertEqual(trial["selected_version"], 2)
        self.assertTrue(trial["trial"])
        self.assert_selected_bytes(trial, NEW)
        self.run_command("confirm", self.flash, 2)
        confirmed = self.run_command("boot", self.flash)
        self.assertFalse(confirmed["trial"])
        self.assertEqual(confirmed["selected_version"], 2)

    def test_unconfirmed_image_rolls_back(self):
        self.baseline()
        self.upload()
        self.assertTrue(self.run_command("boot", self.flash)["trial"])
        recovered = self.run_command("boot", self.flash)
        self.assertEqual(recovered["selected_version"], 1)
        self.assertFalse(recovered["trial"])
        self.assert_selected_bytes(recovered, OLD)
        self.upload()

    def test_empty_flash_and_unconfirmed_first_install_have_no_fallback(self):
        self.run_command("boot", self.flash, expected="no_bootable_image")
        self.upload(OLD, 1)
        self.run_command("boot", self.flash)
        self.run_command("boot", self.flash, expected="no_bootable_image")
        self.assertEqual(self.run_command("status", self.flash)["confirmed_version"], 0)

    def test_repeated_updates_alternate_slots_and_reject_old_versions(self):
        self.baseline()
        self.stream.write_bytes(b"".join(frames(NEW, 1)))
        self.run_command("upload", self.flash, self.stream, expected="invalid_input")
        first = self.run_command("boot", self.flash)
        for version in (2, 3):
            self.upload(NEW, version)
            selected = self.run_command("boot", self.flash)
            self.assertNotEqual(first["slot"], selected["slot"])
            self.run_command("confirm", self.flash, version)
            first = selected

    def test_incorrect_confirmation_and_upload_during_trial_are_rejected(self):
        self.baseline()
        self.run_command("confirm", self.flash, 1, expected="invalid_state")
        self.upload()
        self.run_command("confirm", self.flash, 2, expected="invalid_state")
        self.run_command("upload", self.flash, self.stream, expected="invalid_state")
        self.run_command("boot", self.flash)
        self.run_command("confirm", self.flash, 3, expected="invalid_state")
        self.run_command("upload", self.flash, self.stream, expected="invalid_state")
        self.run_command("confirm", self.flash, 2)

    def test_bad_sequences_missing_end_and_trailing_packets_never_publish(self):
        self.baseline()
        packets = frames(NEW, 2)
        cases = [
            (packets[1:], "out_of_sequence"),
            ([frame(3, 0)], "invalid_state"),
            ([packets[0], packets[2], packets[-1]], "out_of_sequence"),
            ([packets[0], packets[1], packets[1]], "out_of_sequence"),
            (packets[:-1], "incomplete_image"),
            ([packets[0], frame(3, 1)], "incomplete_image"),
            (packets + [frame(3, len(packets))], "invalid_state"),
            (packets + [b"junk"], "invalid_input"),
            ([packets[0], frame(1, 1, struct.pack("<III", 2, len(NEW), zlib.crc32(NEW)))], "invalid_state"),
            ([packets[0], frame(2, 0xffffffff, NEW[:1])], "out_of_sequence"),
        ]
        for payload, expected in cases:
            with self.subTest(expected=expected, payload_lengths=list(map(len, payload))):
                self.stream.write_bytes(b"".join(payload))
                self.run_command("upload", self.flash, self.stream, expected=expected)
                state = self.run_command("status", self.flash)
                self.assertEqual(state["phase"], "stable")
                self.assertEqual(state["confirmed_version"], 1)
                self.assertEqual(state["candidate_version"], 0)
        self.upload()

    def test_corrupt_packet_and_whole_image_checksums_are_rejected(self):
        self.baseline()
        packets = frames(NEW, 2)
        altered = bytearray(packets[1])
        altered[10] ^= 1
        packets[1] = bytes(altered)
        self.stream.write_bytes(b"".join(packets))
        self.run_command("upload", self.flash, self.stream, expected="corrupt_data")
        self.stream.write_bytes(b"".join(frames(NEW, 2, checksum=123)))
        self.run_command("upload", self.flash, self.stream, expected="corrupt_data")
        self.assertEqual(self.run_command("boot", self.flash)["selected_version"], 1)

    def test_empty_truncated_and_oversized_streams_fail_without_hanging(self):
        self.baseline()
        data = b"".join(frames(NEW, 2))
        for content in (b"", data[:1], data[:8], data[:24], data[:-1]):
            with self.subTest(length=len(content)):
                self.stream.write_bytes(content)
                self.run_command("upload", self.flash, self.stream, expected="incomplete_image")
        with self.stream.open("wb") as stream:
            stream.truncate(4 * 1024 * 1024 + 1)
        self.run_command("upload", self.flash, self.stream, expected="invalid_input")

    def test_manifest_limits_and_payload_overruns_are_rejected(self):
        self.baseline()
        for version, size in ((0, 1), (2, 0), (2, CAPACITY + 1), (2, 0xffffffff)):
            with self.subTest(version=version, size=size):
                self.stream.write_bytes(frame(1, 0, struct.pack("<III", version, size, 0)))
                self.run_command("upload", self.flash, self.stream, expected="invalid_input")
        self.stream.write_bytes(frame(1, 0, struct.pack("<III", 2, 1, 0)) + frame(2, 1, b"ab"))
        self.run_command("upload", self.flash, self.stream, expected="invalid_input")

    def test_one_byte_chunks_and_maximum_image_cross_the_rtos_pipeline(self):
        self.upload(OLD, 1, chunk=1)
        self.run_command("boot", self.flash)
        self.run_command("confirm", self.flash, 1)
        image = bytes(range(256)) * 1024
        uploaded = self.upload(image, 2)
        self.assertEqual(uploaded["received_bytes"], CAPACITY)
        self.assertEqual(uploaded["receiver_packets"], 1026)
        selected = self.run_command("boot", self.flash)
        self.assert_selected_bytes(selected, image)

    def test_timeout_preserves_confirmed_state_and_retry_succeeds(self):
        self.baseline()
        self.stream.write_bytes(b"".join(frames(NEW, 2)))
        self.run_command("upload", self.flash, self.stream, "--stuck-busy", expected="timeout")
        self.assertEqual(self.run_command("boot", self.flash)["selected_version"], 1)
        self.run_command("upload", self.flash, self.stream)

    def test_restart_after_each_upload_mutation_keeps_a_valid_image(self):
        self.baseline()
        backup = self.root / "baseline.flash"
        shutil.copyfile(self.flash, backup)
        result = self.upload()
        mutations = result["mutations"]
        for cut in range(1, mutations + 1):
            for tear in (0, 1, 4096):
                with self.subTest(cut=cut, tear=tear):
                    shutil.copyfile(backup, self.flash)
                    self.run_command("upload", self.flash, self.stream, "--cut-after", cut,
                                     "--tear-bytes", tear, expected="power_loss")
                    selected = self.run_command("boot", self.flash)
                    self.assertIn(selected["selected_version"], (1, 2))
                    self.assert_selected_bytes(selected, OLD if selected["selected_version"] == 1 else NEW)
                    if selected["trial"]:
                        selected = self.run_command("boot", self.flash)
                    self.assertEqual(selected["selected_version"], 1)

    def test_power_loss_during_confirmation_recovers_old_or_new_confirmed_image(self):
        self.baseline()
        self.upload()
        self.run_command("boot", self.flash)
        backup = self.root / "trial.flash"
        shutil.copyfile(self.flash, backup)
        for cut in (1, 2, 3):
            for tear in (0, 1, 4096):
                with self.subTest(cut=cut, tear=tear):
                    shutil.copyfile(backup, self.flash)
                    self.run_command("confirm", self.flash, 2, "--cut-after", cut,
                                     "--tear-bytes", tear, expected="power_loss")
                    selected = self.run_command("boot", self.flash)
                    self.assertFalse(selected["trial"])
                    self.assertIn(selected["selected_version"], (1, 2))
                    self.assert_selected_bytes(selected, OLD if selected["selected_version"] == 1 else NEW)

    def test_corrupted_candidate_falls_back_and_corrupted_confirmed_image_fails(self):
        self.baseline()
        self.upload()
        with self.flash.open("r+b") as storage:
            storage.seek(8192 + CAPACITY)
            storage.write(b"\x00")
        selected = self.run_command("boot", self.flash)
        self.assertEqual(selected["selected_version"], 1)
        with self.flash.open("r+b") as storage:
            storage.seek(8192)
            storage.write(b"\xff")
        self.run_command("boot", self.flash, expected="corrupt_data")

    def test_python_sender_cli_produces_a_compatible_upload(self):
        source = self.root / "image.bin"
        source.write_bytes(OLD)
        result = subprocess.run([sys.executable, str(ROOT / "tools/sender.py"), str(source), "--version", "1",
                                 "--chunk-size", "193", "--output", str(self.stream)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.stream.read_bytes(), b"".join(frames(OLD, 1, 193)))
        self.run_command("upload", self.flash, self.stream)

    def test_flash_lock_excludes_concurrent_processes(self):
        with self.flash.open("r+b") as storage:
            fcntl.flock(storage, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.run_command("status", self.flash, expected="io_error")
        self.run_command("status", self.flash)

    def test_files_are_not_overwritten_or_created_implicitly(self):
        existing = self.root / "existing"
        existing.write_bytes(b"keep")
        self.run_command("init", existing, expected="io_error")
        self.assertEqual(existing.read_bytes(), b"keep")
        self.run_command("status", existing, expected="io_error")
        self.run_command("status", self.root / "missing", expected="io_error")
        self.run_command("status", self.root, expected="io_error")
        self.run_command("upload", self.flash, self.root / "missing", expected="io_error")
        self.run_command("upload", self.flash, self.root, expected="invalid_input")

    def test_invalid_command_line_arguments_do_not_start_work(self):
        cases = [[], ["unknown", "x"], ["upload", "x"], ["confirm", "x"], ["init", "x", "extra"],
                 ["status", "x", "extra"], ["confirm", "x", "0"], ["confirm", "x", "-1"],
                 ["confirm", "x", "4294967296"], ["confirm", "x", "1x"], ["confirm", "x", "+1"],
                 ["boot", "x", "--cut-after"], ["boot", "x", "--cut-after", "0"],
                 ["boot", "x", "--cut-after", "99999999999999999999999999"],
                 ["boot", "x", "--tear-bytes", "1"], ["boot", "x", "--cut-after", "1", "--tear-bytes", "4097"],
                 ["boot", "x", "--cut-after", "1", "--cut-after", "2"],
                 ["boot", "x", "--stuck-busy", "--stuck-busy"], ["boot", "x", "--stuck-busy", "--cut-after", "1"],
                 ["boot", "x", "--unknown"], ["--help", "extra"]]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = subprocess.run([str(BINARY), *arguments], capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
                self.assertIn("Usage:", result.stderr)
        result = subprocess.run([str(BINARY), "--help"], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0)


class SenderTests(unittest.TestCase):
    def test_packet_bytes_match_an_independent_encoder(self):
        self.assertEqual(sender.package_image(b"abc", 0xffffffff, 1), b"".join(frames(b"abc", 0xffffffff, 1)))

    def test_empty_oversized_and_nonbinary_images_are_rejected(self):
        for image in (b"", bytes(CAPACITY + 1), "text", None):
            with self.subTest(image_type=type(image)):
                with self.assertRaises(ValueError):
                    sender.package_image(image, 1)

    def test_invalid_versions_and_chunk_sizes_are_rejected(self):
        for version in (0, -1, 0x100000000, 1.5, True, "1"):
            with self.subTest(version=version):
                with self.assertRaises(ValueError):
                    sender.package_image(b"a", version)
        for chunk in (0, -1, 257, 1.5, True, "1"):
            with self.subTest(chunk=chunk):
                with self.assertRaises(ValueError):
                    sender.package_image(b"a", 1, chunk)

    def test_sender_does_not_replace_existing_output_or_invalid_input(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "image.bin"
            output = Path(directory) / "output.packet"
            source.write_bytes(b"abc")
            output.write_bytes(b"keep")
            command = [sys.executable, str(ROOT / "tools/sender.py"), str(source), "--version", "1", "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(output.read_bytes(), b"keep")
            output.unlink()
            source.write_bytes(b"")
            result = subprocess.run(command, capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 2)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)

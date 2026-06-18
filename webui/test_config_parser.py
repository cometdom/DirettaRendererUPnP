"""Unit tests for ShellVarConfig.save() — regression coverage for the
webui save-time deduplication and commented-line preservation fix.

Run from the project root:
    python3 webui/test_config_parser.py

Or from webui/:
    python3 -m unittest test_config_parser
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from config_parser import ShellVarConfig  # noqa: E402


def _write(tmpdir, content):
    path = os.path.join(tmpdir, "diretta-renderer.conf")
    with open(path, "w") as f:
        f.write(content)
    return path


def _read(path):
    with open(path) as f:
        return f.read()


class TestShellVarConfigSave(unittest.TestCase):

    def test_commented_example_lines_are_preserved(self):
        """Commented-out example lines must not be mistaken for active settings.

        Before the fix the regex '^#?\\s*...' would treat '#CPU_AUDIO=2'
        as if it were 'CPU_AUDIO=2' and overwrite it, losing the example.
        """
        with tempfile.TemporaryDirectory() as tmp:
            path = _write(tmp, (
                "# Example values for CPU_AUDIO:\n"
                "#CPU_AUDIO=2\n"
                "#CPU_AUDIO=3,4\n"
                "CPU_AUDIO=\n"
            ))
            ShellVarConfig.save(path, {"CPU_AUDIO": "2"})
            out = _read(path)

            # The two commented example lines must still be present, unchanged.
            self.assertIn("#CPU_AUDIO=2\n", out)
            self.assertIn("#CPU_AUDIO=3,4\n", out)
            # And the previously empty active line is now set.
            self.assertIn("CPU_AUDIO=2\n", out)
            # Exactly one ACTIVE assignment (regex strips leading '#').
            active = [l for l in out.splitlines()
                      if l.startswith("CPU_AUDIO=")]
            self.assertEqual(active, ["CPU_AUDIO=2"])

    def test_multiple_active_duplicates_are_collapsed(self):
        """Pre-existing duplicate active lines (left behind by older webui
        versions or by install.sh's sed migration) collapse to one."""
        with tempfile.TemporaryDirectory() as tmp:
            path = _write(tmp, (
                "TARGET=1\n"
                "CPU_AUDIO=2\n"
                "CPU_AUDIO=2\n"
                "CPU_AUDIO=2\n"
                "PORT=4005\n"
            ))
            ShellVarConfig.save(path, {"CPU_AUDIO": "3"})
            out = _read(path)
            active = [l for l in out.splitlines()
                      if l.startswith("CPU_AUDIO=")]
            self.assertEqual(active, ["CPU_AUDIO=3"])
            # Other keys are untouched.
            self.assertIn("TARGET=1", out)
            self.assertIn("PORT=4005", out)

    def test_save_is_idempotent(self):
        """Calling save() twice with the same settings produces the same file —
        no exponential growth of any kind."""
        with tempfile.TemporaryDirectory() as tmp:
            path = _write(tmp, (
                "# example\n"
                "#CPU_AUDIO=2\n"
                "CPU_AUDIO=\n"
            ))
            settings = {"CPU_AUDIO": "3"}
            ShellVarConfig.save(path, settings)
            once = _read(path)
            ShellVarConfig.save(path, settings)
            twice = _read(path)
            self.assertEqual(once, twice)

    def test_appends_new_key_when_absent(self):
        """A key not present in the file is appended at the end."""
        with tempfile.TemporaryDirectory() as tmp:
            path = _write(tmp, "TARGET=1\n")
            ShellVarConfig.save(path, {"TARGET": "1", "CPU_AUDIO": "2"})
            out = _read(path)
            self.assertIn("TARGET=1", out)
            self.assertIn("CPU_AUDIO=2", out)

    def test_comments_and_blanks_preserved(self):
        """Comments and blank lines around assignments are preserved unchanged."""
        with tempfile.TemporaryDirectory() as tmp:
            original = (
                "# Header comment\n"
                "\n"
                "TARGET=1\n"
                "\n"
                "# Trailing comment\n"
            )
            path = _write(tmp, original)
            ShellVarConfig.save(path, {"TARGET": "2"})
            out = _read(path)
            self.assertIn("# Header comment\n", out)
            self.assertIn("# Trailing comment\n", out)
            self.assertIn("TARGET=2\n", out)
            # Blank line between header and TARGET preserved.
            self.assertIn("# Header comment\n\nTARGET=2\n", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)

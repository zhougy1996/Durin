from __future__ import annotations

import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


PRODUCT_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = PRODUCT_ROOT.parents[1]
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))

from durin_dev_tool import cli
from durin_dev_tool.registry import CommandRegistry
from durin_dev_tool.repository import find_repository_root


class RepositoryDiscoveryTests(unittest.TestCase):
    def test_discovers_repository_from_nested_directory(self) -> None:
        self.assertEqual(find_repository_root(PRODUCT_ROOT / "tests"), REPOSITORY_ROOT)

    def test_rejects_directory_without_repository_markers(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "Could not find"):
                find_repository_root(Path(directory))

    def test_cli_uses_package_location_when_working_directory_is_unrelated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            completed = subprocess.run(
                [sys.executable, str(PRODUCT_ROOT / "durin_dev_tool" / "__main__.py"), "help"],
                cwd=directory,
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("DurinDevTool commands:", completed.stdout)


class CommandRegistryTests(unittest.TestCase):
    def test_direct_help_and_shell_help_share_registry_output(self) -> None:
        registry = CommandRegistry()
        direct_output = io.StringIO()
        self.assertEqual(
            cli.run(
                ["help"],
                registry=registry,
                repository_root=REPOSITORY_ROOT,
                stdout=direct_output,
            ),
            0,
        )
        self.assertEqual(direct_output.getvalue().strip(), registry.format_help())

    def test_missing_environment_stops_before_placeholder_import(self) -> None:
        output = io.StringIO()
        errors = io.StringIO()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch("durin_dev_tool.registry.importlib.import_module") as import_module:
                with self.assertRaisesRegex(RuntimeError, "DevTool setup"):
                    cli.run(
                        ["build"],
                        repository_root=root,
                        stdout=output,
                        stderr=errors,
                    )
        import_module.assert_not_called()

    def test_no_arguments_selects_shell(self) -> None:
        spec, _namespace = CommandRegistry().parse([])
        self.assertEqual(spec.name, "shell")

    def test_shell_startup_uses_durin_prompt(self) -> None:
        from durin_dev_tool.shell import run_shell

        stdout = io.StringIO()
        result = run_shell(
            registry=CommandRegistry(),
            repository_root=REPOSITORY_ROOT,
            stdout=stdout,
            stderr=io.StringIO(),
            input_func=mock.Mock(side_effect=EOFError),
        )
        self.assertEqual(result, 0)
        self.assertIn("Durin Developer Tool shell", stdout.getvalue())


class LauncherTests(unittest.TestCase):
    def test_launcher_prefers_venv_then_python_launcher_then_path_python(self) -> None:
        content = (PRODUCT_ROOT / "DevTool.bat").read_text(encoding="utf-8")
        venv = content.index('if exist "%VENV_PYTHON%"')
        launcher = content.index("where py")
        path_python = content.index("where python")
        self.assertLess(venv, launcher)
        self.assertLess(launcher, path_python)
        self.assertIn('py -3 "%ENTRY_POINT%" %*', content)
        self.assertIn('python "%ENTRY_POINT%" %*', content)
        self.assertIn("EnableDelayedExpansion", content)
        self.assertGreaterEqual(content.count("exit /b !ERRORLEVEL!"), 3)


if __name__ == "__main__":
    unittest.main()

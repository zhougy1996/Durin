from __future__ import annotations

import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[3]
PRODUCT_ROOT = REPO_ROOT / "Tools" / "DurinDevTool"
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))

from durin_dev_tool import cli
from durin_dev_tool.documentation import archive as archive_module
from durin_dev_tool.documentation import handler as handler_module
from durin_dev_tool.documentation.archive import apply_archive, preview_archive
from durin_dev_tool.documentation.plans import (
    PlanStatus,
    load_catalog,
    parse_plan,
    render_listing,
)
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry


PLAN_TEMPLATE = """# {title} Plan

Summary: {summary}

Last reviewed: 2026-07-27

Status: {status}
Completed:{completed}

## Current Status
"""


class PlanCatalogTests(unittest.TestCase):
    def test_legacy_plan_defaults_to_active(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "Legacy.md"
            path.write_text(
                "# Legacy Plan\n\nSummary: Legacy plan.\n\n"
                "Last reviewed: 2026-07-27\n\n## Current Status\n",
                encoding="utf-8",
            )
            plan, errors = parse_plan(path)
        self.assertEqual(errors, [])
        self.assertIsNotNone(plan)
        self.assertEqual(plan.status, PlanStatus.ACTIVE)

    def test_catalog_rejects_duplicate_title_across_active_and_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plans = Path(temporary) / "Plans"
            archive = plans / "Archive" / "2026-07"
            archive.mkdir(parents=True)
            (plans / "Current.md").write_text(
                PLAN_TEMPLATE.format(
                    title="Shared",
                    summary="Current.",
                    status="Active",
                    completed="",
                ),
                encoding="utf-8",
            )
            (archive / "Old.md").write_text(
                PLAN_TEMPLATE.format(
                    title="Shared",
                    summary="Old.",
                    status="Archived",
                    completed=" 2026-07-01",
                ),
                encoding="utf-8",
            )
            catalog = load_catalog(plans)
        self.assertTrue(any("duplicate title" in error for error in catalog.errors))


class ArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.repository = Path(self.temporary.name)
        self.plans = self.repository / "Documentation" / "Plans"
        self.plans.mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(self.repository)], check=True)
        self.plan = self.plans / "Feature.md"
        self.plan.write_text(
            PLAN_TEMPLATE.format(
                title="Feature",
                summary="Completed feature.",
                status="Completed",
                completed=" 2026-07-20",
            ),
            encoding="utf-8",
        )
        self.reference = self.repository / "README.md"
        self.reference.write_text(
            "[Feature](Documentation/Plans/Feature.md)\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_preview_does_not_modify_files(self) -> None:
        preview = preview_archive(self.plans, "2026-07")
        self.assertEqual(len(preview.moves), 1)
        self.assertTrue(self.plan.exists())
        self.assertIn(self.reference.resolve(), preview.reference_files)

    def test_apply_moves_plan_updates_links_and_validates(self) -> None:
        apply_archive(self.plans, "2026-07")
        archived = self.plans / "Archive" / "2026-07" / "Feature.md"
        self.assertFalse(self.plan.exists())
        self.assertIn("Status: Archived", archived.read_text(encoding="utf-8"))
        self.assertEqual(
            self.reference.read_text(encoding="utf-8"),
            "[Feature](Documentation/Plans/Archive/2026-07/Feature.md)\n",
        )
        self.assertEqual(load_catalog(self.plans).errors, ())

    def test_apply_rolls_back_every_file_when_a_write_fails(self) -> None:
        original_write = archive_module._atomic_write
        calls = 0

        def fail_second_write(path: Path, content: bytes) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("simulated write failure")
            original_write(path, content)

        with mock.patch.object(archive_module, "_atomic_write", side_effect=fail_second_write):
            with self.assertRaisesRegex(OSError, "simulated write failure"):
                apply_archive(self.plans, "2026-07")

        self.assertTrue(self.plan.exists())
        self.assertEqual(
            self.reference.read_text(encoding="utf-8"),
            "[Feature](Documentation/Plans/Feature.md)\n",
        )
        self.assertFalse((self.plans / "Archive" / "2026-07" / "Feature.md").exists())


class UnifiedCommandTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.repository = Path(self.temporary.name)
        self.plans = self.repository / "Documentation" / "Plans"
        self.plans.mkdir(parents=True)
        (self.plans / "Active.md").write_text(
            PLAN_TEMPLATE.format(
                title="Active",
                summary="Active plan.",
                status="Active",
                completed="",
            ),
            encoding="utf-8",
        )
        self.registry = CommandRegistry()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _parse_values(self, arguments: list[str]) -> dict[str, object]:
        _, namespace = self.registry.parse(arguments)
        return {
            key: value
            for key, value in vars(namespace).items()
            if key != "_command_spec"
        }

    def test_every_plan_command_has_one_direct_and_shell_request_model(self) -> None:
        commands = (
            ["plan", "list", "--query", "Active", "--format", "markdown"],
            ["plan", "validate", "--scope", "active"],
            ["plan", "archive", "2026-07"],
        )
        for command in commands:
            with self.subTest(command=command):
                direct = self._parse_values(command)
                shell = self._parse_values(command)
                self.assertEqual(direct, shell)

    def test_plan_names_are_case_insensitive_and_accept_slash_aliases(self) -> None:
        expected = self._parse_values(["plan", "list"])
        self.assertEqual(expected, self._parse_values(["PLAN", "LIST"]))
        self.assertEqual(expected, self._parse_values(["/plan", "/list"]))

    def test_list_defaults_to_markdown_direct_and_terminal_in_shell(self) -> None:
        direct_spec, direct = self.registry.parse(["plan", "list"])
        shell_spec, shell = self.registry.parse(["plan", "list"])
        with mock.patch.object(
            handler_module,
            "render_listing",
            return_value="result",
        ) as render:
            self.registry.execute(
                direct_spec,
                direct,
                repository_root=self.repository,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
            self.registry.execute(
                shell_spec,
                shell,
                repository_root=self.repository,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
                session_state={},
            )
        self.assertEqual(
            [call.kwargs["output_format"] for call in render.call_args_list],
            ["markdown", "terminal"],
        )

    def test_explicit_format_has_direct_and_shell_output_parity(self) -> None:
        arguments = ["plan", "list", "--format", "markdown", "--color", "never"]
        outputs: list[str] = []
        for session_state in (None, {}):
            spec, namespace = self.registry.parse(arguments)
            stdout = io.StringIO()
            keywords = {}
            if session_state is not None:
                keywords["session_state"] = session_state
            result = self.registry.execute(
                spec,
                namespace,
                repository_root=self.repository,
                stdout=stdout,
                stderr=io.StringIO(),
                **keywords,
            )
            self.assertEqual(result, 0)
            outputs.append(stdout.getvalue())
        self.assertEqual(outputs[0], outputs[1])

    def test_validate_and_archive_defaults_have_direct_and_shell_output_parity(
        self,
    ) -> None:
        for arguments in (
            ["plan", "validate", "--scope", "active"],
            ["plan", "archive", "2099-01"],
        ):
            with self.subTest(arguments=arguments):
                outputs: list[str] = []
                for session_state in (None, {}):
                    spec, namespace = self.registry.parse(arguments)
                    stdout = io.StringIO()
                    keywords = {}
                    if session_state is not None:
                        keywords["session_state"] = session_state
                    result = self.registry.execute(
                        spec,
                        namespace,
                        repository_root=self.repository,
                        stdout=stdout,
                        stderr=io.StringIO(),
                        **keywords,
                    )
                    self.assertEqual(result, 0)
                    outputs.append(stdout.getvalue())
                self.assertEqual(outputs[0], outputs[1])

    def test_unfiltered_archive_and_all_listings_require_all_results(self) -> None:
        for scope in ("archive", "all"):
            with self.subTest(scope=scope):
                with self.assertRaisesRegex(
                    DevToolError,
                    "archive listings require",
                ):
                    cli.run(
                        ["plan", "list", "--scope", scope],
                        repository_root=self.repository,
                        stdout=io.StringIO(),
                        stderr=io.StringIO(),
                    )

    def test_archive_defaults_to_preview(self) -> None:
        spec, namespace = self.registry.parse(["plan", "archive", "2026-07"])
        with mock.patch.object(handler_module, "preview_archive") as preview:
            preview.return_value = mock.Mock(month="2026-07", moves=())
            result = self.registry.execute(
                spec,
                namespace,
                repository_root=self.repository,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        self.assertEqual(result, 0)
        preview.assert_called_once_with(self.plans, "2026-07")

    def test_archive_apply_is_explicit(self) -> None:
        spec, namespace = self.registry.parse(
            ["plan", "archive", "2026-07", "--apply"]
        )
        with mock.patch.object(handler_module, "apply_archive") as apply:
            apply.return_value = mock.Mock(month="2026-07", moves=())
            result = self.registry.execute(
                spec,
                namespace,
                repository_root=self.repository,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        self.assertEqual(result, 0)
        apply.assert_called_once_with(self.plans, "2026-07")

    def test_archive_listing_groups_plans_by_month(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            plans = Path(temporary) / "Plans"
            for month, title in (("2026-06", "Older"), ("2026-07", "Newer")):
                directory = plans / "Archive" / month
                directory.mkdir(parents=True)
                (directory / f"{title}.md").write_text(
                    PLAN_TEMPLATE.format(
                        title=title,
                        summary=f"{title} plan.",
                        status="Archived",
                        completed=f" {month}-01",
                    ),
                    encoding="utf-8",
                )
            catalog = load_catalog(plans)
            output = render_listing(
                catalog.archived,
                plans,
                scope="archive",
                output_format="markdown",
                color="never",
            )
        self.assertLess(output.index("## 2026-07"), output.index("## 2026-06"))


if __name__ == "__main__":
    unittest.main()

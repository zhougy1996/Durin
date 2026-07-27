from __future__ import annotations

import io
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
DOC_TOOL_DIR = REPO_ROOT / "Tools" / "DocTool"
if str(DOC_TOOL_DIR) not in os.sys.path:
    os.sys.path.insert(0, str(DOC_TOOL_DIR))

from durin_doc_tool import archive as archive_module
from durin_doc_tool import cli
from durin_doc_tool.archive import apply_archive, preview_archive
from durin_doc_tool.plans import (
    PlanStatus,
    load_catalog,
    parse_plan,
    render_listing,
)


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


class CliTests(unittest.TestCase):
    def test_root_wrapper_uses_tools_entrypoint_and_forwards_arguments(self) -> None:
        content = (REPO_ROOT / "DocTool.bat").read_text(encoding="utf-8")
        self.assertIn(
            'Tools\\DocTool\\durin_doc_tool\\__main__.py" %*',
            content,
        )
        self.assertIn("WorktreeTool prepare", content)

    def test_no_arguments_opens_shell(self) -> None:
        with mock.patch.object(cli, "run_shell", return_value=7) as shell:
            self.assertEqual(cli.main([]), 7)
        shell.assert_called_once_with()

    def test_interactive_list_defaults_to_terminal_output(self) -> None:
        with mock.patch.object(cli, "_plans_directory", return_value=Path("unused")):
            with mock.patch.object(cli, "load_catalog") as load:
                catalog = mock.Mock()
                catalog.errors_for.return_value = []
                catalog.select.return_value = [mock.Mock()]
                catalog.plans_directory = Path("plans")
                load.return_value = catalog
                with mock.patch.object(cli, "render_listing", return_value="result") as render:
                    with redirect_stdout(io.StringIO()):
                        args = cli._parser().parse_args(["list"])
                        self.assertEqual(cli._execute(args, interactive=True), 0)
        self.assertEqual(render.call_args.kwargs["output_format"], "terminal")

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

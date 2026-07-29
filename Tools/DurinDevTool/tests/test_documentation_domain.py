from __future__ import annotations
import pytest
import io
import subprocess
import sys
from pathlib import Path
from unittest import mock
REPO_ROOT = Path(__file__).resolve().parents[3]
PRODUCT_ROOT = REPO_ROOT / 'Tools' / 'DurinDevTool'
if str(PRODUCT_ROOT) not in sys.path:
    sys.path.insert(0, str(PRODUCT_ROOT))
from durin_dev_tool import cli
from durin_dev_tool.documentation import archive as archive_module
from durin_dev_tool.documentation import changes as changes_module
from durin_dev_tool.documentation import handler as handler_module
from durin_dev_tool.documentation.archive import apply_archive, preview_archive
from durin_dev_tool.documentation.model import DocumentKind, DocumentRef
from durin_dev_tool.documentation.plans import PlanStatus, load_catalog, parse_plan, render_listing
from durin_dev_tool.documentation.service import DocumentWorkspace, ListDocumentsRequest
from durin_dev_tool.documentation.tasks import load_task_catalog, parse_task
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry
PLAN_TEMPLATE = '# {title} Plan\n\nSummary: {summary}\n\nLast reviewed: 2026-07-27\n\nStatus: {status}\nCompleted:{completed}\n\n## Current Status\n'
TASK_TEMPLATE = '''# {title}

## Outcome

{outcome}

## Evidence

Concrete evidence.

## Required Changes

- Required change.

## Protected Invariants

- Protected invariant.

## Likely Working Set

- `Source/File.cpp`

## Acceptance

- Acceptance gate.
'''


class TestTaskCatalog:

    def test_catalog_extracts_outcome_and_sorts_tasks(self, tmp_path: Path) -> None:
        tasks = tmp_path / 'Documentation' / 'Tasks'
        tasks.mkdir(parents=True)
        (tasks / 'Second.md').write_text(
            TASK_TEMPLATE.format(title='Second Task', outcome='Second outcome.'),
            encoding='utf-8',
        )
        (tasks / 'First.md').write_text(
            TASK_TEMPLATE.format(
                title='First Task',
                outcome='First outcome wraps\nacross lines.\n\nMore detail.',
            ),
            encoding='utf-8',
        )
        catalog = load_task_catalog(tasks)
        assert catalog.diagnostics == ()
        assert [task.title for task in catalog.tasks] == [
            'First Task',
            'Second Task',
        ]
        assert catalog.tasks[0].summary == 'First outcome wraps across lines.'

    def test_parser_reports_missing_empty_and_lifecycle_sections(
        self,
        tmp_path: Path,
    ) -> None:
        tasks = tmp_path / 'Documentation' / 'Tasks'
        tasks.mkdir(parents=True)
        task = tasks / 'Broken.md'
        task.write_text(
            '# Broken\n\n## Outcome\n\n<!-- empty -->\n\n'
            '## Current Status\n\nStatus: Active\n',
            encoding='utf-8',
        )
        _, diagnostics = parse_task(task, tasks_directory=tasks)
        codes = {diagnostic.code for diagnostic in diagnostics}
        assert 'doc.task.section_empty' in codes
        assert 'doc.task.section_missing' in codes
        assert 'doc.task.lifecycle_metadata' in codes
        assert 'doc.task.lifecycle_section' in codes

    def test_catalog_rejects_nested_tasks_and_duplicate_titles(
        self,
        tmp_path: Path,
    ) -> None:
        tasks = tmp_path / 'Documentation' / 'Tasks'
        nested = tasks / 'Nested'
        nested.mkdir(parents=True)
        (tasks / 'One.md').write_text(
            TASK_TEMPLATE.format(title='Shared Task', outcome='One.'),
            encoding='utf-8',
        )
        (nested / 'Two.md').write_text(
            TASK_TEMPLATE.format(title='shared task', outcome='Two.'),
            encoding='utf-8',
        )
        codes = {
            diagnostic.code
            for diagnostic in load_task_catalog(tasks).diagnostics
        }
        assert 'doc.task.layout_invalid' in codes
        assert 'doc.task.title_duplicate' in codes

    def test_catalog_rejects_a_separate_task_index(self, tmp_path: Path) -> None:
        tasks = tmp_path / 'Documentation' / 'Tasks'
        tasks.mkdir(parents=True)
        (tasks / 'README.md').write_text('# Open Tasks\n', encoding='utf-8')
        codes = {
            diagnostic.code
            for diagnostic in load_task_catalog(tasks).diagnostics
        }
        assert 'doc.task.index_unsupported' in codes

class TestPlanCatalog:

    def test_legacy_plan_defaults_to_active(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        temporary = tmp_path_factory.mktemp('case')
        path = Path(temporary) / 'Legacy.md'
        path.write_text('# Legacy Plan\n\nSummary: Legacy plan.\n\nLast reviewed: 2026-07-27\n\n## Current Status\n', encoding='utf-8')
        plan, errors = parse_plan(path)
        assert errors == []
        assert plan is not None
        assert plan.status == PlanStatus.ACTIVE

    def test_catalog_rejects_duplicate_title_across_active_and_archive(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        temporary = tmp_path_factory.mktemp('case')
        plans = Path(temporary) / 'Plans'
        archive = plans / 'Archive' / '2026-07'
        archive.mkdir(parents=True)
        (plans / 'Current.md').write_text(PLAN_TEMPLATE.format(title='Shared', summary='Current.', status='Active', completed=''), encoding='utf-8')
        (archive / 'Old.md').write_text(PLAN_TEMPLATE.format(title='Shared', summary='Old.', status='Archived', completed=' 2026-07-01'), encoding='utf-8')
        catalog = load_catalog(plans)
        assert any(('duplicate title' in error for error in catalog.errors))

class TestArchive:

    @pytest.fixture(autouse=True)
    def _setup_repository(self, tmp_path: Path) -> None:
        self.repository = tmp_path
        self.plans = self.repository / 'Documentation' / 'Plans'
        self.plans.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(self.repository)], check=True)
        self.plan = self.plans / 'Feature.md'
        self.plan.write_text(PLAN_TEMPLATE.format(title='Feature', summary='Completed feature.', status='Completed', completed=' 2026-07-20'), encoding='utf-8')
        self.reference = self.repository / 'README.md'
        self.reference.write_text('[Feature](Documentation/Plans/Feature.md)\n', encoding='utf-8')

    def test_preview_does_not_modify_files(self) -> None:
        preview = preview_archive(self.plans, '2026-07')
        assert len(preview.moves) == 1
        assert self.plan.exists()
        assert self.reference.resolve() in preview.reference_files

    def test_apply_moves_plan_updates_links_and_validates(self) -> None:
        apply_archive(self.plans, '2026-07')
        archived = self.plans / 'Archive' / '2026-07' / 'Feature.md'
        assert not self.plan.exists()
        assert 'Status: Archived' in archived.read_text(encoding='utf-8')
        assert self.reference.read_text(encoding='utf-8') == '[Feature](Documentation/Plans/Archive/2026-07/Feature.md)\n'
        assert load_catalog(self.plans).errors == ()

    def test_apply_rolls_back_every_file_when_a_write_fails(self) -> None:
        original_write = changes_module.atomic_write
        calls = 0

        def fail_second_write(path: Path, content: bytes) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError('simulated write failure')
            original_write(path, content)
        with mock.patch.object(changes_module, 'atomic_write', side_effect=fail_second_write):
            with pytest.raises(OSError, match='simulated write failure'):
                apply_archive(self.plans, '2026-07')
        assert self.plan.exists()
        assert self.reference.read_text(encoding='utf-8') == '[Feature](Documentation/Plans/Feature.md)\n'
        assert not (self.plans / 'Archive' / '2026-07' / 'Feature.md').exists()

    def test_apply_rejects_stale_inputs_before_writing(self) -> None:
        original_apply = archive_module.apply_change_set

        def edit_then_apply(change_set, *, validator):
            self.reference.write_text(
                '[Feature](Documentation/Plans/Feature.md)\n\nUser edit.\n',
                encoding='utf-8',
            )
            return original_apply(change_set, validator=validator)

        with mock.patch.object(
            archive_module,
            'apply_change_set',
            side_effect=edit_then_apply,
        ):
            with pytest.raises(DevToolError, match='modified after preview'):
                apply_archive(self.plans, '2026-07')
        assert self.plan.exists()
        assert self.reference.read_text(encoding='utf-8').endswith('User edit.\n')
        assert not (self.plans / 'Archive' / '2026-07' / 'Feature.md').exists()

    def test_apply_rolls_back_after_post_validation_failure(self) -> None:
        with mock.patch.object(
            archive_module,
            '_validate_archive',
            side_effect=archive_module.ArchiveError(
                'simulated validation failure'
            ),
        ):
            with pytest.raises(
                archive_module.ArchiveError,
                match='simulated validation failure',
            ):
                apply_archive(self.plans, '2026-07')
        assert self.plan.exists()
        assert self.reference.read_text(encoding='utf-8') == '[Feature](Documentation/Plans/Feature.md)\n'
        assert not (self.plans / 'Archive' / '2026-07' / 'Feature.md').exists()

class TestUnifiedCommand:

    @pytest.fixture(autouse=True)
    def _setup_repository(self, tmp_path: Path) -> None:
        self.repository = tmp_path
        self.plans = self.repository / 'Documentation' / 'Plans'
        self.plans.mkdir(parents=True)
        (self.plans / 'Active.md').write_text(PLAN_TEMPLATE.format(title='Active', summary='Active plan.', status='Active', completed=''), encoding='utf-8')
        self.registry = CommandRegistry()

    def _parse_values(self, arguments: list[str]) -> dict[str, object]:
        _, namespace = self.registry.parse(arguments)
        return {key: value for key, value in vars(namespace).items() if key != '_command_spec'}

    def test_every_plan_command_has_one_direct_and_shell_request_model(self) -> None:
        commands = (['doc', 'plan', 'list', '--query', 'Active', '--format', 'markdown'], ['doc', 'plan', 'validate', '--scope', 'active'], ['doc', 'plan', 'archive', '2026-07'])
        for command in commands:
            direct = self._parse_values(command)
            shell = self._parse_values(command)
            assert direct == shell

    def test_plan_names_are_case_insensitive_and_accept_slash_aliases(self) -> None:
        expected = self._parse_values(['doc', 'plan', 'list'])
        assert expected == self._parse_values(['DOC', 'PLAN', 'LIST'])
        assert expected == self._parse_values(['/doc', '/plan', '/list'])

    def test_task_commands_register_expected_requests(self) -> None:
        expected = self._parse_values(['doc', 'task', 'list'])
        assert expected == self._parse_values(['doc', 'task'])
        assert expected == self._parse_values(['/doc', '/task', '/list'])
        filtered = self._parse_values(
            ['doc', 'task', 'list', '--query', 'tool', '--format', 'json']
        )
        assert filtered['task_action'] == 'list'
        assert filtered['task_query'] == 'tool'
        assert filtered['output_format'] == 'json'
        validate = self._parse_values(
            ['doc', 'task', 'validate', '--format', 'markdown']
        )
        assert validate['task_action'] == 'validate'
        remove = self._parse_values(
            ['doc', 'task', 'remove', 'Documentation/Tasks/Tool.md']
        )
        assert remove['task_action'] == 'remove'
        assert remove['task_path'] == 'Documentation/Tasks/Tool.md'

    def test_list_defaults_to_markdown_direct_and_terminal_in_shell(self) -> None:
        direct_spec, direct = self.registry.parse(['doc', 'plan', 'list'])
        shell_spec, shell = self.registry.parse(['doc', 'plan', 'list'])
        with mock.patch.object(handler_module, 'render_listing', return_value='result') as render:
            self.registry.execute(direct_spec, direct, repository_root=self.repository, stdout=io.StringIO(), stderr=io.StringIO())
            self.registry.execute(shell_spec, shell, repository_root=self.repository, stdout=io.StringIO(), stderr=io.StringIO(), session_state={})
        assert [call.kwargs['output_format'] for call in render.call_args_list] == ['markdown', 'terminal']

    def test_explicit_format_has_direct_and_shell_output_parity(self) -> None:
        arguments = ['doc', 'plan', 'list', '--format', 'markdown', '--color', 'never']
        outputs: list[str] = []
        for session_state in (None, {}):
            spec, namespace = self.registry.parse(arguments)
            stdout = io.StringIO()
            keywords = {}
            if session_state is not None:
                keywords['session_state'] = session_state
            result = self.registry.execute(spec, namespace, repository_root=self.repository, stdout=stdout, stderr=io.StringIO(), **keywords)
            assert result == 0
            outputs.append(stdout.getvalue())
        assert outputs[0] == outputs[1]

    def test_validate_and_archive_defaults_have_direct_and_shell_output_parity(self) -> None:
        for arguments in (['doc', 'plan', 'validate', '--scope', 'active'], ['doc', 'plan', 'archive', '2099-01']):
            outputs: list[str] = []
            for session_state in (None, {}):
                spec, namespace = self.registry.parse(arguments)
                stdout = io.StringIO()
                keywords = {}
                if session_state is not None:
                    keywords['session_state'] = session_state
                result = self.registry.execute(spec, namespace, repository_root=self.repository, stdout=stdout, stderr=io.StringIO(), **keywords)
                assert result == 0
                outputs.append(stdout.getvalue())
            assert outputs[0] == outputs[1]

    def test_unfiltered_archive_and_all_listings_require_all_results(self) -> None:
        for scope in ('archive', 'all'):
            with pytest.raises(DevToolError, match='archive listings require'):
                cli.run(['doc', 'plan', 'list', '--scope', scope], repository_root=self.repository, stdout=io.StringIO(), stderr=io.StringIO())

    def test_archive_defaults_to_preview(self) -> None:
        spec, namespace = self.registry.parse(['doc', 'plan', 'archive', '2026-07'])
        with mock.patch.object(handler_module, 'preview_archive') as preview:
            preview.return_value = mock.Mock(month='2026-07', moves=())
            result = self.registry.execute(spec, namespace, repository_root=self.repository, stdout=io.StringIO(), stderr=io.StringIO())
        assert result == 0
        preview.assert_called_once_with(self.plans, '2026-07')

    def test_archive_apply_is_explicit(self) -> None:
        spec, namespace = self.registry.parse(['doc', 'plan', 'archive', '2026-07', '--apply'])
        with mock.patch.object(handler_module, 'apply_archive') as apply:
            apply.return_value = mock.Mock(month='2026-07', moves=())
            result = self.registry.execute(spec, namespace, repository_root=self.repository, stdout=io.StringIO(), stderr=io.StringIO())
        assert result == 0
        apply.assert_called_once_with(self.plans, '2026-07')

    def test_archive_listing_groups_plans_by_month(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        temporary = tmp_path_factory.mktemp('case')
        plans = Path(temporary) / 'Plans'
        for month, title in (('2026-06', 'Older'), ('2026-07', 'Newer')):
            directory = plans / 'Archive' / month
            directory.mkdir(parents=True)
            (directory / f'{title}.md').write_text(PLAN_TEMPLATE.format(title=title, summary=f'{title} plan.', status='Archived', completed=f' {month}-01'), encoding='utf-8')
        catalog = load_catalog(plans)
        output = render_listing(catalog.archived, plans, scope='archive', output_format='markdown', color='never')
        assert output.index('## 2026-07') < output.index('## 2026-06')


class TestTaskCommands:

    @pytest.fixture(autouse=True)
    def _setup_repository(self, tmp_path: Path) -> None:
        self.repository = tmp_path
        self.tasks = self.repository / 'Documentation' / 'Tasks'
        self.tasks.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(self.repository)], check=True)
        self.task = self.tasks / 'Tool.md'
        self.task.write_text(
            TASK_TEMPLATE.format(
                title='Improve Tool',
                outcome='Make the tool behavior explicit.',
            ),
            encoding='utf-8',
        )
        self.workspace = DocumentWorkspace(self.repository)

    def test_list_is_compact_and_supports_json_query(self) -> None:
        output = io.StringIO()
        assert cli.run(
            ['doc', 'task', 'list', '--query', 'explicit', '--format', 'json'],
            repository_root=self.repository,
            stdout=output,
            stderr=io.StringIO(),
        ) == 0
        rendered = output.getvalue()
        assert '"title": "Improve Tool"' in rendered
        assert '"summary": "Make the tool behavior explicit."' in rendered
        assert 'Required change' not in rendered

    def test_remove_defaults_to_preview_and_apply_deletes_only_the_task(
        self,
    ) -> None:
        arguments = [
            'doc',
            'task',
            'remove',
            'Documentation/Tasks/Tool.md',
        ]
        assert cli.run(
            arguments,
            repository_root=self.repository,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
        ) == 0
        assert self.task.exists()
        assert cli.run(
            [*arguments, '--apply'],
            repository_root=self.repository,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
        ) == 0
        assert not self.task.exists()

    def test_remove_rejects_external_document_references(self) -> None:
        reference = self.repository / 'README.md'
        reference.write_text(
            '# Repository\n\n[Task](Documentation/Tasks/Tool.md)\n',
            encoding='utf-8',
        )
        with pytest.raises(DevToolError, match='still referenced'):
            self.workspace.prepare_task_remove(
                task=DocumentRef.parse('Documentation/Tasks/Tool.md')
            )
        assert self.task.exists()

    def test_remove_ignores_unrelated_deleted_tracked_markdown(self) -> None:
        obsolete = self.repository / 'Obsolete.md'
        obsolete.write_text('# Obsolete\n', encoding='utf-8')
        subprocess.run(
            ['git', 'add', 'Obsolete.md'],
            cwd=self.repository,
            check=True,
        )
        obsolete.unlink()
        change_set = self.workspace.prepare_task_remove(
            task=DocumentRef.parse('Documentation/Tasks/Tool.md')
        )
        assert change_set.deletions[0].path == self.task

    def test_remove_is_fingerprint_checked_and_rolls_back_validation_failure(
        self,
    ) -> None:
        ref = DocumentRef.parse('Documentation/Tasks/Tool.md')
        stale = self.workspace.prepare_task_remove(task=ref)
        self.task.write_text(
            self.task.read_text(encoding='utf-8') + '\nUser edit.\n',
            encoding='utf-8',
        )
        with pytest.raises(DevToolError, match='modified after preview'):
            self.workspace.apply(stale)
        assert self.task.exists()

        change_set = self.workspace.prepare_task_remove(task=ref)

        def reject_final_state() -> None:
            raise OSError('simulated validation failure')

        with pytest.raises(OSError, match='simulated validation failure'):
            changes_module.apply_change_set(
                change_set,
                validator=reject_final_state,
            )
        assert self.task.exists()

    def test_changed_validation_checks_cross_task_uniqueness(self) -> None:
        second = self.tasks / 'Other.md'
        second.write_text(
            TASK_TEMPLATE.format(
                title='Other Task',
                outcome='Keep another task.',
            ),
            encoding='utf-8',
        )
        subprocess.run(
            ['git', 'add', 'Documentation'],
            cwd=self.repository,
            check=True,
        )
        subprocess.run(
            [
                'git',
                '-c',
                'user.name=Test',
                '-c',
                'user.email=test@example.invalid',
                'commit',
                '-qm',
                'baseline',
            ],
            cwd=self.repository,
            check=True,
        )
        second.write_text(
            TASK_TEMPLATE.format(
                title='Improve Tool',
                outcome='Now duplicates another title.',
            ),
            encoding='utf-8',
        )
        output = io.StringIO()
        assert cli.run(
            ['doc', 'validate', '--scope', 'changed', '--format', 'json'],
            repository_root=self.repository,
            stdout=output,
            stderr=io.StringIO(),
        ) == 1
        assert '"code": "doc.task.title_duplicate"' in output.getvalue()


class TestOrdinaryDocumentation:

    @pytest.fixture(autouse=True)
    def _setup_repository(self, tmp_path: Path) -> None:
        self.repository = tmp_path
        self.documentation = self.repository / 'Documentation'
        runtime = self.documentation / 'Runtime'
        runtime.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(self.repository)], check=True)
        self.index = runtime / 'README.md'
        self.topic = runtime / 'Topic.md'
        self.index.write_text('# Runtime\n\n[Topic](Topic.md)\n', encoding='utf-8')
        self.topic.write_text('# Topic\n\n[Root](README.md)\n', encoding='utf-8')
        self.workspace = DocumentWorkspace(self.repository)

    def test_catalog_classifies_and_filters_documents(self) -> None:
        documents = self.workspace.list_documents(
            ListDocumentsRequest(kinds=(DocumentKind.CONTRACT,))
        )
        assert [document.ref.as_posix() for document in documents] == [
            'Documentation/Runtime/Topic.md'
        ]
        routers = self.workspace.list_documents(
            ListDocumentsRequest(
                kinds=(DocumentKind.ROUTER,),
                query='runtime',
            )
        )
        assert [document.kind for document in routers] == [DocumentKind.ROUTER]

    def test_refs_reports_inbound_and_outbound_documents(self) -> None:
        references = self.workspace.references(
            DocumentRef.parse('Documentation/Runtime/Topic.md')
        )
        assert [
            document.ref.as_posix() for document, _ in references.inbound
        ] == ['Documentation/Runtime/README.md']
        assert references.outbound[0][0] == DocumentRef.parse(
            'Documentation/Runtime/README.md'
        )

    def test_validate_reports_structured_missing_link(self) -> None:
        self.topic.write_text('# Topic\n\n[Missing](Missing.md)\n', encoding='utf-8')
        output = io.StringIO()
        result = cli.run(
            ['doc', 'validate', '--format', 'json'],
            repository_root=self.repository,
            stdout=output,
            stderr=io.StringIO(),
        )
        assert result == 1
        assert '"code": "doc.link.missing"' in output.getvalue()
        assert '"line": 3' in output.getvalue()

    def test_create_is_preview_only_until_apply(self) -> None:
        path = self.documentation / 'Runtime' / 'Created.md'
        arguments = [
            'doc',
            'create',
            'contract',
            'Documentation/Runtime/Created.md',
            '--title',
            'Created',
            '--summary',
            'Created summary.',
        ]
        assert cli.run(
            arguments,
            repository_root=self.repository,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
        ) == 0
        assert not path.exists()
        assert cli.run(
            [*arguments, '--apply'],
            repository_root=self.repository,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
        ) == 0
        assert path.read_text(encoding='utf-8') == (
            '# Created\n\nCreated summary.\n'
        )

    def test_move_repairs_links_and_is_fingerprint_checked(self) -> None:
        source = DocumentRef.parse('Documentation/Runtime/Topic.md')
        destination = DocumentRef.parse('Documentation/Runtime/Nested/Topic.md')
        change_set = self.workspace.prepare_move(
            source=source,
            destination=destination,
        )
        assert self.index.read_text(encoding='utf-8') == (
            '# Runtime\n\n[Topic](Topic.md)\n'
        )
        self.workspace.apply(change_set)
        moved = self.documentation / 'Runtime' / 'Nested' / 'Topic.md'
        assert moved.exists()
        assert '[Topic](Nested/Topic.md)' in self.index.read_text(encoding='utf-8')
        assert '[Root](../README.md)' in moved.read_text(encoding='utf-8')

        second = self.workspace.prepare_move(
            source=destination,
            destination=DocumentRef.parse(
                'Documentation/Runtime/Nested/Renamed.md'
            ),
        )
        moved.write_text('# User edit\n', encoding='utf-8')
        with pytest.raises(DevToolError, match='modified after preview'):
            self.workspace.apply(second)

    def test_move_rolls_back_all_files_when_a_write_fails(self) -> None:
        change_set = self.workspace.prepare_move(
            source=DocumentRef.parse('Documentation/Runtime/Topic.md'),
            destination=DocumentRef.parse(
                'Documentation/Runtime/Nested/Topic.md'
            ),
        )
        original_write = changes_module.atomic_write
        calls = 0

        def fail_second_write(path: Path, content: bytes) -> None:
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError('simulated write failure')
            original_write(path, content)

        with mock.patch.object(
            changes_module,
            'atomic_write',
            side_effect=fail_second_write,
        ):
            with pytest.raises(OSError, match='simulated write failure'):
                self.workspace.apply(change_set)
        assert self.topic.exists()
        assert not (
            self.documentation / 'Runtime' / 'Nested' / 'Topic.md'
        ).exists()
        assert self.index.read_text(encoding='utf-8') == (
            '# Runtime\n\n[Topic](Topic.md)\n'
        )

    def test_create_rejects_specialized_document_directories(self) -> None:
        with pytest.raises(DevToolError, match='plan workflow'):
            self.workspace.prepare_create(
                destination=DocumentRef.parse(
                    'Documentation/Plans/NotAPlan.md'
                ),
                kind=DocumentKind.CONTRACT,
                title='Not A Plan',
                summary='Invalid placement.',
            )
        with pytest.raises(DevToolError, match='task workflow'):
            self.workspace.prepare_create(
                destination=DocumentRef.parse(
                    'Documentation/Tasks/NotATask.md'
                ),
                kind=DocumentKind.CONTRACT,
                title='Not A Task',
                summary='Invalid placement.',
            )

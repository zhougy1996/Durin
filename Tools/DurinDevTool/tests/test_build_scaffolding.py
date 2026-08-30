import pytest
import io
import json
from pathlib import Path
from . import build_request_fixtures as request_fixtures
REPO_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.build import operations as build_cli
from durin_dev_tool.build import errors
from durin_dev_tool.build import descriptors as build_descriptors
from durin_dev_tool.build import scaffolding as build_scaffolding
from durin_dev_tool.build.output import BuildOutput

parse_build_request = request_fixtures.parse_build_request

class TestScaffoldingInfrastructure:

    @staticmethod
    def write_project(path: Path, name: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({'ProjectName': name, 'ModuleDirs': {}, 'BaseModules': []}, indent=4) + '\n', encoding='utf-8')

    @classmethod
    def create_discovery_workspace(cls, root: Path) -> None:
        cls.write_project(root / 'Engine' / 'Engine.dproject', 'Engine')
        cls.write_project(root / 'Sandbox' / 'Sandbox.dproject', 'Sandbox')
        (root / 'CMakeLists.txt').write_text('add_subdirectory(Engine)\nadd_subdirectory("Sandbox")\n', encoding='utf-8')
        module_dir = root / 'Engine' / 'Source' / 'Runtime' / 'Core'
        module_dir.mkdir(parents=True)
        (module_dir / 'CMakeLists.txt').write_text('add_durin_module(Core)\n', encoding='utf-8')

    @staticmethod
    def snapshot(root: Path) -> tuple[tuple[str, ...], dict[str, bytes]]:
        directories = tuple(sorted((path.relative_to(root).as_posix() for path in root.rglob('*') if path.is_dir())))
        files = {path.relative_to(root).as_posix(): path.read_bytes() for path in sorted(root.rglob('*')) if path.is_file()}
        return (directories, files)

    @staticmethod
    def transaction_plan(root: Path) -> build_scaffolding.ScaffoldPlan:
        generated = root / 'Generated'
        descriptor = generated / 'Gameplay.dmodule'
        root_cmake = root / 'CMakeLists.txt'
        return build_scaffolding.ordered_plan(root, (root,), directories=(generated,), files=((descriptor, b'{\n    "ModuleName": "Gameplay"\n}\n'),), replacements=((root_cmake, root_cmake.read_bytes() + b'add_subdirectory(Generated)\n'),))

    def test_workspace_discovery_cross_checks_root_cmake_and_targets(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_discovery_workspace(root)
        discovery = build_scaffolding.discover_workspace_projects(root)
        assert tuple((project.descriptor.name for project in discovery.projects)) == ('Engine', 'Sandbox')
        assert discovery.projects[1].cmake_registration == 'Sandbox'
        assert 'Core' in discovery.cmake_targets
        with pytest.raises(errors.BuildToolError, match='CMake target'):
            build_scaffolding.require_available_cmake_target('core', discovery)
        (root / 'CMakeLists.txt').write_text('add_subdirectory(Engine)\n', encoding='utf-8')
        with pytest.raises(errors.BuildToolError, match='Sandbox.*exactly one'):
            build_scaffolding.discover_workspace_projects(root)

    def test_destination_checks_cover_containment_overlap_existing_and_case(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_discovery_workspace(root)
        discovery = build_scaffolding.discover_workspace_projects(root)
        with pytest.raises(errors.BuildToolError, match='inside'):
            build_scaffolding.validate_destination(root.parent / 'Outside', discovery, label='Project destination')
        with pytest.raises(errors.BuildToolError, match='overlaps project'):
            build_scaffolding.validate_destination(root / 'Engine' / 'Nested', discovery, label='Project destination')
        existing = root / 'Existing'
        existing.mkdir()
        with pytest.raises(errors.BuildToolError, match='already exists'):
            build_scaffolding.validate_destination(existing, discovery, label='Project destination')
        case_path = root / 'MixedCase'
        case_path.mkdir()
        with pytest.raises(errors.BuildToolError, match='case-insensitive'):
            build_scaffolding.validate_destination(root / 'mixedcase', discovery, label='Project destination')

    def test_templates_are_disk_assets_with_explicit_deterministic_variables(self) -> None:
        renderer = build_scaffolding.TemplateRenderer()
        module_variables = {'MODULE_NAME': 'Gameplay', 'LINK_TYPE': 'Shared', 'PCH': 'Self', 'PRIVATE_DEPENDENCIES': '["Core"]', 'PUBLIC_DEPENDENCIES': '[]', 'OPTIONAL_PRIVATE_DEPENDENCIES': '[]', 'OPTIONAL_PUBLIC_DEPENDENCIES': '[]'}
        first = renderer.render('module/descriptor.json.template', module_variables)
        second = renderer.render('module/descriptor.json.template', module_variables)
        assert first == second
        assert json.loads(first)['ModuleName'] == 'Gameplay'
        rendered_templates = {'module/entry_point.cpp.template': renderer.render('module/entry_point.cpp.template', {'MODULE_NAME': 'Gameplay'}), 'module/api.h.template': renderer.render('module/api.h.template', {'MODULE_NAME_UPPER': 'GAMEPLAY'}), 'module/CMakeLists.txt.template': renderer.render('module/CMakeLists.txt.template', {'MODULE_NAME': 'Gameplay'}), 'module/pch.h.template': renderer.render('module/pch.h.template', {}), 'project/descriptor.json.template': renderer.render('project/descriptor.json.template', {'PROJECT_NAME': 'MyGame'}), 'project/CMakeLists.txt.template': renderer.render('project/CMakeLists.txt.template', {'PROJECT_NAME': 'MyGame'}), 'project/setup.cmake.template': renderer.render('project/setup.cmake.template', {'PROJECT_NAME': 'MyGame'})}
        assert json.loads(rendered_templates['project/descriptor.json.template'])['ProjectName'] == 'MyGame'
        for content in rendered_templates.values():
            assert b'{{' not in content
            assert str(REPO_ROOT).encode() not in content
        assert (renderer.template_root / 'module' / 'descriptor.json.template').is_file()
        with pytest.raises(errors.BuildToolError, match='missing MODULE_NAME_UPPER'):
            renderer.render('module/api.h.template', {'MODULE_NAME': 'Gameplay'})
        with pytest.raises(errors.BuildToolError, match='unknown EXTRA'):
            renderer.render('module/CMakeLists.txt.template', {'MODULE_NAME': 'Gameplay', 'EXTRA': 'value'})

    def test_template_renderers_keep_explicit_roots_isolated(self, tmp_path: Path) -> None:
        first_root = tmp_path / 'first'
        second_root = tmp_path / 'second'
        first_root.mkdir()
        second_root.mkdir()
        (first_root / 'value.template').write_text('first {{VALUE}}', encoding='utf-8')
        (second_root / 'value.template').write_text('second {{VALUE}}', encoding='utf-8')

        first = build_scaffolding.TemplateRenderer(first_root)
        second = build_scaffolding.TemplateRenderer(second_root)

        assert first.render('value.template', {'VALUE': 'root'}) == b'first root'
        assert second.render('value.template', {'VALUE': 'root'}) == b'second root'
        assert not hasattr(build_scaffolding, 'TEMPLATE_DIR')

    def test_dry_run_format_is_stable_and_does_not_mutate(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        (root / 'CMakeLists.txt').write_bytes(b'add_subdirectory(Engine)\r\n')
        before = self.snapshot(root)
        plan = self.transaction_plan(root)
        plain = plan.format(plain=True)
        styled = plan.format(plain=False)
        assert before == self.snapshot(root)
        assert plain == '\n'.join(('Scaffolding plan (3 operations)', '  create directory: Generated', '  create file: Generated/Gameplay.dmodule', '  replace file: CMakeLists.txt'))
        assert styled.replace('[cyan]', '').replace('[/cyan]', '') == plain

    def test_transaction_success_preserves_unrelated_bytes_and_reparses_outputs(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        (root / 'CMakeLists.txt').write_bytes(b'add_subdirectory(Engine)\r\n')
        unrelated = root / 'Unrelated.bin'
        unrelated.write_bytes(b'\x00unchanged\r\n')
        plan = self.transaction_plan(root)
        build_scaffolding.execute_plan(plan)
        assert unrelated.read_bytes() == b'\x00unchanged\r\n'
        assert (root / 'Generated' / 'Gameplay.dmodule').read_bytes() == b'{\n    "ModuleName": "Gameplay"\n}\n'
        assert (root / 'CMakeLists.txt').read_bytes() == b'add_subdirectory(Engine)\r\nadd_subdirectory(Generated)\n'
        assert not any(('.backup.' in path.name or '.write.' in path.name for path in root.rglob('*')))

    def test_every_injected_write_failure_rolls_back_exactly(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        (root / 'CMakeLists.txt').write_bytes(b'add_subdirectory(Engine)\r\n')
        boundaries: list[tuple[str, int, Path]] = []
        build_scaffolding.execute_plan(self.transaction_plan(root), failure_injector=lambda phase, index, path: boundaries.append((phase, index, path)))
        assert len(boundaries) > 0
        for failing_boundary in range(1, len(boundaries) + 1):
            directory = tmp_path_factory.mktemp('case')
            root = Path(directory)
            (root / 'CMakeLists.txt').write_bytes(b'add_subdirectory(Engine)\r\n')
            unrelated = root / 'Unrelated.bin'
            unrelated.write_bytes(b'\xffkeep')
            before = self.snapshot(root)

            def fail_at_boundary(phase: str, index: int, path: Path) -> None:
                if index == failing_boundary:
                    raise RuntimeError(f'injected at {phase}: {path}')
            with pytest.raises(RuntimeError, match='injected'):
                build_scaffolding.execute_plan(self.transaction_plan(root), failure_injector=fail_at_boundary)
            assert self.snapshot(root) == before

    def test_validation_failure_rolls_back_and_plan_rejects_outside_roots(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        (root / 'CMakeLists.txt').write_bytes(b'add_subdirectory(Engine)\n')
        before = self.snapshot(root)
        with pytest.raises(errors.BuildToolError, match='unbalanced'):
            build_scaffolding.ordered_plan(root, (root,), replacements=((root / 'CMakeLists.txt', b'add_subdirectory(Engine\n'),))
        assert self.snapshot(root) == before

        def reject_final_state(plan: build_scaffolding.ScaffoldPlan) -> None:
            raise errors.BuildToolError('injected descriptor validation failure')
        validation_plan = build_scaffolding.ordered_plan(root, (root,), directories=(root / 'Generated',), files=((root / 'Generated' / 'Gameplay.dmodule', b'{\n    "ModuleName": "Gameplay"\n}\n'),), validators=(reject_final_state,))
        with pytest.raises(errors.BuildToolError, match='validation failure'):
            build_scaffolding.execute_plan(validation_plan)
        assert self.snapshot(root) == before
        with pytest.raises(errors.BuildToolError, match='outside'):
            build_scaffolding.ordered_plan(root, (root,), files=((root.parent / 'outside.txt', b'no'),))

class TestModuleScaffolding:

    @staticmethod
    def write_json(path: Path, value: object) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

    @classmethod
    def create_workspace(cls, root: Path) -> tuple[Path, Path]:
        (root / 'CMakeLists.txt').write_text('add_subdirectory(Engine)\nadd_subdirectory(Sandbox)\n', encoding='utf-8')
        engine = root / 'Engine'
        sandbox = root / 'Sandbox'
        engine_project = engine / 'Engine.dproject'
        sandbox_project = sandbox / 'Sandbox.dproject'
        cls.write_json(engine_project, {'ProjectName': 'Engine', 'ModuleDirs': {'Core': 'Source/Runtime/Core', 'AssetCore': 'Source/Runtime/AssetCore', 'DurinEd': 'Source/Editor/DurinEd'}, 'BaseModules': ['Core'], 'ExtraModules': {'DurinEditor': {'Modules': ['DurinEd']}, 'DurinGame': {'Modules': []}}})
        cls.write_json(engine / 'Source' / 'Runtime' / 'Core' / 'Core.dmodule', {'ModuleName': 'Core'})
        cls.write_json(engine / 'Source' / 'Runtime' / 'AssetCore' / 'AssetCore.dmodule', {'ModuleName': 'AssetCore', 'PrivateDependencies': ['Core']})
        cls.write_json(engine / 'Source' / 'Editor' / 'DurinEd' / 'DurinEd.dmodule', {'ModuleName': 'DurinEd', 'PrivateDependencies': ['Core']})
        cls.write_json(sandbox_project, {'ProjectName': 'Sandbox', 'ModuleDirs': {'Sandbox': 'Source/Runtime/Sandbox'}, 'BaseModules': ['Sandbox'], 'ExtraModules': {'DurinEditor': {'Modules': []}, 'DurinGame': {'Modules': []}}})
        cls.write_json(sandbox / 'Source' / 'Runtime' / 'Sandbox' / 'Sandbox.dmodule', {'ModuleName': 'Sandbox', 'PrivateDependencies': ['Core']})
        for module_name, module_root in (('Core', engine / 'Source' / 'Runtime' / 'Core'), ('AssetCore', engine / 'Source' / 'Runtime' / 'AssetCore'), ('DurinEd', engine / 'Source' / 'Editor' / 'DurinEd'), ('Sandbox', sandbox / 'Source' / 'Runtime' / 'Sandbox')):
            (module_root / 'CMakeLists.txt').write_text(f'add_durin_module({module_name})\n', encoding='utf-8')
        (sandbox / 'Source' / 'Editor').mkdir(parents=True)
        return (engine_project, sandbox_project)

    @staticmethod
    def snapshot(root: Path) -> dict[str, bytes]:
        return {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob('*') if path.is_file()}

    def test_runtime_defaults_generate_complete_module_and_base_registration(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        _, sandbox_project = self.create_workspace(root)
        request = parse_build_request(['create', 'module', 'Gameplay', '--project', 'Sandbox/Sandbox.dproject', '--private-dependency', 'Core'])
        plan = build_scaffolding.plan_module_creation(request, root)
        assert len(plan.operations) == 9
        build_scaffolding.execute_plan(plan)
        module_root = root / 'Sandbox' / 'Source' / 'Runtime' / 'Gameplay'
        descriptor = json.loads((module_root / 'Gameplay.dmodule').read_text(encoding='utf-8'))
        project = json.loads(sandbox_project.read_text(encoding='utf-8'))
        assert descriptor['LinkType'] == 'Shared'
        assert descriptor['PCH'] == 'Self'
        assert descriptor['PrivateDependencies'] == ['Core']
        assert project['ModuleDirs']['Gameplay'] == 'Source/Runtime/Gameplay'
        assert project['BaseModules'] == ['Sandbox', 'Gameplay']
        assert (module_root / 'Private' / 'GameplayModule.cpp').is_file()
        assert (module_root / 'Private' / 'PCH.Gameplay.h').is_file()
        assert (module_root / 'Public' / 'GameplayAPI.h').is_file()
        assert 'add_durin_module(Gameplay)' in (module_root / 'CMakeLists.txt').read_text(encoding='utf-8')
        assert build_descriptors.load_workspace_descriptors(root).find_module('Gameplay').owning_project == 'Sandbox'

    def test_editor_overrides_preserve_all_dependency_categories_and_profiles(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        _, sandbox_project = self.create_workspace(root)
        request = parse_build_request(['create', 'module', 'SceneEditor', '--project', 'Sandbox/Sandbox.dproject', '--kind', 'editor', '--link', 'static', '--pch', 'SharedPCH_Core', '--private-dependency', 'Core', '--public-dependency', 'Sandbox', '--optional-private-dependency', 'DurinEd', '--optional-public-dependency', 'AssetCore', '--enable', 'DurinEditor', '--enable', 'DurinGame'])
        build_scaffolding.execute_plan(build_scaffolding.plan_module_creation(request, root))
        module_root = root / 'Sandbox' / 'Source' / 'Editor' / 'SceneEditor'
        descriptor = json.loads((module_root / 'SceneEditor.dmodule').read_text(encoding='utf-8'))
        project = json.loads(sandbox_project.read_text(encoding='utf-8'))
        assert descriptor['LinkType'] == 'Static'
        assert descriptor['PCH'] == 'SharedPCH_Core'
        assert not (module_root / 'Private' / 'PCH.SceneEditor.h').exists()
        assert descriptor['PrivateDependencies'] == ['Core']
        assert descriptor['PublicDependencies'] == ['Sandbox']
        assert descriptor['OptionalPrivateDependencies'] == ['DurinEd']
        assert descriptor['OptionalPublicDependencies'] == ['AssetCore']
        assert project['ExtraModules']['DurinEditor']['Modules'] == ['SceneEditor']
        assert project['ExtraModules']['DurinGame']['Modules'] == ['SceneEditor']
        assert 'SceneEditor' not in project['BaseModules']

    def test_custom_path_is_independent_from_kind_and_default_enablement(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        _, sandbox_project = self.create_workspace(root)
        request = parse_build_request(['create', 'module', 'WorldTools', '--project', 'Sandbox/Sandbox.dproject', '--path', 'Source/Features/World Tools', '--kind', 'editor', '--private-dependency', 'Core'])
        build_scaffolding.execute_plan(build_scaffolding.plan_module_creation(request, root))
        module_root = root / 'Sandbox' / 'Source' / 'Features' / 'World Tools'
        project = json.loads(sandbox_project.read_text(encoding='utf-8'))
        assert (module_root / 'WorldTools.dmodule').is_file()
        assert project['ModuleDirs']['WorldTools'] == 'Source/Features/World Tools'
        assert project['ExtraModules']['DurinEditor']['Modules'] == ['WorldTools']
        assert 'WorldTools' not in project['BaseModules']

    def test_developer_defaults_to_developer_path_and_editor_enablement(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        _, sandbox_project = self.create_workspace(root)
        request = parse_build_request(['create', 'module', 'AssetRecipes', '--project', 'Sandbox/Sandbox.dproject', '--kind', 'developer'])
        plan = build_scaffolding.plan_module_creation(request, root)
        assert 'Source/Developer/AssetRecipes/AssetRecipes.dmodule' in plan.format(plain=True)
        build_scaffolding.execute_plan(plan)
        module_root = root / 'Sandbox' / 'Source' / 'Developer' / 'AssetRecipes'
        project = json.loads(sandbox_project.read_text(encoding='utf-8'))
        assert (module_root / 'AssetRecipes.dmodule').is_file()
        assert project['ModuleDirs']['AssetRecipes'] == 'Source/Developer/AssetRecipes'
        assert project['ExtraModules']['DurinEditor']['Modules'] == ['AssetRecipes']
        assert 'AssetRecipes' not in project['BaseModules']
        assert project['ExtraModules']['DurinGame']['Modules'] == []

    def test_developer_custom_path_and_enablement_remain_authoritative(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        _, sandbox_project = self.create_workspace(root)
        request = parse_build_request(['create', 'module', 'HeadlessRecipes', '--project', 'Sandbox/Sandbox.dproject', '--kind', 'developer', '--path', 'Source/Tools/HeadlessRecipes', '--enable', 'none'])
        build_scaffolding.execute_plan(build_scaffolding.plan_module_creation(request, root))
        project = json.loads(sandbox_project.read_text(encoding='utf-8'))
        assert project['ModuleDirs']['HeadlessRecipes'] == 'Source/Tools/HeadlessRecipes'
        assert 'HeadlessRecipes' not in project['BaseModules']
        assert project['ExtraModules']['DurinEditor']['Modules'] == []
        assert project['ExtraModules']['DurinGame']['Modules'] == []

    def test_custom_path_rejects_outside_and_existing_module_overlap(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_workspace(root)
        outside_request = parse_build_request(['create', 'module', 'Outside', '--project', 'Sandbox/Sandbox.dproject', '--path', str(root.parent / 'Outside')])
        with pytest.raises(errors.BuildToolError, match='inside'):
            build_scaffolding.plan_module_creation(outside_request, root)
        inside_absolute_request = parse_build_request(['create', 'module', 'Absolute', '--project', 'Sandbox/Sandbox.dproject', '--path', str(root / 'Sandbox' / 'Code' / 'Absolute')])
        inside_plan = build_scaffolding.plan_module_creation(inside_absolute_request, root)
        assert any((operation.path == root / 'Sandbox' / 'Code' / 'Absolute' / 'Absolute.dmodule' for operation in inside_plan.operations))
        overlap_request = parse_build_request(['create', 'module', 'Nested', '--project', 'Sandbox/Sandbox.dproject', '--path', 'Source/Runtime/Sandbox/Nested'])
        with pytest.raises(errors.BuildToolError, match='overlaps module'):
            build_scaffolding.plan_module_creation(overlap_request, root)

    def test_none_enablement_dry_run_and_conflicts_leave_workspace_unchanged(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        self.create_workspace(root)
        request = parse_build_request(['create', 'module', 'Utility', '--project', 'Sandbox/Sandbox.dproject', '--enable', 'none', '--dry-run', '--plain'])
        before = self.snapshot(root)
        stdout = io.StringIO()
        build_cli.execute_create_request(request, BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()), root=root)
        assert self.snapshot(root) == before
        assert 'Source/Runtime/Utility/Utility.dmodule' in stdout.getvalue()
        conflict = root / 'Sandbox' / 'Source' / 'Runtime' / 'Utility'
        conflict.mkdir()
        before_conflict = self.snapshot(root)
        with pytest.raises(errors.BuildToolError, match='already exists'):
            build_scaffolding.plan_module_creation(request, root)
        assert self.snapshot(root) == before_conflict

class TestProjectScaffolding:

    @staticmethod
    def snapshot(root: Path) -> tuple[tuple[str, ...], dict[str, bytes]]:
        directories = tuple(sorted((path.relative_to(root).as_posix() for path in root.rglob('*') if path.is_dir())))
        files = {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob('*') if path.is_file()}
        return (directories, files)

    def test_project_creation_generates_registered_project_and_initial_module(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        TestModuleScaffolding.create_workspace(root)
        (root / 'CMakeLists.txt').write_bytes(b'add_subdirectory(Engine)\r\nadd_subdirectory(Sandbox)')
        request = parse_build_request(['create', 'project', 'MyGame', '--path', 'Games With Spaces'])
        plan = build_scaffolding.plan_project_creation(request, root)
        build_scaffolding.execute_plan(plan)
        project_root = root / 'Games With Spaces'
        descriptor = json.loads((project_root / 'MyGame.dproject').read_text(encoding='utf-8'))
        module = json.loads((project_root / 'Source' / 'Runtime' / 'MyGame' / 'MyGame.dmodule').read_text(encoding='utf-8'))
        root_cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
        assert descriptor['ModuleDirs'] == {'MyGame': 'Source/Runtime/MyGame'}
        assert descriptor['BaseModules'] == ['MyGame']
        assert module['PrivateDependencies'] == ['Core']
        assert (project_root / 'Configs').is_dir()
        assert (project_root / 'Content').is_dir()
        assert (project_root / 'CMake' / 'MyGameSetup.cmake').is_file()
        assert root_cmake.count('add_subdirectory("Games With Spaces")') == 1
        assert 'add_subdirectory(Sandbox)\nadd_subdirectory("Games With Spaces")\n' in root_cmake.replace('\r\n', '\n')
        assert root_cmake.index('add_subdirectory(Sandbox)') < root_cmake.index('add_subdirectory("Games With Spaces")')
        workspace = build_descriptors.load_workspace_descriptors(root)
        assert workspace.find_module('MyGame').owning_project == 'MyGame'

    def test_project_dry_run_is_pure_and_direct_execution_reports_success(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        TestModuleScaffolding.create_workspace(root)
        request = parse_build_request(['create', 'project', 'MyGame', '--path', 'MyGame', '--dry-run', '--plain'])
        before = self.snapshot(root)
        stdout = io.StringIO()
        build_cli.execute_create_request(request, BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()), root=root)
        assert self.snapshot(root) == before
        assert 'MyGame/MyGame.dproject' in stdout.getvalue()
        assert 'replace file: CMakeLists.txt' in stdout.getvalue()
        create_request = parse_build_request(['create', 'project', 'MyGame', '--path', 'MyGame'])
        stdout = io.StringIO()
        build_cli.execute_create_request(create_request, BuildOutput(plain=True, stdout=stdout, stderr=io.StringIO()), root=root)
        assert 'Created project "MyGame"' in stdout.getvalue()

    def test_project_creation_rejects_names_paths_and_unsafe_registration(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        TestModuleScaffolding.create_workspace(root)
        invalid_requests = ((['create', 'project', 'Sandbox', '--path', 'Another'], 'already exists'), (['create', 'project', 'Core', '--path', 'Another'], 'Initial module name'), (['create', 'project', 'Nested', '--path', 'Games/Nested'], 'direct child'), (['create', 'project', 'InsideEngine', '--path', 'Engine/Nested'], 'overlaps project'), (['create', 'project', 'Unsafe', '--path', 'Unsafe$Path'], 'safely'))
        for arguments, message in invalid_requests:
            before = self.snapshot(root)
            with pytest.raises(errors.BuildToolError, match=message):
                build_scaffolding.plan_project_creation(parse_build_request(arguments), root)
            assert self.snapshot(root) == before
        outside = root.parent / 'OutsideProject'
        with pytest.raises(errors.BuildToolError, match='inside'):
            build_scaffolding.plan_project_creation(parse_build_request(['create', 'project', 'Outside', '--path', str(outside)]), root)
        with (root / 'CMakeLists.txt').open('a', encoding='utf-8') as stream:
            stream.write('add_subdirectory(Stale)\n')
        with pytest.raises(errors.BuildToolError, match='already has'):
            build_scaffolding.plan_project_creation(parse_build_request(['create', 'project', 'StaleProject', '--path', 'Stale']), root)

    def test_project_creation_failure_restores_root_and_removes_project_tree(self, tmp_path_factory: pytest.TempPathFactory) -> None:
        directory = tmp_path_factory.mktemp('case')
        root = Path(directory)
        TestModuleScaffolding.create_workspace(root)
        request = parse_build_request(['create', 'project', 'MyGame', '--path', 'MyGame'])
        plan = build_scaffolding.plan_project_creation(request, root)
        before = self.snapshot(root)

        def fail_after_root_replacement(phase: str, index: int, path: Path) -> None:
            if phase == 'after-replace' and path == (root / 'CMakeLists.txt').resolve():
                raise RuntimeError(f'injected project failure at {index}')
        with pytest.raises(RuntimeError, match='injected project failure'):
            build_scaffolding.execute_plan(plan, failure_injector=fail_after_root_replacement)
        assert self.snapshot(root) == before

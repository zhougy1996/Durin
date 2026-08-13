
import ast
from pathlib import Path


PACKAGE_ROOT = Path(__file__).resolve().parents[1] / "durin_dev_tool"


def imported_modules(relative_path: str) -> set[str]:
    tree = ast.parse((PACKAGE_ROOT / relative_path).read_text(encoding="utf-8"))
    modules: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            modules.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            modules.add(node.module or "")
    return modules


def assert_avoids(paths: tuple[str, ...], forbidden: tuple[str, ...]) -> None:
    violations = {
        path: sorted(
            module
            for module in imported_modules(path)
            if any(part in module.split(".") for part in forbidden)
        )
        for path in paths
    }
    assert not {path: modules for path, modules in violations.items() if modules}


def test_command_specifications_do_not_import_handlers_or_services() -> None:
    specifications = tuple(
        path.relative_to(PACKAGE_ROOT).as_posix()
        for path in (PACKAGE_ROOT / "commands").glob("*_specs.py")
    )
    assert_avoids(specifications, ("handler", "application", "services"))


def test_domain_models_do_not_depend_on_adapters_or_infrastructure() -> None:
    assert_avoids(
        (
            "bootstrap/models.py",
            "build/models.py",
            "build/requests.py",
            "documentation/model.py",
            "worktree/models.py",
        ),
        (
            "handler",
            "application",
            "installer",
            "manifests",
            "process",
            "sources",
            "transactions",
        ),
    )


def test_infrastructure_modules_do_not_depend_on_command_adapters() -> None:
    assert_avoids(
        (
            "bootstrap/installer.py",
            "bootstrap/manifests.py",
            "bootstrap/sources.py",
            "build/locking.py",
            "build/process.py",
            "worktree/git.py",
            "worktree/links.py",
            "worktree/terminal.py",
        ),
        ("handler", "application"),
    )


def test_application_services_do_not_import_command_handlers() -> None:
    assert_avoids(
        (
            "bootstrap/application.py",
            "bootstrap/dependency_service.py",
            "build/core.py",
            "documentation/service.py",
            "worktree/application.py",
            "worktree/transactions.py",
        ),
        ("handler",),
    )

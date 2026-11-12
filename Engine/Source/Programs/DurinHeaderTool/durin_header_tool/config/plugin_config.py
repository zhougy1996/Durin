"""Source-plugin metadata resolution, shared by DHT and DevTool (no build side effects)."""
from __future__ import annotations

import json
import re
from pathlib import Path

PHASES = ("Providers", "EngineReady", "EditorReady")
DEPENDENCIES = ("PublicDependencies", "PrivateDependencies",
                "OptionalPublicDependencies", "OptionalPrivateDependencies")
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def fingerprint(text: bytes) -> str:
    value = 14695981039346656037
    for byte in text:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{value:016x}"


def plugin_overrides(value, descriptor) -> dict[str, bool]:
    if not isinstance(value, dict):
        raise ValueError(f"{descriptor}: Plugins must be an object")
    seen = set()
    for key, enabled in value.items():
        if not IDENTIFIER.fullmatch(key) or type(enabled) is not bool or key.lower() in seen:
            raise ValueError(f"{descriptor}: invalid or duplicate override {key}")
        seen.add(key.lower())
    return dict(value)


def resolve_plugins(engine_root: Path, project_file: Path, *, platform="Win64",
                    runtime_variant="DurinEditor", project_files=(), discovery_directories=None):
    """Return (portable canonical selection manifest, machine-local locations).

    Build artifact identity is a separate schema; source selection never invents
    toolchain/configuration identity or claims native artifacts already exist.
    """
    if runtime_variant not in ("DurinEditor", "DurinGame") or not IDENTIFIER.fullmatch(platform):
        raise ValueError("invalid target")
    engine_root, project_file = Path(engine_root).resolve(), Path(project_file).resolve()
    inputs, locations, plugins, modules, projects = {}, {}, {}, {}, []

    def fail(path, message, chain=()):
        raise ValueError(f"{path}: {message}" + (" [chain " + " -> ".join(chain) + "]" if chain else ""))

    def read(path, logical):
        try:
            raw = path.read_bytes()
            def pairs(items):
                result = {}
                for key, value in items:
                    if key in result:
                        fail(path, "duplicate JSON field " + key)
                    result[key] = value
                return result
            data = json.loads(raw.decode("utf-8"), object_pairs_hook=pairs)
            if not isinstance(data, dict):
                fail(path, "descriptor must be an object")
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            fail(path, str(error))
        inputs[logical] = fingerprint(raw)
        locations[logical] = path
        return data

    def contained(root, relative):
        if (not isinstance(relative, str) or not relative or "\\" in relative
                or ":" in relative or Path(relative).is_absolute()
                or ".." in relative.split("/")):
            fail(root, "path escape or invalid relative path: " + str(relative))
        path = (root / relative).resolve()
        if not path.is_relative_to(root):
            fail(root, "path escape: " + relative)
        return path

    def ids(data, key, path):
        value = data.get(key, [])
        if (not isinstance(value, list) or any(not isinstance(v, str) or not IDENTIFIER.fullmatch(v) for v in value)
                or len({v.lower() for v in value}) != len(value)):
            fail(path, "invalid identifier list: " + key)
        return sorted(value, key=str.lower)

    def text(data, key, path, default=None):
        value = data.get(key, default)
        if not isinstance(value, str) or not value:
            fail(path, "invalid string: " + key)
        return value

    def add_module(path, logical, name, owner, host="Runtime", phase="Providers", platforms=None):
        if not IDENTIFIER.fullmatch(name):
            fail(path, "invalid module " + name)
        key = name.lower()
        if key in modules:
            fail(path, "duplicate module " + name + " also in " + str(modules[key]["path"]))
        data = read(path, logical) if not owner else {}
        if not owner and data.get("ModuleName") != name:
            fail(path, "ModuleName mismatch: " + name)
        modules[key] = dict(name=name, owner=owner, host=host, phase=phase,
                            platforms=platforms or [], path=path, data=data, logical=logical)
        for field in DEPENDENCIES:
            ids(data, field, path)

    files = {Path(p).resolve() for p in project_files} | {engine_root / "Engine.dproject", project_file}
    project_names = set()
    for path in sorted(files, key=lambda p: p.as_posix()):
        # ProjectName participates in logical identity; load strictly before assigning it.
        data = read(path, "Pending")
        name = text(data, "ProjectName", path)
        if not IDENTIFIER.fullmatch(name) or name.lower() in project_names:
            fail(path, "invalid or duplicate project " + name)
        project_names.add(name.lower())
        inputs["Project/" + name] = inputs.pop("Pending")
        locations["Project/" + name] = locations.pop("Pending")
        directories = data.get("ModuleDirs", {})
        if not isinstance(directories, dict):
            fail(path, "ModuleDirs must be an object")
        overrides = plugin_overrides(data.get("Plugins", {}), path)
        projects.append((path, data, overrides))
        for module, relative in sorted(directories.items()):
            add_module(contained(path.parent, relative) / (module + ".dmodule"),
                       "Project/" + name + "/" + module, module, "")

    roots = {engine_root / "Plugins", project_file.parent / "Plugins"}
    def discover(directory, search_root):
        if discovery_directories is not None:
            discovery_directories.add(directory)
        if not directory.exists():
            return
        if directory.is_symlink() or directory.absolute() != directory.resolve():
            fail(directory, "symbolic plugin directories are unsupported")
        if not directory.resolve().is_relative_to(search_root.resolve()):
            fail(directory, "path escape")
        entries = sorted(directory.iterdir(), key=lambda p: p.name)
        descriptors = [p for p in entries if p.suffix == ".dplugin" and p.is_file()]
        if len(descriptors) > 1:
            fail(directory, "multiple plugin descriptors")
        if descriptors:
            path = descriptors[0]
            if path.resolve().parent != directory.resolve():
                fail(path, "path escape")
            data = read(path, "Pending")
            allowed = {"$schema", "SchemaVersion", "Id", "DisplayName", "Version", "Dependencies",
                       "Platforms", "Content", "Shaders", "Configs", "Modules"}
            if set(data) - allowed or type(data.get("SchemaVersion")) is not int or data["SchemaVersion"] != 1:
                fail(path, "unknown schema version or field")
            name = text(data, "Id", path)
            if not IDENTIFIER.fullmatch(name) or name.lower() in plugins:
                fail(path, "invalid or duplicate plugin " + name)
            logical = "Plugin/" + name
            inputs[logical] = inputs.pop("Pending")
            locations[logical] = locations.pop("Pending")
            for field in ("$schema", "DisplayName", "Version"):
                if field in data:
                    text(data, field, path)
            for field in ("Content", "Shaders", "Configs"):
                if field in data:
                    contained(directory.resolve(), data[field])
            plugin = dict(name=name, path=path, data=data, dependencies=ids(data, "Dependencies", path),
                          platforms=ids(data, "Platforms", path))
            plugins[name.lower()] = plugin
            entries_modules = data.get("Modules", [])
            if not isinstance(entries_modules, list):
                fail(path, "Modules must be an array")
            for module in entries_modules:
                if not isinstance(module, dict) or set(module) - {"Name", "Path", "Host", "Phase", "Platforms"}:
                    fail(path, "invalid module fields")
                mod_name = text(module, "Name", path)
                host, phase = module.get("Host", "Runtime"), module.get("Phase", "Providers")
                if host not in ("Runtime", "Editor") or phase not in PHASES or (host == "Runtime" and phase == "EditorReady"):
                    fail(path, "invalid host/phase")
                mod_path = contained(directory.resolve(), text(module, "Path", path))
                add_module(mod_path / (mod_name + ".dmodule"), logical + "/" + mod_name,
                           mod_name, name, host, phase, ids(module, "Platforms", path))
            return
        for entry in entries:
            if entry.is_dir():
                discover(entry, search_root)

    for root in sorted(roots):
        discover(root, root)
    overrides = next(o for p, _, o in projects if p == project_file)
    explicit = {name.lower(): enabled for name, enabled in overrides.items()}
    for name in explicit:
        if name not in plugins:
            fail(project_file, "missing plugin descriptor " + name, [name])
    selected, visiting, reasons = set(), [], {}
    def visit(key, chain):
        if key not in plugins:
            fail(project_file, "missing plugin descriptor " + key, chain)
        plugin = plugins[key]
        if explicit.get(key) is False:
            fail(plugin["path"], "explicitly disabled plugin", chain)
        if plugin["platforms"] and platform not in plugin["platforms"]:
            fail(plugin["path"], "ineligible plugin platform", chain)
        if key in visiting:
            fail(plugin["path"], "plugin dependency cycle", chain)
        if key in selected:
            return
        visiting.append(key)
        for dep in plugin["dependencies"]:
            reasons.setdefault(dep.lower(), set()).add("RequiredBy:" + plugin["name"])
            visit(dep.lower(), chain + [dep])
        visiting.pop()
        selected.add(key)
        order.append(key)
    order = []
    for key in sorted(k for k, enabled in explicit.items() if enabled):
        reasons.setdefault(key, set()).add("Explicit")
        visit(key, [plugins[key]["name"]])

    enabled_modules = set()
    def edges(mod, include_optional=True, conditional_runtime=False):
        return [dep.lower() for field in DEPENDENCIES for dep in mod["data"].get(field, [])
                if not field.startswith("Optional") or
                (include_optional and dep.lower() in enabled_modules and eligible(modules[dep.lower()])
                 and not (conditional_runtime and not modules[dep.lower()]["owner"] and dep.lower() in editor_only))]
    def project_roots(variant):
        roots = []
        for path, data, _ in projects:
            if path not in (project_file, engine_root / "Engine.dproject"):
                continue
            roots.extend(data.get("BaseModules", list(data.get("ModuleDirs", {}))))
            roots.extend(data.get("ExtraModules", {}).get(variant, {}).get("Modules", []))
        return sorted({root.lower() for root in roots})

    def reachable(variant):
        seen = set()
        def walk(key):
            if key in seen or key not in modules:
                return
            seen.add(key)
            for dep in edges(modules[key], False):
                walk(dep)
        for root in project_roots(variant):
            walk(root)
        return seen
    def eligible(mod):
        return (not mod["owner"] or mod["owner"].lower() in selected) and (not mod["platforms"] or platform in mod["platforms"]) and (runtime_variant == "DurinEditor" or mod["host"] != "Editor")
    for mod in modules.values():
        if mod["owner"] and eligible(mod):
            owner_root = plugins[mod["owner"].lower()]["path"].parent
            mod["path"] = contained(owner_root, mod["path"].relative_to(owner_root).as_posix())
            mod["data"] = read(mod["path"], mod["logical"])
            if mod["data"].get("ModuleName") != mod["name"]:
                fail(mod["path"], "ModuleName mismatch: " + mod["name"])
            for field in DEPENDENCIES:
                ids(mod["data"], field, mod["path"])
    editor_only = reachable("DurinEditor") - reachable("DurinGame")
    active = {key for key, mod in modules.items() if mod["owner"] and eligible(mod)}
    def enable_module(key):
        if key in enabled_modules or key not in modules:
            return
        enabled_modules.add(key)
        for dep in edges(modules[key], False):
            enable_module(dep)
    for key in active | reachable(runtime_variant):
        enable_module(key)
    module_order, done, walk_seen = [], set(), set()
    def walk_module(key, chain, stack, source):
        if key not in modules:
            fail(source["path"], "missing module " + key, chain)
        mod = modules[key]
        if not eligible(mod):
            fail(mod["path"], "disabled or ineligible module", chain)
        if key in stack:
            fail(mod["path"], "module dependency cycle", chain)
        if key in walk_seen:
            return
        walk_seen.add(key)
        stack = stack + [key]
        # Existing Runtime modules may have editor-only optional implementation
        # dependencies. They do not make every Runtime plugin using Engine an
        # Editor plugin; plugin-owned optional edges still obey the host contract.
        for dep in sorted(edges(mod, conditional_runtime=source["host"] == "Runtime" and not mod["owner"])):
            target = modules.get(dep)
            next_chain = chain + [target["name"] if target else dep]
            if target:
                if source["host"] == "Runtime" and (target["host"] == "Editor" or (not target["owner"] and dep in editor_only)):
                    fail(mod["path"], "Runtime-to-Editor dependency", next_chain)
                if target["owner"] and mod["owner"] and mod["owner"].lower() != target["owner"].lower():
                    declared = plugins[mod["owner"].lower()]["dependencies"]
                    if target["owner"].lower() not in {v.lower() for v in declared}:
                        fail(mod["path"], "undeclared plugin dependency", next_chain)
                if target["owner"] and PHASES.index(source["phase"]) < PHASES.index(target["phase"]):
                    fail(mod["path"], "invalid phase edge", next_chain)
            walk_module(dep, next_chain, stack, source)
        if key in active and key not in done:
            done.add(key)
            module_order.append(dict(Name=mod["name"], Owner=mod["owner"], Host=mod["host"], Phase=mod["phase"]))
    for key in sorted(active, key=lambda k: (PHASES.index(modules[k]["phase"]), k)):
        walk_seen.clear()
        walk_module(key, [modules[key]["name"]], [], modules[key])
    # Existing project roots may not implicitly activate disabled plugin modules.
    if any(mod["owner"] for mod in modules.values()):
        for key in project_roots(runtime_variant):
            if key not in modules:
                fail(project_file, "missing module " + key, [key])
            source = dict(modules[key], host="Editor" if runtime_variant == "DurinEditor" else "Runtime")
            walk_seen.clear()
            walk_module(key, [source["name"]], [], source)
    manifest = dict(SchemaVersion=1, Platform=platform, RuntimeVariant=runtime_variant,
                    Plugins=[dict(Id=plugins[k]["name"], Reasons=sorted(reasons[k])) for k in order],
                    Modules=module_order,
                    Inputs=[dict(Path=key, Hash=inputs[key]) for key in sorted(inputs)])
    manifest["Fingerprint"] = fingerprint(canonical(manifest).encode("utf-8"))
    return manifest, locations

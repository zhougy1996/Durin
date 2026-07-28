from __future__ import annotations

import re
from pathlib import Path
from typing import Mapping

from .config import BuildToolError, REPOSITORY_CONFIG
from .scaffolding_workspace import require_contained_path


TEMPLATE_DIR = REPOSITORY_CONFIG.resolve(
    REPOSITORY_CONFIG.paths.scaffolding_templates
)
TEMPLATE_VARIABLE_PATTERN = re.compile(r"\{\{([A-Z][A-Z0-9_]*)\}\}")


class TemplateRenderer:
    def __init__(self, template_root: Path = TEMPLATE_DIR):
        self.template_root = template_root.resolve()

    def render(self, template_name: str, variables: Mapping[str, str]) -> bytes:
        template_path = require_contained_path(
            Path(template_name),
            self.template_root,
            label="Template path",
        )
        try:
            template = template_path.read_text(encoding="utf-8")
        except FileNotFoundError as exc:
            raise BuildToolError(f'Scaffolding template was not found: "{template_path}".') from exc
        except OSError as exc:
            raise BuildToolError(f'Could not read scaffolding template "{template_path}": {exc}') from exc
        expected = set(TEMPLATE_VARIABLE_PATTERN.findall(template))
        supplied = set(variables)
        missing = sorted(expected - supplied)
        unknown = sorted(supplied - expected)
        if missing or unknown:
            details = []
            if missing:
                details.append("missing " + ", ".join(missing))
            if unknown:
                details.append("unknown " + ", ".join(unknown))
            raise BuildToolError(
                f'Template variables for "{template_name}" are invalid: {"; ".join(details)}.'
            )
        rendered = TEMPLATE_VARIABLE_PATTERN.sub(
            lambda match: variables[match.group(1)],
            template,
        )
        unresolved = TEMPLATE_VARIABLE_PATTERN.findall(rendered)
        if unresolved:
            raise BuildToolError(
                f'Template "{template_name}" left unresolved variables: {", ".join(unresolved)}.'
            )
        return rendered.replace("\r\n", "\n").encode("utf-8")

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class ReflectionHeaderGenerationResult:
    header: str
    generated_header: str
    generated_source: str
    class_count: int
    property_count: int
    resolved_symbol_dependencies: dict[str, dict[str, str]] = field(default_factory=dict)
    elapsed_ms: float = field(default=0.0, compare=False)

    def to_json(self) -> dict[str, object]:
        return {
            "Header": self.header,
            "GeneratedHeader": self.generated_header,
            "GeneratedSource": self.generated_source,
            "ClassCount": self.class_count,
            "PropertyCount": self.property_count,
            "ResolvedSymbolDependencies": {
                symbol_name: dict(sorted(snapshot.items()))
                for symbol_name, snapshot in sorted(self.resolved_symbol_dependencies.items())
            },
        }

    @classmethod
    def from_json(cls, data: object) -> "ReflectionHeaderGenerationResult":
        expected = {
            "Header",
            "GeneratedHeader",
            "GeneratedSource",
            "ClassCount",
            "PropertyCount",
            "ResolvedSymbolDependencies",
        }
        if not isinstance(data, dict) or set(data) != expected:
            raise ValueError("The reflection generation result has an invalid JSON object shape.")
        for field_name in ("Header", "GeneratedHeader", "GeneratedSource"):
            if not isinstance(data[field_name], str) or (field_name == "Header" and not data[field_name]):
                raise ValueError(f"Reflection result field '{field_name}' must be a valid string.")
        for field_name in ("ClassCount", "PropertyCount"):
            value = data[field_name]
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"Reflection result field '{field_name}' must be a non-negative integer.")
        raw_dependencies = data["ResolvedSymbolDependencies"]
        if not isinstance(raw_dependencies, dict):
            raise ValueError("Reflection result dependencies must be an object.")
        dependencies: dict[str, dict[str, str]] = {}
        for symbol_name, raw_snapshot in raw_dependencies.items():
            if not isinstance(symbol_name, str) or not symbol_name or not isinstance(raw_snapshot, dict):
                raise ValueError("Reflection result dependencies must contain string-keyed objects.")
            snapshot: dict[str, str] = {}
            for key, value in raw_snapshot.items():
                if not isinstance(key, str) or not key or not isinstance(value, str):
                    raise ValueError("Reflection result dependency snapshots must contain string pairs.")
                snapshot[key] = value
            dependencies[symbol_name] = snapshot
        return cls(
            header=data["Header"],
            generated_header=data["GeneratedHeader"],
            generated_source=data["GeneratedSource"],
            class_count=data["ClassCount"],
            property_count=data["PropertyCount"],
            resolved_symbol_dependencies=dependencies,
        )

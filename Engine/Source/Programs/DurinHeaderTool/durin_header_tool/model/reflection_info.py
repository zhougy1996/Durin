from dataclasses import dataclass, field
from pathlib import Path


SYMBOL_NAME_SCHEME = "namespace-scoped-v2"
TOOL_VERSION = "22"


@dataclass(frozen=True, order=True)
class NamespaceSegment:
    name: str
    is_inline: bool = False

    def __post_init__(self) -> None:
        if not self.name:
            raise ValueError("A generated-symbol namespace segment must have a name.")


def namespace_path_from_name(namespace: str) -> tuple[NamespaceSegment, ...]:
    return tuple(NamespaceSegment(segment) for segment in namespace.split("::") if segment)


@dataclass(frozen=True)
class GeneratedSymbol:
    kind: str
    namespace_path: tuple[NamespaceSegment, ...]
    short_name: str

    def __post_init__(self) -> None:
        if self.kind not in {"class", "enum", "struct"}:
            raise ValueError(f"Unsupported reflected symbol kind '{self.kind}'.")
        if not self.short_name:
            raise ValueError("A generated symbol must have a short name.")

    @property
    def kind_token(self) -> str:
        return {"class": "DClass", "enum": "DEnum", "struct": "DStruct"}[self.kind]

    @property
    def namespace(self) -> str:
        return "::".join(segment.name for segment in self.namespace_path)

    def qualify(self, local_name: str) -> str:
        return "::" + "::".join((*[segment.name for segment in self.namespace_path], local_name))

    @property
    def local_helper_name(self) -> str:
        return f"Z_Construct_{self.kind_token}_{self.short_name}"

    @property
    def helper_reference(self) -> str:
        return self.qualify(self.local_helper_name)

    @property
    def local_helper_no_register_name(self) -> str:
        return f"{self.local_helper_name}_NoRegister"

    @property
    def helper_no_register_reference(self) -> str:
        return self.qualify(self.local_helper_no_register_name)

    @property
    def local_statics_name(self) -> str:
        return f"{self.local_helper_name}_Statics"

    @property
    def statics_reference(self) -> str:
        return self.qualify(self.local_statics_name)

    @property
    def local_registration_info_name(self) -> str:
        return f"Z_Registration_Info_{self.kind_token}_{self.short_name}"

    @property
    def registration_info_reference(self) -> str:
        return self.qualify(self.local_registration_info_name)


def make_generated_symbol(
    kind: str,
    short_name: str,
    namespace_path: tuple[NamespaceSegment, ...] = (),
    *,
    namespace: str = "",
) -> GeneratedSymbol:
    return GeneratedSymbol(kind, namespace_path or namespace_path_from_name(namespace), short_name)


def _generated_symbol_from_qualified_name(kind: str, qualified_name: str) -> GeneratedSymbol:
    parts = [part for part in qualified_name.removeprefix("::").split("::") if part]
    if not parts:
        raise ValueError("A reflected qualified name must not be empty.")
    return make_generated_symbol(
        kind,
        parts[-1],
        tuple(NamespaceSegment(part) for part in parts[:-1]),
    )


def make_generated_helper_name(qualified_name: str) -> str:
    return _generated_symbol_from_qualified_name("class", qualified_name).helper_reference


def make_generated_enum_helper_name(qualified_name: str) -> str:
    return _generated_symbol_from_qualified_name("enum", qualified_name).helper_reference


def make_generated_struct_helper_name(qualified_name: str) -> str:
    return _generated_symbol_from_qualified_name("struct", qualified_name).helper_reference


@dataclass
class ReflectedEnumValueInfo:
    name: str
    value: int
    display_name: str = ""


@dataclass
class ReflectedEnumInfo:
    short_name: str
    namespace: str
    qualified_name: str
    header: str
    api: str
    namespace_path: tuple[NamespaceSegment, ...] = ()
    is_scoped: bool = False
    underlying_type: str = ""
    underlying_kind: str = "Unknown"
    underlying_size: int = 0
    display_name: str = ""
    legacy_names: list[str] = field(default_factory=list)
    values: list[ReflectedEnumValueInfo] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.namespace_path:
            self.namespace_path = namespace_path_from_name(self.namespace)

    @property
    def generated_symbol(self) -> GeneratedSymbol:
        return make_generated_symbol("enum", self.short_name, self.namespace_path)

    @property
    def generated_helper_name(self) -> str:
        return self.generated_symbol.local_helper_name

    @property
    def generated_helper_reference(self) -> str:
        return self.generated_symbol.helper_reference

    @property
    def generated_helper_no_register_name(self) -> str:
        return self.generated_symbol.local_helper_no_register_name

    @property
    def generated_helper_no_register_reference(self) -> str:
        return self.generated_symbol.helper_no_register_reference

    @property
    def generated_statics_name(self) -> str:
        return self.generated_symbol.local_statics_name

    @property
    def generated_statics_reference(self) -> str:
        return self.generated_symbol.statics_reference

    @property
    def registration_info_name(self) -> str:
        return self.generated_symbol.local_registration_info_name

    @property
    def registration_info_reference(self) -> str:
        return self.generated_symbol.registration_info_reference


@dataclass
class ReflectedPropertyMetadataInfo:
    display_name: str = ""
    tooltip: str = ""
    category: str = ""
    units: str = ""
    step: str = ""
    precision: int | None = None
    clamp_min: str = ""
    clamp_max: str = ""
    ui_min: str = ""
    ui_max: str = ""
    numeric_kind: str = ""

    def is_empty(self) -> bool:
        return not any((self.display_name, self.tooltip, self.category, self.units,
                        self.step, self.precision is not None, self.clamp_min,
                        self.clamp_max, self.ui_min, self.ui_max))


@dataclass
class ReflectedPropertyDeprecationInfo:
    custom_version_type: str
    deprecated_before: str
    historical_name: str
    migrates_to: list[str] = field(default_factory=list)


@dataclass
class ReflectedPropertyInfo:
    name: str
    type_name: str
    kind: str
    referenced_type: str = ""
    referenced_enum_type: str = ""
    referenced_struct_type: str = ""
    array_dim: int = 1
    element_size: str = "0"
    flags: str = "None"
    is_object_ptr_wrapper: bool = False
    inner: "ReflectedPropertyInfo | None" = None
    key: "ReflectedPropertyInfo | None" = None
    value: "ReflectedPropertyInfo | None" = None
    metadata: list[tuple[str, str]] = field(default_factory=list)
    typed_metadata: ReflectedPropertyMetadataInfo | None = None
    deprecation: ReflectedPropertyDeprecationInfo | None = None
    legacy_names: list[str] = field(default_factory=list)


@dataclass
class ReflectedClassInfo:
    short_name: str
    namespace: str
    qualified_name: str
    header: str
    api: str
    namespace_path: tuple[NamespaceSegment, ...] = ()
    base_qualified_name: str = ""
    generated_body_line: int = 0
    has_default_constructor: bool = False
    has_object_initializer_constructor: bool = False
    has_destructor: bool = False
    is_abstract: bool = False
    no_class_default_object: bool = False
    display_name: str = ""
    default_object_name: str = ""
    legacy_names: list[str] = field(default_factory=list)
    properties: list[ReflectedPropertyInfo] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.namespace_path:
            self.namespace_path = namespace_path_from_name(self.namespace)

    @property
    def generated_symbol(self) -> GeneratedSymbol:
        return make_generated_symbol("class", self.short_name, self.namespace_path)

    @property
    def generated_helper_name(self) -> str:
        return self.generated_symbol.local_helper_name

    @property
    def generated_helper_reference(self) -> str:
        return self.generated_symbol.helper_reference

    @property
    def generated_helper_no_register_name(self) -> str:
        return self.generated_symbol.local_helper_no_register_name

    @property
    def generated_helper_no_register_reference(self) -> str:
        return self.generated_symbol.helper_no_register_reference

    @property
    def generated_statics_name(self) -> str:
        return self.generated_symbol.local_statics_name

    @property
    def generated_statics_reference(self) -> str:
        return self.generated_symbol.statics_reference

    @property
    def registration_info_name(self) -> str:
        return self.generated_symbol.local_registration_info_name

    @property
    def registration_info_reference(self) -> str:
        return self.generated_symbol.registration_info_reference


@dataclass
class ReflectedStructInfo:
    short_name: str
    namespace: str
    qualified_name: str
    header: str
    api: str
    namespace_path: tuple[NamespaceSegment, ...] = ()
    generated_body_line: int = 0
    legacy_names: list[str] = field(default_factory=list)
    properties: list[ReflectedPropertyInfo] = field(default_factory=list)

    def __post_init__(self) -> None:
        if not self.namespace_path:
            self.namespace_path = namespace_path_from_name(self.namespace)

    @property
    def generated_symbol(self) -> GeneratedSymbol:
        return make_generated_symbol("struct", self.short_name, self.namespace_path)

    @property
    def generated_helper_name(self) -> str:
        return self.generated_symbol.local_helper_name

    @property
    def generated_helper_reference(self) -> str:
        return self.generated_symbol.helper_reference

    @property
    def generated_helper_no_register_name(self) -> str:
        return self.generated_symbol.local_helper_no_register_name

    @property
    def generated_helper_no_register_reference(self) -> str:
        return self.generated_symbol.helper_no_register_reference

    @property
    def generated_statics_name(self) -> str:
        return self.generated_symbol.local_statics_name

    @property
    def generated_statics_reference(self) -> str:
        return self.generated_symbol.statics_reference


@dataclass
class ReflectedHeaderInfo:
    module_name: str
    header: str
    header_path: Path
    include_path: str
    file_id: str
    classes: list[ReflectedClassInfo] = field(default_factory=list)
    enums: list[ReflectedEnumInfo] = field(default_factory=list)
    structs: list[ReflectedStructInfo] = field(default_factory=list)

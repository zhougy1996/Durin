from dataclasses import dataclass, field
from pathlib import Path


SYMBOL_NAME_SCHEME = "qualified-underscore-v1"
TOOL_VERSION = "21"


def qualified_name_to_helper_suffix(qualified_name: str) -> str:
    segments = [segment for segment in qualified_name.split("::") if segment]
    for segment in segments:
        if "_" in segment:
            raise ValueError(
                f"Reflected symbol segment '{segment}' in '{qualified_name}' contains '_', "
                f"which is not allowed by {SYMBOL_NAME_SCHEME}."
            )
    return "_".join(segments)


def make_generated_helper_name(qualified_name: str) -> str:
    return f"Z_Construct_DClass_{qualified_name_to_helper_suffix(qualified_name)}"


def make_generated_enum_helper_name(qualified_name: str) -> str:
    return f"Z_Construct_DEnum_{qualified_name_to_helper_suffix(qualified_name)}"


def make_generated_struct_helper_name(qualified_name: str) -> str:
    return f"Z_Construct_DStruct_{qualified_name_to_helper_suffix(qualified_name)}"


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
    generated_helper_name: str
    header: str
    api: str
    is_scoped: bool = False
    underlying_type: str = ""
    underlying_kind: str = "Unknown"
    underlying_size: int = 0
    display_name: str = ""
    legacy_names: list[str] = field(default_factory=list)
    values: list[ReflectedEnumValueInfo] = field(default_factory=list)

    @property
    def generated_helper_no_register_name(self) -> str:
        return f"{self.generated_helper_name}_NoRegister"

    @property
    def generated_statics_name(self) -> str:
        return f"{self.generated_helper_name}_Statics"

    @property
    def registration_info_name(self) -> str:
        return f"Z_Registration_Info_DEnum_{qualified_name_to_helper_suffix(self.qualified_name)}"


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
    generated_helper_name: str
    header: str
    api: str
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

    @property
    def generated_helper_no_register_name(self) -> str:
        return f"{self.generated_helper_name}_NoRegister"

    @property
    def generated_statics_name(self) -> str:
        return f"{self.generated_helper_name}_Statics"

    @property
    def registration_info_name(self) -> str:
        return f"Z_Registration_Info_DClass_{qualified_name_to_helper_suffix(self.qualified_name)}"


@dataclass
class ReflectedStructInfo:
    short_name: str
    namespace: str
    qualified_name: str
    generated_helper_name: str
    header: str
    api: str
    generated_body_line: int = 0
    legacy_names: list[str] = field(default_factory=list)
    properties: list[ReflectedPropertyInfo] = field(default_factory=list)

    @property
    def generated_helper_no_register_name(self) -> str:
        return f"{self.generated_helper_name}_NoRegister"

    @property
    def generated_statics_name(self) -> str:
        return f"{self.generated_helper_name}_Statics"


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

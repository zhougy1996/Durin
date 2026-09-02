import json
import re
import sys
import traceback
from pathlib import Path
from unittest import mock

import clang.cindex
import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.config.module_config import DurinModuleConfig
from durin_header_tool.extractors.export_symbol_extractor import (
    _extract_header_export_symbols_impl,
    extract_module_export_info,
    resolve_module_export_info,
)
from durin_header_tool.generators import module_reflection_files_generator as reflection_generator
from durin_header_tool.generators.module_reflection_files_generator import (
    _write_reflection_files,
    make_new_reflection_state,
)
from durin_header_tool.model.export_info import (
    ExportedSymbolInfo,
    load_module_export_file,
    save_module_export_file,
)
from durin_header_tool.cache.phase_state import ReflectionPhaseState, save_reflection_phase_state
from durin_header_tool.model.reflection_info import (
    ReflectedEnumInfo,
    ReflectedEnumValueInfo,
    make_generated_enum_helper_name,
    make_generated_helper_name,
)
from durin_header_tool.parser.reflection_parser import (
    _make_property_from_spelling,
    _make_property_from_type,
    _parse_translation_unit,
    _scan_generated_body_line,
    _validate_explicit_container_spelling,
    _validate_soft_object_spelling,
    make_dht_parse_source,
    parse_reflection_header,
)
from durin_header_tool.parser import property_parser, reflection_parser
from durin_header_tool.resolver.reflection_resolver import (
    load_available_symbols,
    resolve_header_symbols,
    resolve_symbol,
    resolved_symbol_dependencies_for_header,
)
from durin_header_tool.resolver import reflection_resolver
from durin_header_tool.writers.reflection_source_writer import (
    _enum_definitions,
    generate_cpp_content,
    generate_header_content,
)



def build_reflection_fixture(cls, temp_root):
        cls.temp_root = temp_root
        cls.module_dir = cls.temp_root / "Fixture"
        cls.header = "Public/FixtureTypes.h"
        header_path = cls.module_dir / cls.header
        header_path.parent.mkdir(parents=True)
        header_path.write_text(
            '''#pragma once

namespace Durin
{
    struct FVector3f {};
    struct FVector3 {};
    struct FLinearColor {};
    class FName {};
    struct FGuid {};
    class DObject {};
    template<typename T> class TObjectPtr {};
    template<typename T> class TSoftObjectPtr {};
    template<typename T> class TWeakObjectPtr {};
    using int32 = int;
    template<typename T> struct TDStructOpsTraitsBase {};
    template<typename T> struct TDStructOpsTraits {};
}

namespace std
{
	enum class byte : unsigned char {};
	template<typename T>
    class vector {};
    template<typename K, typename V>
    class unordered_map {};
}

namespace Fixture
{
    using FFloatVector = std::vector<float>;

    DENUM(DisplayName = "Fixture Mode", LegacyNames = "Legacy::EFixtureMode")
    enum class EFixtureMode : int
    {
        Disabled DMETA(DisplayName = "Not Enabled") = -1,
        URLValue,
        FinalValue DMETA(DisplayName = "Final (Ready)") = 7,
    };

    DENUM()
    enum ELegacyMode : unsigned char
    {
        LegacyFirst,
        LegacySecond = 4
    };

    DCLASS(DisplayName = "Sample Actor", DefaultObjectName = "SampleActor", LegacyNames = "Legacy::ASampleActor;Older::ASampleActor")
    class ASampleActor : public Durin::DObject
    {
        GENERATED_BODY()

        DPROPERTY(Edit, ReadOnly, DisplayName = "Scalar Value", ToolTip = "Authored scalar", Category = "Numbers", Units = "Percent", Step = "1", Precision = 3, ClampMin = "-100", ClampMax = "100", UIMin = "-50", UIMax = "50")
        float Value = 0.0f;

        DPROPERTY(EditorOnly)
        int32 EditorDiagnostic = 0;

        DPROPERTY(Deprecated)
        int32 Value_DEPRECATED = 0;

        DPROPERTY(LegacyNames = "OldRenamedValue;OlderRenamedValue")
        float RenamedValue = 0.0f;

        DPROPERTY(Edit, MetaData = "HideAlpha=true")
        Durin::FLinearColor Color{};

        DPROPERTY(Edit)
        Durin::FName Identifier{};

        DPROPERTY(Edit)
        Durin::FGuid PersistentId{};

        DPROPERTY()
        std::vector<Durin::FGuid> RelatedIds;

        DPROPERTY(Edit, MetaData = "Role=Primary")
        std::string Label;

        DPROPERTY()
        float Weights[2]{};

        DPROPERTY()
        EFixtureMode Mode = EFixtureMode::Disabled;

        DPROPERTY()
        Durin::DObject* RawReference = nullptr;

        DPROPERTY()
        Durin::TObjectPtr<Durin::DObject> StrongReference;
    };

    DCLASS(Abstract, DisplayName = "Abstract Actor")
    class AAbstractActor : public ASampleActor
    {
        GENERATED_BODY()
    };

    DCLASS(NoClassDefaultObject)
    class AInfrastructure : public Durin::DObject
    {
        GENERATED_BODY()
    };

    DSTRUCT(LegacyNames = "Legacy::FCurvePoint")
    struct FCurvePoint
    {
        GENERATED_BODY()

        DPROPERTY()
        Durin::FVector3 Position{};

        DPROPERTY()
        Durin::FVector3 Tangent{};

        DPROPERTY()
        Durin::FVector3f CompactTangent{};
    };

    DSTRUCT()
    struct FMoveOnly
    {
        GENERATED_BODY()
        FMoveOnly() = default;
        FMoveOnly(const FMoveOnly&) = delete;
        auto operator=(const FMoveOnly&) -> FMoveOnly& = delete;
        FMoveOnly(FMoveOnly&&) = default;
        auto operator=(FMoveOnly&&) -> FMoveOnly& = default;
    };

    DSTRUCT()
    struct FDeletedDefault
    {
        GENERATED_BODY()
        FDeletedDefault() = delete;
        explicit FDeletedDefault(int InValue);
        int Value = 0;
    };

    DSTRUCT()
    struct FTrivialOps
    {
        GENERATED_BODY()
        DPROPERTY()
        float Value = 0.0f;
    };

    DSTRUCT()
    struct FNonTrivialOps
    {
        GENERATED_BODY()
        DPROPERTY()
        std::vector<float> Values;
    };

    DSTRUCT()
    struct FCustomEquality
    {
        GENERATED_BODY()
        DPROPERTY()
        float Value = 0.0f;
    };

    DSTRUCT()
    struct FMalformedEquality
    {
        GENERATED_BODY()
    };

    DSTRUCT()
    struct FUnavailableContainers
    {
        GENERATED_BODY()
        DPROPERTY()
        std::vector<FDeletedDefault> Values;
        DPROPERTY()
        std::unordered_map<float, FMoveOnly> MoveValues;
    };

    DSTRUCT()
    struct FWeakNested
    {
        GENERATED_BODY()
        DPROPERTY(Transient)
        Durin::TWeakObjectPtr<Durin::DObject> Target;
    };

    DCLASS()
    class AContainerShapes : public Durin::DObject
    {
        GENERATED_BODY()

		DPROPERTY()
		std::vector<int32> DirectScores;

		DPROPERTY()
		std::byte DirectByte{};

		DPROPERTY()
		std::byte FixedBytes[2]{};

		DPROPERTY()
		Durin::FByteArray Blob;

        DPROPERTY()
        FFloatVector AliasedScores;

        DPROPERTY()
        std::vector<std::vector<int32>> NestedScores;

        DPROPERTY()
        std::vector<Durin::TObjectPtr<Durin::DObject>> ObjectReferences;

        DPROPERTY()
        std::unordered_map<std::string, int32> NamedScores;

        DPROPERTY()
        std::unordered_map<std::string, std::vector<Durin::TObjectPtr<Durin::DObject>>> ObjectLists;

        DPROPERTY(Edit)
        Durin::TSoftObjectPtr<Durin::DObject> SoftReference;

        DPROPERTY()
        Durin::TSoftObjectPtr<Durin::DObject> SoftFixed[2];

        DPROPERTY()
        std::vector<Durin::TSoftObjectPtr<Durin::DObject>> SoftReferences;

        DPROPERTY()
        std::unordered_map<std::string, std::vector<Durin::TSoftObjectPtr<Durin::DObject>>> SoftLists;

	    DPROPERTY(Transient)
	    Durin::TWeakObjectPtr<Durin::DObject> WeakReference;

	    DPROPERTY(Transient)
	    Durin::TWeakObjectPtr<Durin::DObject> WeakFixed[2];

	    DPROPERTY(Transient)
	    std::vector<Durin::TWeakObjectPtr<Durin::DObject>> WeakReferences;

	    DPROPERTY(Transient)
	    std::unordered_map<std::string, Durin::TWeakObjectPtr<Durin::DObject>> WeakValues;

        DPROPERTY(Transient)
        FWeakNested WeakNested;
    };

    DSTRUCT()
    struct FNonTrivialDestructor
    {
        GENERATED_BODY()
        ~FNonTrivialDestructor();
    };

    DSTRUCT()
    struct FStructPropertyShapes
    {
        GENERATED_BODY()

        DPROPERTY()
        FTrivialOps Direct;

        DPROPERTY()
        FDeletedDefault DeletedDefault;

        DPROPERTY()
        FMoveOnly DeletedCopy;

        DPROPERTY()
        FNonTrivialDestructor NonTrivialDestructor;

        DPROPERTY(Edit, MetaData = "Role=Primary")
        FTrivialOps Metadata;

        DPROPERTY()
        FTrivialOps Fixed[3];

        DPROPERTY()
        std::vector<FDeletedDefault> ArrayValues;

        DPROPERTY()
        std::unordered_map<FMoveOnly, float> MapByStruct;

        DPROPERTY()
        std::unordered_map<float, FDeletedDefault> MapToStruct;
    };
}

template<>
struct Durin::TDStructOpsTraits<Fixture::FCustomEquality>
    : Durin::TDStructOpsTraitsBase<Fixture::FCustomEquality>
{
    static constexpr bool bWithIdentical = true;
    static bool Identical(const Fixture::FCustomEquality&, const Fixture::FCustomEquality&);
};

template<>
struct Durin::TDStructOpsTraits<Fixture::FMalformedEquality>
    : Durin::TDStructOpsTraitsBase<Fixture::FMalformedEquality>
{
    static constexpr bool bWithIdentical = true;
    static void Identical(const Fixture::FMalformedEquality&, const Fixture::FMalformedEquality&);
};
''',
            encoding="utf-8",
        )
        cls.module_config = DurinModuleConfig(
            module_name="Fixture",
            module_dir=cls.module_dir,
            reflect_headers=[cls.header],
        )
        cls.dht_output_dir = cls.temp_root / "DHT"

        with (
            mock.patch.object(configs, "get_module_config", return_value=cls.module_config),
            mock.patch.object(configs, "collect_all_dependent_modules", return_value=set()),
            mock.patch.object(utils, "get_module_dht_output_dir", return_value=cls.dht_output_dir),
        ):
            cls.export_info = extract_module_export_info("Fixture")
            cls.symbols = dict(cls.export_info.Symbols)
            cls.symbols.update({
                "Durin::DObject": ExportedSymbolInfo(
                    Kind="class", ShortName="DObject", Namespace="Durin", QualifiedName="Durin::DObject",
                    Header="DObject/Object.h", API="COREDOBJECT_API",
                ),
                "Durin::FVector3": ExportedSymbolInfo(
                    Kind="struct", ShortName="FVector3", Namespace="Durin", QualifiedName="Durin::FVector3",
                    Header="DObject/MathStructs.h", API="COREDOBJECT_API",
                ),
                "Durin::FVector3f": ExportedSymbolInfo(
                    Kind="struct", ShortName="FVector3f", Namespace="Durin", QualifiedName="Durin::FVector3f",
                    Header="DObject/MathStructs.h", API="COREDOBJECT_API",
                ),
                "Durin::FLinearColor": ExportedSymbolInfo(
                    Kind="struct", ShortName="FLinearColor", Namespace="Durin", QualifiedName="Durin::FLinearColor",
                    Header="DObject/MathStructs.h", API="COREDOBJECT_API",
                ),
            })
            cls.header_info = parse_reflection_header("Fixture", cls.header, exported_symbols=cls.symbols)
            cls.generated_cpp = generate_cpp_content(cls.header_info, cls.symbols)


@pytest.fixture(scope="class")
def reflection_fixture(request, tmp_path_factory):
    build_reflection_fixture(request.cls, tmp_path_factory.mktemp("dht-reflection"))

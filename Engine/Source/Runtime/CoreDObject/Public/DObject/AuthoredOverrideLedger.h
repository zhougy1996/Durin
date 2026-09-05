#pragma once

#include "CoreDObjectAPI.h"
#include "Misc/Name.h"

namespace Durin
{
	class DObject;

	enum class EAuthoredOverrideProvenance : uint8 { LoadedExplicit, Forced };
	enum class EAuthoredOverridePathTokenKind : uint8
	{
		Field,
		FixedArrayElement,
		ArrayElement,
		MapValue,
	};

	struct FAuthoredOverridePathToken
	{
		EAuthoredOverridePathTokenKind Kind = EAuthoredOverridePathTokenKind::Field;
		FName DeclaringType;
		FName FieldName;
		uint64 Index = 0;
		FByteBuffer MapKeyToken;

		static COREDOBJECT_API auto Field(FName DeclaringType, FName FieldName)
			-> FAuthoredOverridePathToken;
		static COREDOBJECT_API auto FixedArrayElement(uint64 Index)
			-> FAuthoredOverridePathToken;
		static COREDOBJECT_API auto ArrayElement(uint64 Index)
			-> FAuthoredOverridePathToken;
		static COREDOBJECT_API auto MapValue(FByteBuffer CanonicalKeyToken)
			-> FAuthoredOverridePathToken;
	};

	using FAuthoredOverridePath = std::vector<FAuthoredOverridePathToken>;

	struct FAuthoredOverrideEntry
	{
		FAuthoredOverridePath Path;
		EAuthoredOverrideProvenance Provenance = EAuthoredOverrideProvenance::LoadedExplicit;
	};

	enum class EAuthoredOverrideFailureReason : uint8
	{
		None,
		InvalidObject,
		TemplateObject,
		EmptyPath,
		DepthLimit,
		PathLimit,
		InvalidToken,
		InvalidProvenance,
		DuplicatePath,
		FieldNotFound,
		TypeMismatch,
		IndexOutOfRange,
		MapKeyUnavailable,
		MapKeyNotFound,
		SchemaMismatch,
		ArchiveFailure,
	};

	struct FAuthoredOverrideDiagnostic
	{
		EAuthoredOverrideFailureReason Reason = EAuthoredOverrideFailureReason::None;
		std::string LogicalPath;

		auto Reset() -> void { *this = {}; }
	};

	class FAuthoredOverrideLedger final
	{
	public:
		auto GetEntries() const -> const std::vector<FAuthoredOverrideEntry>& { return Entries; }

	private:
		std::vector<FAuthoredOverrideEntry> Entries;
		friend class DObject;
	};

	COREDOBJECT_API auto CompareAuthoredOverridePaths(
		const FAuthoredOverridePath& Left,
		const FAuthoredOverridePath& Right) -> std::strong_ordering;
	COREDOBJECT_API auto IsAuthoredOverridePathPrefix(
		const FAuthoredOverridePath& Prefix,
		const FAuthoredOverridePath& Path) -> bool;
	COREDOBJECT_API auto ValidateAuthoredOverridePath(
		DObject* Object,
		const FAuthoredOverridePath& Path,
		FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr) -> bool;
	COREDOBJECT_API auto ValidateAuthoredOverrideEntries(
		DObject* Object,
		std::span<const FAuthoredOverrideEntry> Entries,
		FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr) -> bool;
}

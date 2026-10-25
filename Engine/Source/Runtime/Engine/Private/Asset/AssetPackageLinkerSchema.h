#pragma once

#include "CoreMinimal.h"
#include "Asset/Load.h"
#include "DObject/PackageFormat.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin { class DStructBase; class DClass; }

namespace Durin::AssetPrivate::LinkerSchema
{
	enum class EPropertyLoadRoute : uint8 { Current, Deprecated, Removed, Incompatible };
	struct FPropertyLoadBinding
	{
		EPropertyLoadRoute Route = EPropertyLoadRoute::Incompatible;
		FProperty* Property = nullptr;
		// Cooked native fields may lack reflection. A mismatched current property
		// must still reject; this is distinct from a missing native projection.
		bool bHasCurrentProperty = false;
	};

	// One compatibility rule shared by linker admission and the load Archive.
	auto ResolvePropertyForLoad(DStructBase* Owner, std::string_view StoredName,
		DurinCodeGen::EPropertyGenFlags Kind, std::string_view Signature) -> FPropertyLoadBinding;

	struct FPropertyLoadInfo
	{
		const ObjectPackage::FSerializedField* Descriptor = nullptr;
		FPropertyLoadBinding Match;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string Signature;
	};

	// Direct lookups into the Linker-owned, normalized tables. Results are used
	// while processing the current property; no schema/type registry is retained.
	auto FindSchema(const ObjectPackage::FLinkerTables& Linker, std::string_view Name)
		-> const ObjectPackage::FSerializedSchema*;
	auto FindPropertyByName(const ObjectPackage::FSerializedSchema& Schema, std::string_view Name)
		-> std::optional<FPropertyLoadInfo>;

	// Fixups precede object construction and preserve original identities as
	// evidence. A failed candidate is discarded; evidence is replaced on success.
	auto FixupSerializedNames(ObjectPackage::FLinkerTables& Linker,
		std::vector<FAssetCanonicalizationEvidence>& OutEvidence) -> FAssetReadResult;

	auto TypeWidth(ObjectPackage::EValueKind Kind) -> uint64;
	auto IntrinsicName(uint64 Layout) -> std::string_view;
}

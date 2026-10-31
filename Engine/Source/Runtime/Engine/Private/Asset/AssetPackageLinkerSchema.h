#pragma once

#include "CoreMinimal.h"
#include "Asset/Load.h"
#include "DObject/PackageFormat.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectKey.h"

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

	// One normalized linker and owner-thread reflection snapshot; never retained globally.
	// Traversal consumed bits and object values are deliberately outside this cache.
	class ENGINE_API FLoadSchemaBindings final
	{
	public:
		explicit FLoadSchemaBindings(const ObjectPackage::FLinkerTables& InLinker) : Linker(InLinker) {}
		~FLoadSchemaBindings();
		FLoadSchemaBindings(const FLoadSchemaBindings&) = delete;
		auto operator=(const FLoadSchemaBindings&) -> FLoadSchemaBindings& = delete;
		auto FindProperty(const ObjectPackage::FSerializedSchema& Schema, std::string_view Name) -> const FPropertyLoadInfo*;
		auto GetBuildCount() const -> uint64 { return Builds; }
		auto GetReuseCount() const -> uint64 { return Reuses; }
	private:
		struct FEntry
		{
			FPropertyLoadInfo Info;
			FObjectKey Owner;
			DStructBase* OwnerPointer = nullptr;
		};
		const ObjectPackage::FLinkerTables& Linker;
		std::unordered_map<const ObjectPackage::FSerializedField*, FEntry> Entries;
		uint64 Builds = 0, Reuses = 0;
	};

	// Fixups precede object construction and preserve original identities as
	// evidence. A failed candidate is discarded; evidence is replaced on success.
	auto FixupSerializedNames(ObjectPackage::FLinkerTables& Linker,
		std::vector<FAssetCanonicalizationEvidence>& OutEvidence) -> FAssetReadResult;

	auto TypeWidth(ObjectPackage::EValueKind Kind) -> uint64;
	auto IntrinsicName(uint64 Layout) -> std::string_view;
}

#pragma once

#include "CoreMinimal.h"

#include "AssetRegistry/Catalog.h"
#include "Asset/AssetReadResult.h"
#include "Asset/PackageResource.h"
#include "DObject/Archive.h"
#include "DObject/PackageLinker.h"
#include "DObject/PackageValueCodec.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::AssetPrivate
{
	struct FAuthoredPackageSummary
	{
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FPackagePath RedirectDestination;
		std::vector<FPackagePath> Dependencies;
	};

	// Per-package sources shared by all object reads, for authored or cooked data.
	// The loader owns dependency admission/lifetime and keeps these bindings alive
	// during each read. Missing bindings fail instead of falling back to live state.
	struct FPackageLoadBindings
	{
		FPackageResourceHandle BulkResource;
		std::function<FAssetReadResult(const FObjectPath&, DObject*&)> ResolveExternalObject;
	};

	// Applies fields only; this does not construct a graph, invoke PostLoad, or
	// roll back serializer writes. Callers preparing a replacement must own the target.
	// Fields and Linker are borrowed unchanged for the synchronous Serialize call.
	ENGINE_API auto LoadPackageObject(
		DObject& Object,
		std::span<const ObjectPackage::FPropertyTag> Fields,
		const ObjectPackage::FLinkerTables& Linker,
		std::span<DObject* const> Objects,
		const FPackageLoadBindings& Bindings,
		const FArchiveState& Context = {}) -> FAssetReadResult;

	// Byte-tool restoration may resolve external object references. It is a load
	// operation; construct-free inspection only calls it for explicit typed reads.
	auto DecodeReferenceByteToolValue(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		PackagePrivate::FByteReader& Reader,
		const std::vector<DObject*>& Objects,
		uint32 SourceVersion) -> FAssetReadResult;
}

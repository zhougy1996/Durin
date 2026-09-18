#pragma once

#include "AssetRegistry/Catalog.h"
#include "Asset/AssetDefinitions.h"
#include "Asset/PackageResource.h"
#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::AssetPrivate
{
	struct FAuthoredPackageFieldRecord
	{
		std::string DeclaringClass;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		FByteBuffer Payload;
	};

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
		std::function<FAssetResult(const FObjectPath&, DObject*&)> ResolveExternalObject;
	};

	// Applies fields only; this does not construct a graph, invoke PostLoad, or
	// roll back serializer writes. Callers preparing a replacement must own the target.
	ENGINE_API auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		const FPackageLoadBindings& Bindings,
		uint32 SourceVersion,
		std::span<const FArchiveCustomVersion> CustomVersions = {},
		const FArchiveState& Context = {}) -> FAssetResult;
}

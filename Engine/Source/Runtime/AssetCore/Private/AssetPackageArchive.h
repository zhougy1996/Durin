#pragma once

#include "Asset/Catalog.h"
#include "Asset/Result.h"
#include "DObject/Archive.h"
#include "DObject/DObjectGlobals.h"

namespace Durin::Asset::Private
{
	struct FAuthoredPackageFieldRecord
	{
		std::string DeclaringClass;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		std::vector<std::byte> Payload;
	};

	struct FAuthoredPackageSummary
	{
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		std::vector<FAssetPath> Dependencies;
	};

	auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		const FAssetPath& PackagePath,
		uint32 SourceVersion,
		std::span<const FArchiveCustomVersion> CustomVersions = {}) -> FAssetResult;
}

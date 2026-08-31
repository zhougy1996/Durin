#pragma once

#include "AssetRegistry/Catalog.h"
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
		FByteArray Payload;
	};

	struct FAuthoredPackageSummary
	{
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FPackagePath RedirectDestination;
		std::vector<FPackagePath> Dependencies;
	};

	auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		const FPackagePath& PackagePath,
		uint32 SourceVersion,
		std::span<const FArchiveCustomVersion> CustomVersions = {},
		const FArchiveState& Context = {}) -> FAssetResult;
}

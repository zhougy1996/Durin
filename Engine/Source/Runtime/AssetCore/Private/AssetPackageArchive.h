#pragma once

#include "AssetSystemInternal.h"
#include "DObject/Archive.h"

namespace Durin::Asset::Private
{
	struct FAuthoredPackageFieldRecord
	{
		std::string DeclaringClass;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		std::vector<uint8> Payload;
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
		uint32 SourceVersion,
		std::vector<FAssetLegacyField>& OutLegacyFields,
		std::span<const FArchiveCustomVersion> CustomVersions = {}) -> FAssetResult;
}

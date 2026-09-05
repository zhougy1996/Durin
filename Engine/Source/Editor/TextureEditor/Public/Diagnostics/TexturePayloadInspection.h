#pragma once

#include "Asset/PackageInspection.h"
#include "Misc/Guid.h"
#include "TextureEditorAPI.h"

namespace Durin
{
	class DTexture2D;
	class DTextureCube;
	class DVolumeTexture;

	enum class ETexturePayloadStage : uint8
	{
		Source,
		DerivedData,
		Cooked,
		Decoded,
		RuntimeResource
	};

	enum class ETexturePayloadState : uint8
	{
		Unknown,
		NotPresent,
		Available,
		Missing,
		Stale,
		Corrupt,
		Unsupported,
		Failed
	};

	enum class ETexturePayloadRepairAction : uint8
	{
		None,
		ReimportSource,
		RebuildDerivedData,
		RestoreEditorCompanion,
		Recook,
		RetryRuntimeResource,
		RemoveOrphan,
		UpgradeOrResave
	};

	// One domain-qualified lifecycle fact. Placement is descriptive and never a
	// backend path or a cross-authority provider key.
	struct FTexturePayloadInspectionEntry
	{
		std::string Domain;
		ETexturePayloadStage Stage = ETexturePayloadStage::Source;
		ETexturePayloadState State = ETexturePayloadState::Unknown;
		ETexturePayloadRepairAction Repair = ETexturePayloadRepairAction::None;
		uint32 DomainSchemaVersion = 0;
		uint64 LogicalElementCount = 0;
		uint64 LogicalByteCount = 0;
		uint64 StoredByteCount = 0;
		FGuid PayloadId;
		std::string Placement;
		std::string Provenance;
		std::string Diagnostic;
	};

	struct FTexturePayloadInspection
	{
		bool bConstructFree = false;
		std::vector<FTexturePayloadInspectionEntry> Entries;
	};

	// Reads reflected package fields only. It never constructs an asset object,
	// opens a source file/DDC record, or mutates package/companion state.
	TEXTUREEDITOR_API auto InspectTexturePayloadPackage(
		const FAssetPackageInspection& Package,
		FTexturePayloadInspection& OutInspection,
		std::string* OutError = nullptr) -> bool;

	// Joins live source, derived, cooked, decoded, and render-resource state.
	// These overloads are read-only and do not perform repair or fallback.
	TEXTUREEDITOR_API auto InspectTexturePayloads(const DTexture2D& Texture)
		-> FTexturePayloadInspection;
	TEXTUREEDITOR_API auto InspectTexturePayloads(const DTextureCube& Texture)
		-> FTexturePayloadInspection;
	TEXTUREEDITOR_API auto InspectTexturePayloads(const DVolumeTexture& Texture)
		-> FTexturePayloadInspection;
}

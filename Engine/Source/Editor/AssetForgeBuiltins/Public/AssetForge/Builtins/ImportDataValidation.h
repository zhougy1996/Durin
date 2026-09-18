#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/AssetImportData.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::AssetForge::Builtins
{
	enum class EImportDataFamily : uint8 { StaticMesh, VolumeTexture };
	enum class EImportDataValidationError : uint8 { InvalidSourceRole, InvalidAxisSettings, InvalidAtlas };
	struct FImportDataValidationCause final : IAssetImportDataCause
	{
		EImportDataValidationError Code = EImportDataValidationError::InvalidSourceRole;
		EImportDataFamily Family = EImportDataFamily::StaticMesh;
		uint64 SourceCount = 0;
		std::vector<std::string> SourceRoles;
		std::optional<FStaticMeshImportSettingsError> SettingsCause;
		uint32 SliceWidth = 0;
		uint32 SliceHeight = 0;
		uint32 Depth = 0;
		uint32 TilesX = 0;
		uint32 TilesY = 0;
		ASSETFORGEBUILTINS_API auto Format() const -> std::string override;
	};
}

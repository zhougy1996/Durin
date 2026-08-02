#pragma once

#include "Asset/SourcePath.h"
#include "DObject/CoreDObject.h"
#include "DObject/AssetPath.h"
#include "EngineAPI.h"
#include "Math/Vector.h"
#include "StandardAssetImportAPI.h"

#include "LegacySceneImport.gen.h"

namespace Durin
{
	inline constexpr uint32 StaticModelImportManifestVersion = 1;
	class DMaterialInstance;
	class DTexture2D;

	// Editor-only compatibility schema for the retired StaticMesh-owned Scene
	// relationship. New runtime assets never persist these fields.
	DSTRUCT()
	struct FStaticModelImportDependencyRecord
	{
		GENERATED_BODY()
		DPROPERTY()
		uint8 Role = 0;
		DPROPERTY()
		std::string StableIdentity;
		DPROPERTY()
		FSourcePath SourcePath;
		DPROPERTY()
		std::string ContentHash;
		DPROPERTY()
		uint64 ByteCount = 0;
	};

	DSTRUCT()
	struct FStaticModelImportMaterialRecord
	{
		GENERATED_BODY()
		DPROPERTY()
		FGuid SlotId;
		DPROPERTY()
		uint32 SourceMaterialIndex = 0;
		DPROPERTY()
		std::string SourceName;
		DPROPERTY()
		FVector4 BaseColorFactor{1.0};
		DPROPERTY()
		FAssetPath GeneratedMaterialPath;
		DPROPERTY()
		bool bImporterManaged = true;
		DPROPERTY()
		TObjectPtr<DMaterialInstance> GeneratedMaterial;
	};

	DSTRUCT()
	struct FStaticModelImportTextureRecord
	{
		GENERATED_BODY()
		DPROPERTY()
		std::string StableIdentity;
		DPROPERTY()
		uint8 Semantic = 0;
		DPROPERTY()
		FAssetPath GeneratedTexturePath;
		DPROPERTY()
		TObjectPtr<DTexture2D> GeneratedTexture;
	};

	DSTRUCT()
	struct FStaticModelImportManifest
	{
		GENERATED_BODY()
		DPROPERTY()
		uint32 Version = 0;
		DPROPERTY()
		std::string DependencyFingerprint;
		DPROPERTY()
		uint32 ImporterVersion = 0;
		DPROPERTY()
		uint32 MaterialMapperVersion = 0;
		DPROPERTY()
		std::vector<FStaticModelImportDependencyRecord> Dependencies;
		DPROPERTY()
		std::vector<FStaticModelImportMaterialRecord> Materials;
		DPROPERTY()
		std::vector<FStaticModelImportTextureRecord> Textures;
		DPROPERTY()
		std::vector<std::string> Warnings;

		auto IsValid() const -> bool
		{
			return Version == StaticModelImportManifestVersion
				&& !DependencyFingerprint.empty()
				&& ImporterVersion > 0 && MaterialMapperVersion > 0;
		}
	};
}

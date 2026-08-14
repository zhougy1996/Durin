#pragma once

#include "StandardAssetImportAPI.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "Texture/Texture2DPostLoad.h"
#include "Texture/TextureCubePostLoad.h"

namespace Durin::Asset::Import::Standard
{
	class FStandardAssetAuthoringFeatures final
		: public IStaticMeshAuthoringFeature
		, public ITexture2DAuthoringFeature
		, public ITextureCubeAuthoringFeature
	{
	public:
		STANDARDASSETIMPORT_API auto BuildFileProduct(
			DStaticMesh& Mesh,
			std::string_view SourcePath,
			FStaticMeshSourceImportData SourceImportData,
			std::string_view SourceContentHash,
			FStaticMeshAuthoringProduct& OutProduct,
			std::string& OutError) -> bool override;
		STANDARDASSETIMPORT_API auto PostLoadUncooked(
			DStaticMesh& Mesh,
			FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
			std::string& OutError) -> bool override;
		STANDARDASSETIMPORT_API auto ChangeSourceReference(
			DStaticMesh& Mesh,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool override;
		STANDARDASSETIMPORT_API auto PostLoadUncooked(
			DTexture2D& Texture, std::string& OutError) -> bool override;
		STANDARDASSETIMPORT_API auto PostLoadUncooked(
			DTextureCube& Texture, std::string& OutError) -> bool override;
	};
}

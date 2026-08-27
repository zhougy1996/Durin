#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	enum class EStaticMeshBuildFailureStage : uint8
	{
		None,
		Request,
		RenderConversion,
		Key,
		DerivedDataWrite
	};

	struct FStaticMeshBuildProduct
	{
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		FStaticMeshImportedData ImportedData;
		FStaticMeshSourceImportData SourceImportData;
		float NormalizedSize = 1.5f;
		std::string DerivedDataKey;
		bool bSlotMetadataChanged = false;
		EStaticMeshDerivedDataStatus DerivedDataStatus =
			EStaticMeshDerivedDataStatus::Rebuilt;
		std::string DiagnosticMessage =
			"Built imported StaticMesh and populated the DDC.";
		bool bSourceImporterInvoked = true;
		bool bMarkPackageDirty = true;
		bool bContainsImportedData = false;
		EStaticMeshBuildFailureStage FailureStage =
			EStaticMeshBuildFailureStage::None;
	};

	struct FStaticMeshCollisionBuildProduct
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		EBodySetupCollisionBuildStatus Status =
			EBodySetupCollisionBuildStatus::None;
		std::string DerivedDataKey;
		std::string Diagnostic;
		uint64 PayloadBytes = 0;
	};

	class IStaticMeshCollisionBuildFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.StaticMeshCollisionBuild";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto BuildCollisionProduct(
			const FStaticMeshRenderData& RenderData,
			const FStaticMeshSourceImportData& SourceImportData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FStaticMeshCollisionBuildProduct& OutProduct,
			std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeStaticMeshCollisionBuildFeature(
		const FStaticMeshRenderData& RenderData,
		const FStaticMeshSourceImportData& SourceImportData,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FStaticMeshCollisionBuildProduct& OutProduct,
		std::string& OutError) -> bool;
}

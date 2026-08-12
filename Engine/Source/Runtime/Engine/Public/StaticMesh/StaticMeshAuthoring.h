#pragma once

#include "EngineAPI.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	enum class EStaticMeshAuthoringFailureStage : uint8
	{
		None,
		Request,
		RenderConversion,
		Key,
		DerivedDataWrite
	};

	struct FStaticMeshAuthoringProduct
	{
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		std::vector<FStaticMeshMaterialSlotDefinition> MaterialSlots;
		FStaticMeshSourceImportData SourceImportData;
		std::string DerivedDataKey;
		bool bSlotMetadataChanged = false;
		EStaticMeshDerivedDataStatus DerivedDataStatus =
			EStaticMeshDerivedDataStatus::Rebuilt;
		std::string DiagnosticMessage =
			"Built imported StaticMesh and populated the DDC.";
		bool bSourceImporterInvoked = true;
		bool bMarkPackageDirty = true;
		EStaticMeshAuthoringFailureStage FailureStage =
			EStaticMeshAuthoringFailureStage::None;
	};

	struct FStaticMeshCollisionAuthoringProduct
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		EBodySetupCollisionBuildStatus Status =
			EBodySetupCollisionBuildStatus::None;
		std::string DerivedDataKey;
		std::string Diagnostic;
		uint64 PayloadBytes = 0;
	};

	using FStaticMeshFileBuildHandler = std::function<bool(
		DStaticMesh&,
		std::string_view,
		FStaticMeshSourceImportData,
		std::string_view,
		FStaticMeshAuthoringProduct&,
		std::string&)>;
	using FStaticMeshPostLoadHandler = std::function<bool(
		DStaticMesh&, FStaticMeshDerivedDataDiagnostic&, std::string&)>;
	using FStaticMeshSourceChangeHandler = std::function<bool(
		DStaticMesh&, std::string_view, std::string&)>;
	using FStaticMeshCollisionBuildHandler = std::function<bool(
		const FStaticMeshRenderData&,
		const FStaticMeshSourceImportData&,
		EBodySetupCollisionSourceMode,
		EBodySetupCollisionQueryPolicy,
		FStaticMeshCollisionAuthoringProduct&,
		std::string&)>;

	struct FStaticMeshAuthoringHandlers
	{
		FStaticMeshFileBuildHandler BuildFileProduct;
		FStaticMeshPostLoadHandler PostLoadUncooked;
		FStaticMeshSourceChangeHandler ChangeSourceReference;
		FStaticMeshCollisionBuildHandler BuildCollisionProduct;
	};

	ENGINE_API auto RegisterStaticMeshAuthoringHandlers(
		FStaticMeshAuthoringHandlers Handlers) -> bool;
	ENGINE_API auto UnregisterStaticMeshAuthoringHandlers() -> void;
	ENGINE_API auto RegisterStaticMeshCollisionBuildHandler(
		FStaticMeshCollisionBuildHandler Handler) -> bool;
	ENGINE_API auto UnregisterStaticMeshCollisionBuildHandler() -> void;
	ENGINE_API auto InvokeStaticMeshSourceChangeHandler(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
	ENGINE_API auto GetStaticMeshAuthoringHandlers() -> FStaticMeshAuthoringHandlers;
}

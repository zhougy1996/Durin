#pragma once

#include "DObject/Package.h"
#include "AssetForgeBuiltinsAssetFeatures.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"

#include "Animation/AnimationClip.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "AssetForge/Builtins/ImportSupport.h"
#include "DObject/ObjectLifecycle.h"
#include "EncodedSourceSnapshot.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialInstance.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "SceneImportInternal.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{

		inline constexpr uint32 StaticMeshAssimpImporterVersion = 3;
		inline constexpr std::string_view StaticMeshImporterId = "Assimp";
		auto ImportAxisVector(EStaticMeshImportAxis Axis) -> FVector3f
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: return {1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeX: return {-1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveY: return {0.0f, 1.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeY: return {0.0f, -1.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveZ: return {0.0f, 0.0f, 1.0f};
			case EStaticMeshImportAxis::NegativeZ: return {0.0f, 0.0f, -1.0f};
			}
			return {};
		}

		auto MakeMeshImportOptions(
			const FStaticMeshImportSettings& Settings,
			const FSourcePath& RootSource) -> FMeshImportOptions
		{
			const FVector3f Forward = ImportAxisVector(Settings.ForwardAxis);
			const FVector3f Right = ImportAxisVector(Settings.RightAxis);
			const FVector3f Up = ImportAxisVector(Settings.UpAxis);
			FMeshImportOptions Options;
			for (uint32 Component = 0; Component < 3; ++Component)
			{
				Options.SourceToEngine[Component][0] = Forward[Component];
				Options.SourceToEngine[Component][1] = Right[Component];
				Options.SourceToEngine[Component][2] = Up[Component];
			}
			Options.RootSource = RootSource;
			return Options;
		}

		auto DecodeStaticMeshSource(
			std::string_view FilePath,
			const FStaticMeshImportSettings& Settings,
			Asset::FStaticMeshImportedData& OutData,
			std::string& OutError) -> bool
		{
			FImportedSceneData Scene;
			if (ImportFromFile(
				FilePath, Scene, MakeMeshImportOptions(Settings, {})))
			{
				OutData = MakeStaticMeshImportedData(Scene);
				OutError.clear();
				return true;
			}
			OutError = std::format("Failed to decode StaticMesh source {}.", FilePath);
			return false;
		}

			auto BuildStaticMeshFileProduct(
			DStaticMesh& Mesh,
			std::string_view FilePath,
			FStaticMeshSourceImportData SourceImportData,
			std::string_view SourceLabel,
			FStaticMeshBuildProduct& OutProduct,
			std::string& OutError) -> bool
		{
			Asset::FStaticMeshImportedData ImportedData;
			if (!DecodeStaticMeshSource(
				FilePath, SourceImportData.ImportSettings, ImportedData, OutError))
				return false;
			return Asset::FStaticMeshBuildOperations::BuildImportedProduct(
				Asset::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(Mesh),
				ImportedData, std::move(SourceImportData), SourceLabel,
				OutProduct, OutError);
		}

		auto PostLoadStaticMesh(
			DStaticMesh& Mesh,
			FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
			std::string& OutError) -> bool
		{
			const FStaticMeshSourceDiagnostic SourceDiagnostic =
				InspectStaticMeshSource(Mesh);
			if (SourceDiagnostic.Status == EStaticMeshSourceStatus::NoSource)
			{
				OutDiagnostic = {};
				OutError.clear();
				return true;
			}
			if (Mesh.GetMaterialSlots().empty())
			{
				OutError = "StaticMesh with source metadata must contain a material slot.";
				return false;
			}

			const auto* ImportData = dynamic_cast<const DStaticMeshImportData*>(
				Mesh.GetAssetImportData());
			if (!ImportData)
			{
				OutError = "StaticMesh has no current family import data.";
				return false;
			}
			const FStaticMeshImportDataState ImportState =
				ImportData->GetStaticMeshState();
			const AssetImport::FSourceFile* ImportedSource =
				ImportState.SourceData.FindByRole("source");
			if (!ImportedSource)
			{
				OutError = "StaticMesh family import data has no source.";
				return false;
			}
			FStaticMeshSourceImportData Source{
				.SourcePath = {.Path = ImportedSource->Filename},
				.SourceContentHash = FXxHash128{
					.HashLow = ImportedSource->ContentHashLow,
					.HashHigh = ImportedSource->ContentHashHigh}.ToString(),
				.ImporterId = ImportState.ImporterId,
				.ImporterVersion = ImportState.ImporterVersion,
				.ImportSettings = ImportState.ImportSettings};
			const bool bSourceAvailable = SourceDiagnostic.IsAvailable();
			if (bSourceAvailable)
			{
				std::vector<std::byte> Bytes;
				if (!FFileHelper::LoadFileToArray(Bytes, SourceDiagnostic.ResolvedPath))
				{
					OutDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
					OutError = std::format(
						"Failed to read StaticMesh source file: {}",
						SourceDiagnostic.ResolvedPath);
					OutDiagnostic.Message = OutError;
					return false;
				}
				Source.SourceContentHash = FXxHash128::HashBuffer(Bytes).ToString();
			}
			const bool bSourceHashValid = Source.SourceContentHash.size() == 32
				&& std::ranges::all_of(Source.SourceContentHash, [](char Character) {
					return Character >= '0' && Character <= '9'
						|| Character >= 'a' && Character <= 'f';
				});
			if (!bSourceHashValid)
			{
				OutDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
				OutError = SourceDiagnostic.Message.empty()
					? "StaticMesh source hash is unavailable."
					: SourceDiagnostic.Message;
				OutDiagnostic.Message = OutError;
				return false;
			}

			const bool bSourceMetadataStale = bSourceAvailable
				&& (ImportedSource->ContentHashLow != FXxHash128::FromString(
						Source.SourceContentHash).HashLow
					|| ImportedSource->ContentHashHigh != FXxHash128::FromString(
						Source.SourceContentHash).HashHigh);
			FStaticMeshBuildProduct Product;
			EStaticMeshDerivedDataStatus CacheStatus =
				EStaticMeshDerivedDataStatus::Missing;
			std::string CacheMessage;
			if (!bSourceMetadataStale
				&& Asset::FStaticMeshBuildOperations::LoadDerivedDataProduct(
					Asset::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(Mesh),
					Source, bSourceAvailable, Product, CacheStatus,
					CacheMessage, OutError))
			{
				return Mesh.PublishImportedProduct(std::move(Product), OutError);
			}
			if (!bSourceAvailable)
			{
				OutDiagnostic.Status = EStaticMeshDerivedDataStatus::SourceUnavailable;
				OutDiagnostic.Message = std::format(
					"{}. Cached payload was unavailable: {} Reimport and cache regeneration are unavailable.",
					SourceDiagnostic.Message, CacheMessage);
				OutError = OutDiagnostic.Message;
				return false;
			}
			if (bSourceMetadataStale)
			{
				OutError = "StaticMesh source changed; explicit reimport is required before derived-data recovery.";
				return false;
			}
			if (!BuildStaticMeshFileProduct(Mesh, SourceDiagnostic.ResolvedPath,
				Source, Source.SourcePath.Path, Product, OutError)) return false;
			Product.bMarkPackageDirty = false;
			if (!Mesh.PublishImportedProduct(std::move(Product), OutError)) return false;
			OutError.clear();
			return true;
		}
		}
}

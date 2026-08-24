#pragma once

#include "AssetForge/ImportService.h"
#include "AssetForge/Persistence/ImportRecord.h"
#include "DObject/ObjectLifecycle.h"
#include "Animation/AnimationClip.h"
#include "Materials/MaterialInstance.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "StaticMesh/StaticMesh.h"
#include "Terrain/TerrainHeightmap.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto MakeCandidatePath(const FAssetPath& TargetPath, FAssetPath& OutPath) -> bool
		{
			for (uint32 Suffix = 1; Suffix != 0; ++Suffix)
			{
				if (!FAssetPath::TryCreate(std::format("{}_ImportCandidate_{}",
					TargetPath.ToString(), Suffix), OutPath)) return false;
				if (!Asset::FindResidentPackage(OutPath)
					&& !Asset::FindAssetExact(OutPath)) return true;
			}
			return false;
		}

		class FBuiltinSingleAssetCandidate final : public ISingleAssetCandidate
		{
		public:
			explicit FBuiltinSingleAssetCandidate(
				DObject* InAsset, bool bInNewAsset = false)
				: AssetObject(InAsset),
				Package(InAsset ? InAsset->GetPackage() : nullptr),
				bNewAsset(bInNewAsset) {}

			auto GetAsset() const -> DObject* override { return AssetObject; }
			auto GetPackage() const -> DPackage* override { return Package; }
			auto IsNewAsset() const -> bool override { return bNewAsset; }

			auto GetAuthoredFingerprint() const -> std::string override
			{
				if (const auto* Mesh = Cast<DStaticMesh>(AssetObject))
					return Mesh->GetSourceImportData().SourceContentHash;
				if (const auto* Texture = Cast<DTexture2D>(AssetObject))
					return Texture->GetDerivedDataKey();
				if (const auto* Volume = Cast<DVolumeTexture>(AssetObject))
					return Volume->GetDerivedDataKey();
				if (const auto* Cube = Cast<DTextureCube>(AssetObject))
					return Cube->GetDerivedDataKey();
				if (const auto* Heightmap = Cast<DTerrainHeightmap>(AssetObject))
					return Heightmap->GetDerivedDataKey();
				return {};
			}

			auto Validate(std::vector<FImportDiagnostic>& OutDiagnostics) const
				-> bool override
			{
				bool bValid = false;
				if (const auto* Mesh = Cast<DStaticMesh>(AssetObject))
					bValid = Mesh->GetRenderData() != nullptr;
				else if (const auto* Texture = Cast<DTexture2D>(AssetObject))
					bValid = Texture->GetPlatformData() != nullptr
						&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready;
				else if (const auto* Volume = Cast<DVolumeTexture>(AssetObject))
					bValid = Volume->GetPlatformData() != nullptr
						&& Volume->GetBuildStatus() == ETextureBuildStatus::Ready;
				else if (const auto* Cube = Cast<DTextureCube>(AssetObject))
					bValid = Cube->GetPlatformData() != nullptr
						&& Cube->GetBuildStatus() == ETextureBuildStatus::Ready;
				else if (const auto* Heightmap = Cast<DTerrainHeightmap>(AssetObject))
					bValid = Heightmap->GetPayload() != nullptr
						&& Heightmap->GetStatus() == ETerrainHeightmapStatus::Ready;
				else if (const auto* Skeleton = Cast<DSkeleton>(AssetObject))
				{
					std::string Error;
					bValid = Skeleton->Validate(Error);
				}
				else if (const auto* Mesh = Cast<DSkeletalMesh>(AssetObject))
				{
					std::string Error;
					bValid = Mesh->Validate(Error);
				}
				else if (const auto* Clip = Cast<DAnimationClip>(AssetObject))
				{
					std::string Error;
					bValid = Clip->Validate(Error);
				}
				else if (const auto* Record = Cast<DImportRecord>(AssetObject))
				{
					std::string Error;
					bValid = Record->Validate(Error);
				}
				else if (Cast<DMaterialInstance>(AssetObject)) bValid = true;
				if (!bValid)
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::ValidationFailure,
						.Phase = "candidate-validation",
						.Message = "Engine asset candidate has no validated runtime data."});
				return bValid;
			}

			auto Abandon() noexcept -> void override
			{
				if (DPackage* Detached = DetachPackageForAbandon())
					(void)Asset::UnloadPackage(Detached,
						Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			}

			auto DetachPackageForAbandon() noexcept -> DPackage* override
			{
				DPackage* Detached = Package;
				Package = nullptr;
				AssetObject = nullptr;
				return Detached;
			}

		private:
			DObject* AssetObject = nullptr;
			DPackage* Package = nullptr;
			bool bNewAsset = false;
		};

		template<typename T>
		class TImportedStateExchange final : public IPreparedImportedStateExchange
		{
		public:
			TImportedStateExchange(T& InTarget, T& InCandidate)
				: Target(&InTarget), Candidate(&InCandidate) {}

			auto Commit() noexcept -> void override
			{
				if (!bCommitted)
				{
					Target->ExchangeImportedState(*Candidate);
					bCommitted = true;
				}
			}

			auto Reverse() noexcept -> void override
			{
				if (bCommitted)
				{
					Target->ExchangeImportedState(*Candidate);
					bCommitted = false;
				}
			}

			auto Finalize() noexcept -> void override
			{
				Target = nullptr;
				Candidate = nullptr;
			}

		private:
			T* Target = nullptr;
			T* Candidate = nullptr;
			bool bCommitted = false;
		};

	}
}

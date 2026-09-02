#include "Asset/Cook.h"

#include "Animation/AnimationClip.h"
#include "Asset/AssetCompilingManager.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "Materials/Material.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "StaticMesh/StaticMesh.h"
#include "Terrain/TerrainHeightmap.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace Durin
{
	namespace
	{
		template<typename T>
		auto RegisterFamily(
			std::string Name,
			std::vector<FCookContributorHandle>& Handles) -> bool
		{
			const FCookContributorHandle Handle = RegisterCookContributor(
				T::StaticClass(), {std::move(Name), 1, 1,
					[](DObject& Object, std::string_view VirtualPath,
						FCookContext& Context) -> FAssetResult {
						if (!Object.IsA(T::StaticClass()))
							return {EAssetError::TypeMismatch,
								"Cook contributor received an incompatible object."};
						(void)FAssetCompilingManager::Get()
							.FinishCompilationForObject(Object);
						std::string Error;
						if (!ContributeEngineCookAsset(
								Object, VirtualPath, Context, Error))
							return {EAssetError::UnsupportedProperty, std::move(Error)};
						return {};
					},
					[](const DObject&) -> ECookPackageStatus {
						return ECookPackageStatus::Captured;
					}}
			);
			if (Handle == 0) return false;
			Handles.push_back(Handle);
			return true;
		}
	}

	auto ContributeEngineCookAsset(
		DObject& Object,
		std::string_view VirtualPackagePath,
		FCookContext& Context,
		std::string& OutError) -> bool
	{
		if (Object.IsA(DTexture2D::StaticClass()))
			return static_cast<DTexture2D&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DTextureCube::StaticClass()))
			return static_cast<DTextureCube&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DVolumeTexture::StaticClass()))
			return static_cast<DVolumeTexture&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DStaticMesh::StaticClass()))
			return static_cast<DStaticMesh&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DSkeletalMesh::StaticClass()))
			return static_cast<DSkeletalMesh&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DSkeleton::StaticClass()))
			return static_cast<DSkeleton&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DAnimationClip::StaticClass()))
			return static_cast<DAnimationClip&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DTerrainHeightmap::StaticClass()))
			return static_cast<DTerrainHeightmap&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DMaterial::StaticClass()))
			return static_cast<DMaterial&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		if (Object.IsA(DEnvironmentLighting::StaticClass()))
			return static_cast<DEnvironmentLighting&>(Object).ContributeToCook(
				Context, VirtualPackagePath, OutError);
		OutError = "No Engine family Cook contribution exists for the object class.";
		return false;
	}

	auto RegisterEngineCookContributors(
		std::vector<FCookContributorHandle>& OutHandles,
		std::string& OutError) -> bool
	{
		const size_t FirstNewHandle = OutHandles.size();
		const bool bRegistered =
			RegisterFamily<DTexture2D>("texture2d", OutHandles)
			&& RegisterFamily<DTextureCube>("texture-cube", OutHandles)
			&& RegisterFamily<DVolumeTexture>("volume-texture", OutHandles)
			&& RegisterFamily<DStaticMesh>("static-mesh", OutHandles)
			&& RegisterFamily<DSkeletalMesh>("skeletal-mesh", OutHandles)
			&& RegisterFamily<DSkeleton>("skeleton", OutHandles)
			&& RegisterFamily<DAnimationClip>("animation-clip", OutHandles)
			&& RegisterFamily<DTerrainHeightmap>("terrain-heightmap", OutHandles)
			&& RegisterFamily<DMaterial>("material", OutHandles)
			&& RegisterFamily<DEnvironmentLighting>(
				"environment-lighting", OutHandles);
		if (bRegistered)
		{
			OutError.clear();
			return true;
		}
		for (size_t Index = FirstNewHandle; Index < OutHandles.size(); ++Index)
			UnregisterCookContributor(OutHandles[Index]);
		OutHandles.resize(FirstNewHandle);
		OutError = "CookContributorRegistrationFailed: an Engine class has a duplicate or invalid contributor.";
		return false;
	}
}

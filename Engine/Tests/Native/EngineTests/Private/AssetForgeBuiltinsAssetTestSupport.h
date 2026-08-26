#pragma once

#include "Modules/ModuleTestSupport.h"
#include "AssetForgeBuiltinsAssetFeatures.h"
#include "TerrainHeightmapAssetFeatures.h"

namespace Durin::Tests
{
	struct FAssetForgeBuiltinsAssetTestState
	{
		FAssetForgeBuiltinsAssetTestState()
		{
			StaticMeshBuild = Context.RegisterFeature<IStaticMeshBuildFeature>(Features);
			StaticMeshPostLoad = Context.RegisterFeature<IStaticMeshPostLoadFeature>(Features);
			StaticMeshSourceMutation = Context.RegisterFeature<IStaticMeshSourceMutationFeature>(Features);
			Texture2D = Context.RegisterFeature<ITexture2DPostLoadFeature>(Features);
			Texture2DRecovery = Context.RegisterFeature<
				ITexture2DImportRecoveryFeature>(Features);
			TextureCube = Context.RegisterFeature<ITextureCubePostLoadFeature>(Features);
			TerrainDerivedDataLoad = Context.RegisterFeature<
				ITerrainHeightmapDerivedDataLoadFeature>(TerrainFeatures);
			TerrainSourceMutation = Context.RegisterFeature<
				ITerrainHeightmapSourceMutationFeature>(TerrainFeatures);
		}

		FModuleTestOwner Context{"EngineTests.AssetForgeBuiltinsAssets"};
		AssetForge::Builtins::FAssetForgeBuiltinsAssetFeatures Features;
		AssetForge::Builtins::FTerrainHeightmapAssetFeatures TerrainFeatures;
		FModularFeatureRegistration StaticMeshBuild;
		FModularFeatureRegistration StaticMeshPostLoad;
		FModularFeatureRegistration StaticMeshSourceMutation;
		FModularFeatureRegistration Texture2D;
		FModularFeatureRegistration Texture2DRecovery;
		FModularFeatureRegistration TextureCube;
		FModularFeatureRegistration TerrainDerivedDataLoad;
		FModularFeatureRegistration TerrainSourceMutation;
	};

	inline auto GetAssetForgeBuiltinsAssetTestState() -> FAssetForgeBuiltinsAssetTestState&
	{
		static FAssetForgeBuiltinsAssetTestState State;
		return State;
	}

	inline auto InstallAssetForgeBuiltinsAssetFeatures() -> bool
	{
		auto& State = GetAssetForgeBuiltinsAssetTestState();
		return State.StaticMeshBuild.IsValid()
			&& State.StaticMeshPostLoad.IsValid()
			&& State.StaticMeshSourceMutation.IsValid()
			&& State.Texture2D.IsValid()
			&& State.Texture2DRecovery.IsValid()
			&& State.TextureCube.IsValid()
			&& State.TerrainDerivedDataLoad.IsValid()
			&& State.TerrainSourceMutation.IsValid();
	}

	inline auto EnsureTerrainDerivedDataOperationGroup() -> bool
	{
		auto& State = GetAssetForgeBuiltinsAssetTestState();
		return State.TerrainFeatures.SetOperationGroup(
			State.Context.CreateAsyncOperationGroup("TerrainDerivedDataLoads"));
	}
}

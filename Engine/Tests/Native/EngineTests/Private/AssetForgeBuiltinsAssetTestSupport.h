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
			Texture2D = Context.RegisterFeature<ITexture2DPostLoadFeature>(Features);
			TextureCube = Context.RegisterFeature<ITextureCubePostLoadFeature>(Features);
			TerrainDerivedDataLoad = Context.RegisterFeature<
				ITerrainHeightmapDerivedDataLoadFeature>(TerrainFeatures);
		}

		FModuleTestOwner Context{"EngineTests.AssetForgeBuiltinsAssets"};
		AssetForge::Builtins::FAssetForgeBuiltinsAssetFeatures Features;
		AssetForge::Builtins::FTerrainHeightmapAssetFeatures TerrainFeatures;
		FModularFeatureRegistration StaticMeshBuild;
		FModularFeatureRegistration StaticMeshPostLoad;
		FModularFeatureRegistration Texture2D;
		FModularFeatureRegistration TextureCube;
		FModularFeatureRegistration TerrainDerivedDataLoad;
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
			&& State.Texture2D.IsValid()
			&& State.TextureCube.IsValid()
			&& State.TerrainDerivedDataLoad.IsValid();
	}

	inline auto EnsureTerrainDerivedDataOperationGroup() -> bool
	{
		auto& State = GetAssetForgeBuiltinsAssetTestState();
		return State.TerrainFeatures.SetOperationGroup(
			State.Context.CreateAsyncOperationGroup("TerrainDerivedDataLoads"));
	}
}

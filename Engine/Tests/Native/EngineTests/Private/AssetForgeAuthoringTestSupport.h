#pragma once

#include "Modules/ModuleTestSupport.h"
#include "AssetForgeAuthoringFeatures.h"
#include "TerrainAuthoringFeature.h"

namespace Durin::Tests
{
	struct FAssetForgeAuthoringTestState
	{
		FAssetForgeAuthoringTestState()
		{
			StaticMesh = Context.RegisterFeature<IStaticMeshAuthoringFeature>(Features);
			Texture2D = Context.RegisterFeature<ITexture2DAuthoringFeature>(Features);
			TextureCube = Context.RegisterFeature<ITextureCubeAuthoringFeature>(Features);
			Terrain = Context.RegisterFeature<ITerrainHeightmapAuthoringFeature>(TerrainFeatures);
		}

		FModuleTestOwner Context{"EngineTests.AssetForgeAuthoring"};
		Asset::Forge::FAssetForgeAuthoringFeatures Features;
		Asset::Forge::FTerrainAuthoringFeature TerrainFeatures;
		FModularFeatureRegistration StaticMesh;
		FModularFeatureRegistration Texture2D;
		FModularFeatureRegistration TextureCube;
		FModularFeatureRegistration Terrain;
	};

	inline auto GetAssetForgeAuthoringTestState() -> FAssetForgeAuthoringTestState&
	{
		static FAssetForgeAuthoringTestState State;
		return State;
	}

	inline auto InstallAssetForgeAuthoringFeatures() -> bool
	{
		auto& State = GetAssetForgeAuthoringTestState();
		return State.StaticMesh.IsValid() && State.Texture2D.IsValid()
			&& State.TextureCube.IsValid() && State.Terrain.IsValid();
	}

	inline auto EnsureTerrainAuthoringOperationGroup() -> bool
	{
		auto& State = GetAssetForgeAuthoringTestState();
		return State.TerrainFeatures.SetOperationGroup(
			State.Context.CreateAsyncOperationGroup("TerrainAuthoringLoads"));
	}
}

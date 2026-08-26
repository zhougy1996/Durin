#pragma once

#include "Modules/ModuleTestSupport.h"
#include "AssetForgeBuiltinsAuthoringFeatures.h"
#include "TerrainAuthoringFeature.h"

namespace Durin::Tests
{
	struct FAssetForgeBuiltinsAuthoringTestState
	{
		FAssetForgeBuiltinsAuthoringTestState()
		{
			StaticMesh = Context.RegisterFeature<IStaticMeshAuthoringFeature>(Features);
			Texture2D = Context.RegisterFeature<ITexture2DPostLoadFeature>(Features);
			Texture2DRecovery = Context.RegisterFeature<
				ITexture2DImportRecoveryFeature>(Features);
			TextureCube = Context.RegisterFeature<ITextureCubeAuthoringFeature>(Features);
			Terrain = Context.RegisterFeature<ITerrainHeightmapAuthoringFeature>(TerrainFeatures);
		}

		FModuleTestOwner Context{"EngineTests.AssetForgeBuiltinsAuthoring"};
		AssetForge::Builtins::FAssetForgeBuiltinsAuthoringFeatures Features;
		AssetForge::Builtins::FTerrainAuthoringFeature TerrainFeatures;
		FModularFeatureRegistration StaticMesh;
		FModularFeatureRegistration Texture2D;
		FModularFeatureRegistration Texture2DRecovery;
		FModularFeatureRegistration TextureCube;
		FModularFeatureRegistration Terrain;
	};

	inline auto GetAssetForgeBuiltinsAuthoringTestState() -> FAssetForgeBuiltinsAuthoringTestState&
	{
		static FAssetForgeBuiltinsAuthoringTestState State;
		return State;
	}

	inline auto InstallAssetForgeBuiltinsAuthoringFeatures() -> bool
	{
		auto& State = GetAssetForgeBuiltinsAuthoringTestState();
		return State.StaticMesh.IsValid() && State.Texture2D.IsValid()
			&& State.Texture2DRecovery.IsValid()
			&& State.TextureCube.IsValid() && State.Terrain.IsValid();
	}

	inline auto EnsureTerrainAuthoringOperationGroup() -> bool
	{
		auto& State = GetAssetForgeBuiltinsAuthoringTestState();
		return State.TerrainFeatures.SetOperationGroup(
			State.Context.CreateAsyncOperationGroup("TerrainAuthoringLoads"));
	}
}

#pragma once

#include "Modules/ModuleTestContext.h"
#include "StandardAssetAuthoringFeatures.h"
#include "StandardTerrainAuthoringFeature.h"

namespace Durin::Tests
{
	struct FStandardAssetAuthoringTestState
	{
		FStandardAssetAuthoringTestState()
		{
			StaticMesh = Context.RegisterFeature<IStaticMeshAuthoringFeature>(Features);
			Texture2D = Context.RegisterFeature<ITexture2DAuthoringFeature>(Features);
			TextureCube = Context.RegisterFeature<ITextureCubeAuthoringFeature>(Features);
			Terrain = Context.RegisterFeature<ITerrainHeightmapAuthoringFeature>(TerrainFeatures);
		}

		FModuleContext Context = FModuleTestContextFactory::CreateStartupContext(
			"EngineTests.StandardAssetAuthoring");
		Asset::Import::Standard::FStandardAssetAuthoringFeatures Features;
		Asset::Import::Standard::FStandardTerrainAuthoringFeature TerrainFeatures;
		FModularFeatureRegistration StaticMesh;
		FModularFeatureRegistration Texture2D;
		FModularFeatureRegistration TextureCube;
		FModularFeatureRegistration Terrain;
	};

	inline auto GetStandardAssetAuthoringTestState() -> FStandardAssetAuthoringTestState&
	{
		static FStandardAssetAuthoringTestState State;
		return State;
	}

	inline auto InstallStandardAssetAuthoringFeatures() -> bool
	{
		auto& State = GetStandardAssetAuthoringTestState();
		return State.StaticMesh.IsValid() && State.Texture2D.IsValid()
			&& State.TextureCube.IsValid() && State.Terrain.IsValid();
	}

	inline auto EnsureStandardTerrainAuthoringOperationGroup() -> bool
	{
		auto& State = GetStandardAssetAuthoringTestState();
		return State.TerrainFeatures.SetOperationGroup(
			State.Context.CreateAsyncOperationGroup("TerrainAuthoringLoads"));
	}
}

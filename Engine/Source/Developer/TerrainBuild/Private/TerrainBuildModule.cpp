#include "TerrainBuildFunctionRegistry.h"
#include "Modules/ModuleManager.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin
{
	// Owns Terrain build-function registration for the loaded module generation.
	class FTerrainBuildModule final
		: public IModuleInterface
		, public ITerrainHeightmapDerivedDataLoadFeature
	{
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;
		FModularFeatureRegistration DerivedDataLoadRegistration;

		auto PostLoadUncooked(DTerrainHeightmap& Heightmap,
			std::string& OutError) -> bool override
		{
			if (!Heightmap.GetImportedData().IsValid())
			{
				OutError = "Terrain heightmap canonical imported data is missing or invalid.";
				return false;
			}
			std::string Key = Asset::MakeTerrainHeightmapDerivedDataKey(Heightmap, OutError);
			if (Key.empty()) return false;
			std::shared_ptr<const FTerrainHeightmapPayload> Payload;
			if (Asset::LoadTerrainHeightmapDerivedData(Key, Payload, OutError))
			{
				Heightmap.PublishDerivedDataLoadResult(std::move(Payload),
					std::move(Key), "Loaded terrain heightmap payload from DDC.",
					false, false, true);
				OutError.clear();
				return true;
			}
			Asset::FTerrainHeightmapBuildProduct Product;
			if (!Asset::BuildTerrainHeightmap({
				.Samples = Heightmap.GetImportedData().GetSamples(),
				.Width = Heightmap.GetImportedData().Width,
				.Height = Heightmap.GetImportedData().Height,
				.bQueryDerivedData = false}, Product, OutError)) return false;
			const std::string Diagnostic = Product.PersistenceDiagnostic.empty()
				? "Rebuilt terrain heightmap from canonical samples after DDC miss or corruption."
				: std::format(
					"Rebuilt terrain heightmap from canonical samples; DDC persistence was best effort: {}",
					Product.PersistenceDiagnostic);
			Heightmap.PublishDerivedDataLoadResult(std::move(Product.Payload),
				std::move(Product.DerivedDataKey), Diagnostic, false, false, false);
			OutError.clear();
			return true;
		}

		auto WaitForDerivedDataLoad(DTerrainHeightmap& Heightmap,
			std::string& OutError) -> bool override
		{
			if (Heightmap.GetStatus() == ETerrainHeightmapStatus::Ready)
			{
				OutError.clear();
				return true;
			}
			OutError = Heightmap.GetLastDiagnostic();
			return false;
		}

		auto StartupModule() -> void override
		{
			std::string Error;
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration("TerrainBuild.BuildFunctions");
			checkf(Asset::InitializeTerrainBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"TerrainBuild could not register its build functions: {}", Error);
			DerivedDataLoadRegistration = FModuleStartup::RegisterFeature<
				ITerrainHeightmapDerivedDataLoadFeature>(*this);
			checkf(DerivedDataLoadRegistration.IsValid(),
				"TerrainBuild could not register TerrainHeightmap post-load building.");
		}

		auto ShutdownModule() -> void override
		{
			Asset::ShutdownTerrainBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FTerrainBuildModule, TerrainBuild)
}

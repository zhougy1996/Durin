#include "TerrainBuildFunctionRegistry.h"

#include "Terrain/TerrainHeightmapBuildFunctions.h"
#include "Terrain/TerrainWorldBuildFunctions.h"

namespace Durin::Asset::Build
{
	namespace
	{
		std::mutex GTerrainBuildFunctionMutex;
		FBuildFunctionRegistration GTerrainHeightmapRegistration;
		std::array<FBuildFunctionRegistration, 5> GTerrainWorldRegistrations;
	}

	auto EnsureTerrainBuildFunctions(
		std::string* OutError, FModuleOwnedCallbackGate Gate) -> bool
	{
		std::lock_guard Lock(GTerrainBuildFunctionMutex);
		if (GTerrainHeightmapRegistration.IsValid()
			&& std::ranges::all_of(GTerrainWorldRegistrations,
				[](const FBuildFunctionRegistration& Registration) { return Registration.IsValid(); })) return true;

		const bool bAcquiredHeightmap = !GTerrainHeightmapRegistration.IsValid();
		std::array<bool, 5> AcquiredTerrainWorld{};
		for (size_t Index = 0; Index < AcquiredTerrainWorld.size(); ++Index)
			AcquiredTerrainWorld[Index] = !GTerrainWorldRegistrations[Index].IsValid();
		auto RollBack = [&] {
			for (size_t Index = GTerrainWorldRegistrations.size(); Index-- > 0;)
				if (AcquiredTerrainWorld[Index]) GTerrainWorldRegistrations[Index].Reset();
			if (bAcquiredHeightmap) GTerrainHeightmapRegistration.Reset();
		};

		if (bAcquiredHeightmap)
		{
			GTerrainHeightmapRegistration = RegisterBuildFunction(
				Private::TerrainHeightmapFunctionIdentity,
				Private::CreateTerrainHeightmapBuildFunction(), Gate, OutError);
			if (!GTerrainHeightmapRegistration.IsValid()) return false;
		}
		for (uint8 Value = 1; Value <= 5; ++Value)
		{
			const size_t Index = Value - 1;
			if (!AcquiredTerrainWorld[Index]) continue;
			const auto ProductClass = static_cast<ETerrainTileProductClass>(Value);
			GTerrainWorldRegistrations[Index] = RegisterBuildFunction(
				Private::GetTerrainWorldBuildFunctionIdentity(ProductClass),
				Private::CreateTerrainWorldBuildFunction(ProductClass), Gate, OutError);
			if (!GTerrainWorldRegistrations[Index].IsValid())
			{
				RollBack();
				return false;
			}
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto InitializeTerrainBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError) -> bool
	{
		return EnsureTerrainBuildFunctions(OutError, std::move(Gate));
	}

	auto ShutdownTerrainBuildFunctions() -> void
	{
		std::lock_guard Lock(GTerrainBuildFunctionMutex);
		for (auto It = GTerrainWorldRegistrations.rbegin();
			It != GTerrainWorldRegistrations.rend(); ++It) It->Reset();
		GTerrainHeightmapRegistration.Reset();
	}
}

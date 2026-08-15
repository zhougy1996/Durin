#pragma once

#include "AssetBuild/BuildTypes.h"
#include "Modules/ModularFeature.h"

namespace Durin::Asset::Build
{
	// Family-neutral workload contribution used to aggregate independent async
	// asset-family queues. Synchronous recipe registration does not contribute.
	struct FBuildServiceContribution
	{
		std::string Identity;
		int32 DrainOrder = 0;
		FModuleOwnedCallbackGate OwnerGate;
		std::function<bool()> Start;
		std::function<void()> StopAdmission;
		std::function<uint32(uint32)> PumpCompletions;
		std::function<bool(double)> Wait;
		std::function<void()> Drain;
		std::function<std::tuple<uint32, uint32, uint64>()> Snapshot;
	};

	struct FBuildHostSnapshot
	{
		uint32 ServiceCount = 0;
		uint32 QueuedRequestCount = 0;
		uint32 RunningRequestCount = 0;
		uint64 InFlightEstimatedBytes = 0;
		bool bAcceptingRequests = false;
	};

	// Lifetime token for one service contribution.
	class FBuildServiceRegistration
	{
	public:
		FBuildServiceRegistration() = default;
		ASSETBUILDCORE_API ~FBuildServiceRegistration();
		FBuildServiceRegistration(const FBuildServiceRegistration&) = delete;
		auto operator=(const FBuildServiceRegistration&) -> FBuildServiceRegistration& = delete;
		ASSETBUILDCORE_API FBuildServiceRegistration(FBuildServiceRegistration&& Other) noexcept;
		ASSETBUILDCORE_API auto operator=(FBuildServiceRegistration&& Other) noexcept
			-> FBuildServiceRegistration&;

		ASSETBUILDCORE_API auto Reset() -> void;
		auto IsValid() const -> bool { return Generation != 0; }

	private:
		std::string Identity;
		uint64 Generation = 0;

		friend ASSETBUILDCORE_API auto RegisterBuildServiceContribution(
			FBuildServiceContribution, std::string*) -> FBuildServiceRegistration;
	};

	ASSETBUILDCORE_API auto RegisterBuildServiceContribution(
		FBuildServiceContribution Contribution,
		std::string* OutError = nullptr) -> FBuildServiceRegistration;
	ASSETBUILDCORE_API auto InitializeBuildHost(std::string* OutError = nullptr) -> bool;
	ASSETBUILDCORE_API auto PumpBuildHostCompletions(uint32 MaximumCount = 64) -> uint32;
	ASSETBUILDCORE_API auto WaitForBuildHost(double TimeoutSeconds = 30.0) -> bool;
	ASSETBUILDCORE_API auto GetBuildHostSnapshot() -> FBuildHostSnapshot;
	ASSETBUILDCORE_API auto ShutdownBuildHost() -> void;
}

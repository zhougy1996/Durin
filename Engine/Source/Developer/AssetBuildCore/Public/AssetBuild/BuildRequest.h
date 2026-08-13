#pragma once

#include "AssetBuild/BuildTypes.h"

namespace Durin::AssetBuild
{
	// Aggregate state of one request owner without exposing its synchronization state.
	struct FBuildRequestOwnerSnapshot
	{
		uint64 AcceptedRequestCount = 0;
		uint64 CompletedRequestCount = 0;
		uint32 ActiveRequestCount = 0;
		bool bAcceptingRequests = true;
		bool bCanceled = false;
		std::vector<std::string> Diagnostics;
	};

	// Owns admission, cancellation, barriers, callbacks and diagnostics for local requests.
	class FBuildRequestOwner
	{
	public:
		ASSETBUILDCORE_API FBuildRequestOwner();
		ASSETBUILDCORE_API ~FBuildRequestOwner();
		FBuildRequestOwner(const FBuildRequestOwner&) = delete;
		auto operator=(const FBuildRequestOwner&) -> FBuildRequestOwner& = delete;

		ASSETBUILDCORE_API auto CloseAdmission() -> void;
		ASSETBUILDCORE_API auto Cancel() -> void;
		ASSETBUILDCORE_API auto IsCanceled() const -> bool;
		ASSETBUILDCORE_API auto Wait(double TimeoutSeconds) -> bool;
		ASSETBUILDCORE_API auto GetSnapshot() const -> FBuildRequestOwnerSnapshot;

	private:
		struct FState;
		std::shared_ptr<FState> State;

		friend auto BeginBuildRequest(FBuildRequestOwner&) -> bool;
		friend auto CompleteBuildRequest(
			FBuildRequestOwner&, std::string_view) -> void;
	};
}

#pragma once

#include "Components/SkyLightComponent.h"
#include "RHIResources.h"

namespace Durin
{
	// Native diagnostic mailbox; no component or managed asset crosses this boundary.
	enum class ESkyLightUpdateState : uint8 { Pending, Ready, Failed, Unsupported, MissingProvider, Backpressure };
	struct FSkyLightUpdateStatus
	{
		std::atomic<ESkyLightUpdateState> State{ESkyLightUpdateState::Pending};
		std::atomic<uint64> PublishedRevision{0};
		std::atomic<double> CaptureTime{0};
		std::atomic<double> UpdateMilliseconds{0};
		std::atomic<uint64> CompletedUpdates{0};
        std::atomic<uint64> RetainedImageBytes{0};
        std::atomic<double> SteadyCPUMilliseconds{0};
	};

	// A detached value retained by scene membership and admitted lighting work.
	struct FSkyLightSceneProxy
	{
		FGuid PersistentId;
		std::string SelectionKey;
		uint64 InstanceId = 0;
		uint64 SourceEpoch = 0;
		uint64 RequestSerial = 0;
		int32 Priority = 0;
		bool bEligible = false;
		ESkyLightSourceMode SourceMode = ESkyLightSourceMode::SpecifiedCube;
		FRHITextureReferenceRef Texture;
		FQuat Rotation{1.0, 0.0, 0.0, 0.0};
		float Intensity = 1.0f;
		bool bAutomaticRefresh = true;
		float RefreshInterval = 1.0f;
		std::shared_ptr<FSkyLightUpdateStatus> UpdateStatus;
	};
}

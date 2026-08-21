#pragma once

#include "LaunchArguments.h"

namespace Durin
{
	struct FTaskSchedulerLifecycleSmokeState;
	struct FRendererContactRuntimeSmokeState;

	// Owns opt-in diagnostic configuration and retained lifecycle-smoke state.
	class FApplicationDiagnostics
	{
	public:
		explicit FApplicationDiagnostics(FLaunchDiagnosticsRequest InRequest);

		auto AtPreInitialization() const -> void;
		auto AfterLoggerStarted() const -> void;
		auto AfterEngineInitialized() -> void;
		auto Tick() -> void;
		auto BeginConsumerDetachment() -> void;
		auto BeforeAssetServiceShutdown() const -> void;
		auto AfterTaskSystemShutdown() const -> void;
		auto AtObjectCollection() const -> void;

	private:
		auto FillCrashLogGap() const -> void;

		FLaunchDiagnosticsRequest Request;
		std::shared_ptr<FTaskSchedulerLifecycleSmokeState> TaskSchedulerState;
		std::shared_ptr<FRendererContactRuntimeSmokeState>
			RendererContactRuntimeState;
		bool bEditorPIELifecycleSmokeCompleted = false;
		bool bNativeGameplayLifecycleSmokeCompleted = false;
		bool bRendererContactRuntimeSmokeCompleted = false;
	};
}

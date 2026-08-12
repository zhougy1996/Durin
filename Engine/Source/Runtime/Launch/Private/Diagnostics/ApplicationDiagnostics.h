#pragma once

#include "LaunchContracts/LaunchArguments.h"

namespace Durin
{
	struct FTaskSchedulerLifecycleSmokeState;

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
		bool bEditorPIELifecycleSmokeCompleted = false;
		bool bNativeGameplayLifecycleSmokeCompleted = false;
	};
}

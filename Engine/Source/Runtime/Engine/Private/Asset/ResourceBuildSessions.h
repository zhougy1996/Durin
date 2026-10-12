#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"

namespace Durin::AssetPrivate
{
	// Family-owned sessions; the production service still owns cache dispatch.
	// Callers finish asset compilation before closing these sessions at module unload.
	class FResourceBuildSessions
	{
	public:
		auto Open() -> void { std::lock_guard Lock(Mutex); Closed = false; }
		auto Get(DerivedData::EBuildCacheExecution Execution)
			-> std::expected<std::shared_ptr<DerivedData::FBuildSession>, DerivedData::FBuildAdmissionError>
		{
			std::lock_guard Lock(Mutex);
			if (Closed) return std::unexpected(DerivedData::FBuildAdmissionError{
				DerivedData::EBuildAdmissionReason::Closed, "Resource build sessions are closed."});
			const bool Task = Execution == DerivedData::EBuildCacheExecution::Task;
			auto& Session = Sessions[Task ? 1 : 0];
			if (!Session)
			{
				auto& Service = Task && TaskService ? *TaskService : DerivedData::GetBuild();
				auto Created = Service.CreateSession({}, Task ? DerivedData::CreateTaskBuildScheduler()
					: DerivedData::CreateInlineBuildScheduler(), Execution);
				if (!Created) return std::unexpected(std::move(Created.error()));
				Session = std::move(*Created);
			}
			return Session;
		}

		// Test fixtures scope the injected service and restore it after owner retirement.
		// Detach before drain so callbacks can reenter session acquisition.
		auto SetTaskService(std::shared_ptr<DerivedData::IBuild> Service) -> DerivedData::EBuildDrainResult
		{
			std::shared_ptr<DerivedData::FBuildSession> Retired;
			std::shared_ptr<DerivedData::IBuild> RetiredService;
			{
				std::lock_guard Lock(Mutex);
				Retired = std::exchange(Sessions[1], {});
				RetiredService = std::exchange(TaskService, std::move(Service));
			}
			if (Retired) Retired->Close();
			if (RetiredService) RetiredService->Close();
			if (Retired && Retired->Drain() == DerivedData::EBuildDrainResult::WouldBlock)
				return DerivedData::EBuildDrainResult::WouldBlock;
			return RetiredService ? RetiredService->Drain() : DerivedData::EBuildDrainResult::Drained;
		}

		auto Close() -> DerivedData::EBuildDrainResult
		{
			std::array<std::shared_ptr<DerivedData::FBuildSession>, 2> Retired;
			std::shared_ptr<DerivedData::IBuild> RetiredService;
			{
				std::lock_guard Lock(Mutex);
				Closed = true; Retired = Sessions;
				RetiredService = TaskService;
			}
			for (const auto& Session : Retired) if (Session) Session->Close();
			if (RetiredService) RetiredService->Close();
			for (const auto& Session : Retired)
				if (Session && Session->Drain() == DerivedData::EBuildDrainResult::WouldBlock)
					return DerivedData::EBuildDrainResult::WouldBlock;
			if (RetiredService && RetiredService->Drain() == DerivedData::EBuildDrainResult::WouldBlock)
				return DerivedData::EBuildDrainResult::WouldBlock;
			{
				std::lock_guard Lock(Mutex);
				if (Sessions == Retired) Sessions = {};
				if (TaskService == RetiredService) TaskService.reset();
			}
			return DerivedData::EBuildDrainResult::Drained;
		}

		auto Build(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::FBuildRequestOptions Options)
			-> std::expected<DerivedData::FBuildCompleteParams, DerivedData::FBuildAdmissionError>
		{
			if (Options.Cancellation.IsCancelled()) return DerivedData::FBuildCompleteParams::Canceled(
				std::nullopt, DerivedData::EBuildStatus::None);
			auto Session = Get(DerivedData::EBuildCacheExecution::Inline);
			if (!Session) return std::unexpected(std::move(Session.error()));
			Options.InputResolver = std::move(Resolver);
			DerivedData::FRequestOwner Owner;
			std::optional<DerivedData::FBuildCompleteParams> Completion;
			auto Admitted = (*Session)->Build(std::move(Definition), Owner, [&](auto Value) {
				Completion = std::move(Value);
			}, {}, std::move(Options));
			if (!Admitted) return std::unexpected(std::move(Admitted.error()));
			require(Owner.Wait() == DerivedData::ERequestWaitResult::Completed);
			require(Completion.has_value());
			return std::move(*Completion);
		}

	private:
		std::mutex Mutex;
		bool Closed = false;
		std::array<std::shared_ptr<DerivedData::FBuildSession>, 2> Sessions;
		std::shared_ptr<DerivedData::IBuild> TaskService;
	};
}
#endif

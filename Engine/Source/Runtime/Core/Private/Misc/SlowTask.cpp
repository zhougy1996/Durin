#include "Misc/ScopedSlowTask.h"
#include "Misc/Time.h"

#include <exception>

namespace Durin
{
	struct FSlowTaskData
	{
		uint64 Id = 0;
		std::string Title, Message, Details, Diagnostic;
		double Total = 0, Completed = 0, FrameWork = 0, FrameFraction = 0;
		double Started = 0;
		std::optional<double> Ended, OverrideFraction, DialogThreshold;
		bool bOverride = false, bCanCancel = false, bRetired = false, bHasParent = false;
		ESlowTaskStatus Status = ESlowTaskStatus::Running;
		std::weak_ptr<FSlowTaskData> Parent, Child;
		std::shared_ptr<std::atomic<bool>> Cancellation = std::make_shared<std::atomic<bool>>(false);
	};
	struct FSlowTaskContextState
	{
		std::mutex Mutex;
		bool bRetired = false;
		uint64 NextId = 1;
		std::function<double()> Clock;
		std::vector<std::weak_ptr<FSlowTaskData>> Tasks;
		std::unordered_map<std::thread::id, std::vector<std::weak_ptr<FSlowTaskData>>> Scopes;
	};
	namespace
	{
		auto IsLive(const FSlowTaskContextState& State, const FSlowTaskData& Task) -> bool
		{
			if (State.bRetired || Task.bRetired) return false;
			if (!Task.bHasParent) return true;
			auto Parent = Task.Parent.lock();
			while (Parent)
			{
				if (Parent->bRetired || Parent->Status != ESlowTaskStatus::Running) return false;
				if (!Parent->bHasParent) return true;
				Parent = Parent->Parent.lock();
			}
			return false;
		}
		auto Fraction(const FSlowTaskData& Task) -> std::optional<double>
		{
			if (Task.Status == ESlowTaskStatus::Completed) return 1.0;
			if (Task.bOverride) return Task.OverrideFraction;
			if (Task.Total <= 0) return std::nullopt;
			double FrameFraction = Task.FrameFraction;
			if (const auto Child = Task.Child.lock(); Child && !Child->bRetired)
			{
				const auto ChildFraction = Fraction(*Child);
				if (!ChildFraction && Task.FrameWork > 0) return std::nullopt;
				FrameFraction = ChildFraction.value_or(0);
			}
			return std::clamp((Task.Completed + Task.FrameWork * FrameFraction) / Task.Total, 0.0, 1.0);
		}
		auto ElapsedSeconds(const FSlowTaskContextState& State, const FSlowTaskData& Task) -> double
		{
			return std::max(0.0, (Task.Ended ? *Task.Ended : State.Clock()) - Task.Started);
		}
		auto IsDialogVisible(const FSlowTaskData& Task, double Elapsed) -> bool
		{
			return Task.DialogThreshold && Task.Status == ESlowTaskStatus::Running && Elapsed >= *Task.DialogThreshold;
		}
		auto Snapshot(const FSlowTaskContextState& State, const FSlowTaskData& Task) -> FSlowTaskSnapshot
		{
			const double Elapsed = ElapsedSeconds(State, Task);
			FSlowTaskSnapshot Result{.Id = Task.Id, .Title = Task.Title, .Message = Task.Message,
				.Details = Task.Details, .Diagnostic = Task.Diagnostic, .Fraction = Fraction(Task),
				.ElapsedSeconds = Elapsed, .Status = Task.Status,
				.bDialogVisible = IsDialogVisible(Task, Elapsed),
				.bCanCancel = Task.bCanCancel, .bCancelRequested = Task.Cancellation->load()};
			auto Child = Task.Child.lock();
			while (Child && !Child->bRetired && Child->Status == ESlowTaskStatus::Running)
			{
				Result.Message = Child->Message;
				Child = Child->Child.lock();
			}
			return Result;
		}
	}

	FFeedbackContext::FFeedbackContext(std::function<double()> Clock) : State(std::make_shared<FSlowTaskContextState>())
	{
		State->Clock = Clock ? std::move(Clock) : FTime::Seconds;
	}
	FFeedbackContext::~FFeedbackContext() { Retire(); }
	auto FFeedbackContext::Get() -> FFeedbackContext& { thread_local FFeedbackContext Context; return Context; }
	auto FFeedbackContext::Retire() -> void { std::lock_guard Lock(State->Mutex); State->bRetired = true; }
	auto FFeedbackContext::GetVisibleTask() const -> std::optional<FSlowTaskSnapshot>
	{
		std::lock_guard Lock(State->Mutex);
		for (auto Iterator = State->Tasks.rbegin(); Iterator != State->Tasks.rend(); ++Iterator)
			if (const auto Task = Iterator->lock(); Task && Task->DialogThreshold && IsLive(*State, *Task)
				&& IsDialogVisible(*Task, ElapsedSeconds(*State, *Task)))
				return Snapshot(*State, *Task);
		return std::nullopt;
	}
	auto FFeedbackContext::RequestCancel(uint64 Id) -> bool
	{
		std::lock_guard Lock(State->Mutex);
		for (const auto& Weak : State->Tasks)
			if (const auto Task = Weak.lock(); Task && Task->Id == Id && IsLive(*State, *Task))
			{
				if (!Task->bCanCancel || Task->Cancellation->load()
					|| !IsDialogVisible(*Task, ElapsedSeconds(*State, *Task))) return false;
				Task->Cancellation->store(true);
				return true;
			}
		return false;
	}

	FSlowTask::FSlowTask(double AmountOfWork, std::string Message, FFeedbackContext& Context)
		: State(Context.State), Data(std::make_shared<FSlowTaskData>())
	{
		std::lock_guard Lock(State->Mutex);
		Data->Id = State->NextId++;
		Data->Title = Data->Message = std::move(Message);
		Data->Total = std::isfinite(AmountOfWork) ? std::max(0.0, AmountOfWork) : 0;
		Data->Started = State->Clock();
		auto& Stack = State->Scopes[std::this_thread::get_id()];
		while (!Stack.empty() && Stack.back().expired()) Stack.pop_back();
		if (!Stack.empty())
			if (auto Parent = Stack.back().lock(); Parent && Parent->Status == ESlowTaskStatus::Running)
			{
				Data->Parent = Parent; Data->bHasParent = true;
				Data->Cancellation = Parent->Cancellation;
				Parent->Child = Data;
			}
		std::erase_if(State->Tasks, [](const auto& Task) { return Task.expired(); });
		State->Tasks.emplace_back(Data);
	}
	FSlowTask::~FSlowTask()
	{
		std::lock_guard Lock(State->Mutex);
		if (auto Parent = Data->Parent.lock(); Parent && Parent->Child.lock() == Data)
			Parent->FrameFraction = Fraction(*Data).value_or(0);
		Data->bRetired = true;
		for (auto& [Thread, Stack] : State->Scopes)
			std::erase_if(Stack, [this](const auto& Entry) { return Entry.expired() || Entry.lock() == Data; });
		std::erase_if(State->Scopes, [](const auto& Entry) { return Entry.second.empty(); });
	}
	auto FSlowTask::PushScope() -> void
	{
		std::lock_guard Lock(State->Mutex);
		State->Scopes[std::this_thread::get_id()].emplace_back(Data);
	}
	auto FSlowTask::EnterProgressFrame(double ExpectedWork, std::string Message) -> void
	{
		std::lock_guard Lock(State->Mutex);
		if (!IsLive(*State, *Data) || Data->Status != ESlowTaskStatus::Running) return;
		Data->Completed = std::min(Data->Total, Data->Completed + Data->FrameWork);
		Data->FrameWork = std::isfinite(ExpectedWork) ? std::clamp(ExpectedWork, 0.0, Data->Total - Data->Completed) : 0;
		Data->FrameFraction = 0; Data->Child.reset(); Data->bOverride = false;
		Data->Message = Message.empty() ? Data->Title : std::move(Message);
	}
	auto FSlowTask::SetProgress(std::optional<double> Value, std::string Message) -> void
	{
		std::lock_guard Lock(State->Mutex);
		if (!IsLive(*State, *Data) || Data->Status != ESlowTaskStatus::Running) return;
		Data->bOverride = true;
		Data->OverrideFraction = Value && std::isfinite(*Value) ? std::optional(std::clamp(*Value, 0.0, 1.0)) : std::nullopt;
		if (!Message.empty()) Data->Message = std::move(Message);
	}
	auto FSlowTask::SetMessage(std::string Message) -> void
	{
		std::lock_guard Lock(State->Mutex);
		if (IsLive(*State, *Data) && Data->Status == ESlowTaskStatus::Running) Data->Message = std::move(Message);
	}
	auto FSlowTask::SetDetails(std::string Details) -> void
	{
		std::lock_guard Lock(State->Mutex);
		if (IsLive(*State, *Data) && Data->Status == ESlowTaskStatus::Running) Data->Details = std::move(Details);
	}
	auto FSlowTask::MakeDialog(bool bShowCancelButton) -> void { MakeDialogDelayed(0, bShowCancelButton); }
	auto FSlowTask::MakeDialogDelayed(double ThresholdSeconds, bool bShowCancelButton) -> void
	{
		std::lock_guard Lock(State->Mutex);
		if (!IsLive(*State, *Data) || Data->Status != ESlowTaskStatus::Running) return;
		Data->bCanCancel = bShowCancelButton;
		Data->DialogThreshold = std::isfinite(ThresholdSeconds) ? std::max(0.0, ThresholdSeconds) : 0;
	}
	auto FSlowTask::ShouldCancel() const -> bool
	{
		std::lock_guard Lock(State->Mutex);
		return !IsLive(*State, *Data) || Data->Cancellation->load();
	}
	auto FSlowTask::Finish(ESlowTaskStatus Status, std::string Diagnostic) -> void
	{
		std::lock_guard Lock(State->Mutex);
		if (!IsLive(*State, *Data) || Data->Status != ESlowTaskStatus::Running) return;
		Data->Status = Status; Data->Diagnostic = std::move(Diagnostic); Data->Ended = State->Clock();
		if (Status == ESlowTaskStatus::Canceled) Data->Cancellation->store(true);
	}
	auto FSlowTask::Complete() -> void { Finish(ESlowTaskStatus::Completed); }
	auto FSlowTask::Fail(std::string Diagnostic) -> void { Finish(ESlowTaskStatus::Failed, std::move(Diagnostic)); }
	auto FSlowTask::Cancel() -> void { Finish(ESlowTaskStatus::Canceled); }
	auto FSlowTask::GetSnapshot() const -> std::optional<FSlowTaskSnapshot>
	{
		std::lock_guard Lock(State->Mutex);
		return IsLive(*State, *Data) ? std::optional(Snapshot(*State, *Data)) : std::nullopt;
	}
	FScopedSlowTask::FScopedSlowTask(double AmountOfWork, std::string Message, FFeedbackContext& Context)
		: FSlowTask(AmountOfWork, std::move(Message), Context), ExceptionsOnEntry(std::uncaught_exceptions()) { PushScope(); }
	FScopedSlowTask::~FScopedSlowTask()
	{
		if (std::uncaught_exceptions() > ExceptionsOnEntry) Fail("Scope exited with an exception.");
		else if (ShouldCancel()) Cancel();
		else Complete();
	}
}

#include "Tools/EditorToolService.h"
#include "Editor/EditorEngine.h"
#include "DObject/Object.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		using FClock = std::chrono::steady_clock;
		auto Diagnostic(EToolError Code, std::string Message) -> FToolDiagnostic { return {Code, std::move(Message), {}}; }
		auto Failure(EToolError Code, std::string Message) -> FToolResult
		{
			return {EToolStatus::Failed, {}, {Diagnostic(Code, std::move(Message))}};
		}
		auto BoundedDiagnostic(FToolDiagnostic Item) -> FToolDiagnostic
		{
			if (Item.Path.size() > 1024) { Item.Path.clear(); Item.Message += " (Diagnostic path exceeds its byte limit.)"; }
			if (Item.Message.size() > 4096) Item.Message.resize(4096);
			return Item;
		}
		auto ValueBytes(const FToolValue& Value) -> size_t
		{
			size_t Bytes = sizeof(Value);
			if (const auto* String = std::get_if<std::string>(&Value.Data)) Bytes += String->capacity();
			if (const auto* Array = std::get_if<FToolValue::FArray>(&Value.Data))
			{
				Bytes += Array->capacity() * sizeof(FToolValue);
				for (const auto& Child : *Array) Bytes += ValueBytes(Child);
			}
			if (const auto* Object = std::get_if<FToolValue::FObject>(&Value.Data))
				for (const auto& [Key, Child] : *Object) Bytes += sizeof(Key) + Key.capacity() + 4 * sizeof(void*) + ValueBytes(Child);
			return Bytes;
		}
		auto SchemaBytes(const FToolSchema& Schema) -> size_t
		{
			size_t Bytes = sizeof(Schema) + Schema.Description.capacity()
				+ Schema.Required.capacity() * sizeof(std::string) + Schema.Enum.capacity() * sizeof(FToolValue);
			for (const auto& [Key, Child] : Schema.Properties) Bytes += Key.capacity() + 4 * sizeof(void*) + SchemaBytes(Child);
			for (const auto& Key : Schema.Required) Bytes += Key.capacity();
			for (const auto& Value : Schema.Enum) Bytes += ValueBytes(Value);
			for (const auto* Value : {&Schema.Default, &Schema.Minimum, &Schema.Maximum}) if (*Value) Bytes += ValueBytes(**Value);
			if (Schema.Items) Bytes += SchemaBytes(*Schema.Items);
			return Bytes;
		}
		auto ResultBytes(const FToolResult& Result) -> size_t
		{
			size_t Bytes = sizeof(Result) + ValueBytes(Result.Data) + Result.Diagnostics.capacity() * sizeof(FToolDiagnostic);
			for (const auto& Item : Result.Diagnostics) Bytes += Item.Message.capacity() + Item.Path.capacity();
			return Bytes;
		}
		auto ValidDiagnostics(const std::vector<FToolDiagnostic>& Items) -> bool
		{
			return Items.size() <= 64 && std::ranges::all_of(Items, [](const auto& Item) {
				return Item.Message.size() <= 4096 && Item.Path.size() <= 1024
					&& static_cast<uint8>(Item.Code) <= static_cast<uint8>(EToolError::DomainFailure);
			});
		}
		auto ValidateRequest(const FToolRequest& Request, EToolSource Source) -> FToolValidationResult
		{
			if (!ValidateToolName(Request.Address.Toolset) || !ValidateToolName(Request.Address.Tool) || static_cast<uint8>(Source) > static_cast<uint8>(EToolSource::External))
				return std::unexpected(Diagnostic(EToolError::InvalidArguments, "Invalid tool name or source."));
			return ValidateToolValue(Request.Arguments);
		}
		auto HostReady(DEditorEngine& Host) -> bool
		{
			const auto* Buffer = Cast<DTransBuffer>(Host.GetTransactor());
			return Host.GetEditorSubsystemState() == ESubsystemState::Ready && Buffer
				&& Buffer->GetState() == Editor::ETransactorState::Idle;
		}
		struct FHostBinding;
		thread_local FHostBinding* GBinding = nullptr;
		struct FHostBinding
		{
			DEditorEngine& Editor;
			const FToolContext& Context;
			FHostBinding* Previous = GBinding;
			FHostBinding(DEditorEngine& InEditor, const FToolContext& InContext) : Editor(InEditor), Context(InContext) { GBinding = this; }
			~FHostBinding() { GBinding = Previous; }
		};
		struct FExecutingScope
		{
			bool& Executing;
			explicit FExecutingScope(bool& Value) : Executing(Value) { Executing = true; }
			~FExecutingScope() { Executing = false; }
		};
	}

	struct FEditorToolService::FImpl
	{
		struct FRoute { FToolDescriptor Descriptor; uint64 ToolsetIdentity = 0; size_t Bytes = 0; };
		struct FInvocation
		{
			FToolRequest Request;
			std::shared_ptr<const FRoute> Route;
			FToolCancellationSource Cancellation;
			FToolContext Context;
			EToolInvocationState State = EToolInvocationState::Queued;
			std::optional<FToolResult> Result;
			size_t Bytes = 0;
			FClock::time_point Completed;
		};
		// Retaining the native endpoint also retains the subsystem's code/work gate.
		std::shared_ptr<const FSubsystemWorkGate> Gate;
		mutable std::mutex Mutex;
		FEditorToolLimits Limits;
		std::map<FToolAddress, std::shared_ptr<const FRoute>> Routes;
		std::map<std::string, FEditorToolsetCatalogEntry, std::less<>> Toolsets;
		std::vector<FToolDiagnostic> CatalogDiagnostics;
		std::map<uint64, std::unique_ptr<FInvocation>> Invocations;
		std::deque<uint64> Queue;
		std::deque<uint64> Completed;
		uint64 Session;
		uint64 NextSequence = 1;
		uint64 CatalogRevision = 0;
		uint64 FeatureRevision = 0;
		size_t QueuedBytes = 0;
		size_t CompletedBytes = 0;
		bool HasCatalog = false;
		bool Closed = false;
		bool Executing = false; // GameThread only, never consulted by endpoint worker methods.

		explicit FImpl(std::shared_ptr<const FSubsystemWorkGate> InGate) : Gate(std::move(InGate))
		{
			static uint64 NextSession = 1; // Endpoint construction is GameThread-only.
			Session = NextSession++;
		}
		auto Open() const -> bool { return !Closed && Gate && Gate->IsOpen(); }
		auto Prune() -> void
		{
			const auto Now = FClock::now();
			while (!Completed.empty())
			{
				const auto Found = Invocations.find(Completed.front());
				if (Completed.size() <= Limits.MaxCompleted && CompletedBytes <= Limits.MaxCompletedBytes
					&& Now - Found->second->Completed < Limits.ResultTTL) break;
				CompletedBytes -= Found->second->Bytes;
				Invocations.erase(Found);
				Completed.pop_front();
			}
		}
		auto Complete(uint64 Id, FToolResult Result, EToolInvocationState State = EToolInvocationState::Completed) -> void
		{
			auto& Entry = *Invocations.at(Id);
			Entry.Route.reset();
			Entry.Request = {};
			Entry.Result = std::move(Result);
			Entry.State = State;
			Entry.Completed = FClock::now();
			Entry.Bytes = ResultBytes(*Entry.Result);
			CompletedBytes += Entry.Bytes;
			Completed.push_back(Id);
			Prune();
		}
		auto CloseLocked() -> void
		{
			if (Closed) return;
			Closed = true;
			Routes.clear(); Toolsets.clear();
			CatalogDiagnostics.clear();
			++CatalogRevision;
			for (auto Id : Completed) Invocations.erase(Id);
			Completed.clear(); CompletedBytes = 0;
			auto Pending = std::move(Queue);
			Queue.clear(); QueuedBytes = 0;
			for (auto Id : Pending)
			{
				Invocations.at(Id)->Cancellation.Cancel();
				Complete(Id, Failure(EToolError::Cancelled, "Editor tool service closed before execution."), EToolInvocationState::Cancelled);
			}
			for (auto& [Id, Entry] : Invocations) if (Entry->State == EToolInvocationState::Running) Entry->Cancellation.Cancel();
		}
		auto SetContext(FInvocation& Entry, uint64 Id, EToolSource Source) -> void
		{
			Entry.Context = {std::format("{}:{}", Session, Id), std::to_string(Session), Source, Entry.Cancellation.GetToken()};
		}
		// Caller holds Mutex; preparation calls no provider and resolves no engine objects.
		auto Admit(const FToolRequest& Request, EToolSource Source) -> std::expected<std::unique_ptr<FInvocation>, FToolDiagnostic>
		{
			if (!Open()) return std::unexpected(Diagnostic(EToolError::ToolUnavailable, "Editor tool service is closed."));
			const auto Route = Routes.find(Request.Address);
			if (Route == Routes.end() || (Source == EToolSource::External && !Route->second->Descriptor.bExposeExternally))
				return std::unexpected(Diagnostic(EToolError::ToolUnavailable, "Tool is not available to this caller."));
			auto Prepared = PrepareToolArguments(Route->second->Descriptor, Request);
			if (!Prepared) return std::unexpected(BoundedDiagnostic(Prepared.error()));
			auto Entry = std::make_unique<FInvocation>();
			Entry->Request = std::move(*Prepared); Entry->Route = Route->second;
			Entry->Bytes = sizeof(FInvocation) + Request.Address.Toolset.size() + Request.Address.Tool.size() + ValueBytes(Entry->Request.Arguments) + Route->second->Bytes;
			return Entry;
		}
		auto Run(DEditorEngine& Host, FInvocation& Entry, bool AvailabilityOnly, FToolAvailability* Availability) -> FToolResult
		{
			if (!Gate->IsOpen() || Entry.Cancellation.GetToken().IsCancellationRequested()) return Failure(EToolError::Cancelled, "Tool admission closed.");
			auto Call = FModularFeatureRegistry::Get().InvokeRegistered<IToolset>(Entry.Route->ToolsetIdentity, [&](auto& Toolset) {
				FHostBinding Binding(Host, Entry.Context);
				auto Available = Toolset.CheckAvailability(Entry.Request, Entry.Context);
				if (!ValidDiagnostics(Available.Diagnostics)) return Failure(EToolError::LimitExceeded, "Toolset availability diagnostics exceed the contract.");
				if (!Available.bAvailable && Available.Diagnostics.empty()) Available.Diagnostics.push_back(Diagnostic(EToolError::ToolUnavailable, "Tool is unavailable in the current state."));
				if (Availability) *Availability = Available;
				if (!Gate->IsOpen() || Entry.Context.Cancellation.IsCancellationRequested()) return Failure(EToolError::Cancelled, "Tool cancelled before execution.");
				if (!Available.bAvailable) return FToolResult{EToolStatus::Failed, {}, std::move(Available.Diagnostics)};
				if (!HostReady(Host)) return Failure(EToolError::Busy, "Editor left the dispatch safe point during availability checking.");
				if (AvailabilityOnly) return FToolResult{EToolStatus::Succeeded};
				return Toolset.Call(Entry.Request, Entry.Context);
			});
			if (Call.Status == EFeatureInvokeStatus::Unavailable) return Failure(EToolError::ToolUnavailable, "The admitted toolset is no longer registered.");
			if (!Call.WasInvoked() || !Call.Value) return Failure(EToolError::InternalError, "Toolset threw an exception.");
			auto Result = std::move(*Call.Value);
			if (!ValidDiagnostics(Result.Diagnostics)) return Failure(EToolError::LimitExceeded, "Toolset result diagnostics exceed the contract.");
			if (Result.OperationId || (Result.Status != EToolStatus::Succeeded && Result.Status != EToolStatus::Failed))
				return Failure(EToolError::InvalidOutput, "Only synchronous tool results are supported.");
			if (auto Check = ValidateToolValue(Result.Data); !Check) return Failure(EToolError::InvalidOutput, Check.error().Message);
			if (!AvailabilityOnly && Result.Status == EToolStatus::Succeeded)
				if (auto Check = ValidateToolOutput(Entry.Route->Descriptor, Result.Data); !Check)
				{
					auto Item = Check.error();
					if (Item.Code != EToolError::LimitExceeded) Item.Code = EToolError::InvalidOutput;
					return {EToolStatus::Failed, {}, {BoundedDiagnostic(std::move(Item))}};
				}
			if (Result.Status == EToolStatus::Failed && Result.Diagnostics.empty()) Result.Diagnostics.push_back(Diagnostic(EToolError::DomainFailure, "Tool failed without a diagnostic."));
			if (ResultBytes(Result) > Limits.MaxCompletedBytes) return Failure(EToolError::LimitExceeded, "Toolset result exceeds the service byte budget.");
			return Result;
		}
	};

	auto GetToolEditor(const FToolContext& Context) -> DEditorEngine*
	{
		return IsInGameThread() && GBinding && &GBinding->Context == &Context ? &GBinding->Editor : nullptr;
	}
	FEditorToolService::FEditorToolService(std::shared_ptr<const FSubsystemWorkGate> Gate) : Impl(std::make_unique<FImpl>(std::move(Gate))) {}
	FEditorToolService::~FEditorToolService() = default;
	auto FEditorToolService::CanDispatch(DEditorEngine& Editor) const -> bool
	{
		require(IsInGameThread());
		if (Impl->Executing || !HostReady(Editor)) return false;
		std::scoped_lock Lock(Impl->Mutex);
		return Impl->Open();
	}

	auto FEditorToolService::RefreshCatalog() -> void
	{
		require(IsInGameThread());
		auto& Registry = FModularFeatureRegistry::Get();
		if (Registry.IsDispatching()) return;
		const auto Revision = Registry.GetPublicationRevision();
		FEditorToolLimits Limits;
		{
			std::scoped_lock Lock(Impl->Mutex);
			if (!Impl->Open() || (Impl->HasCatalog && Impl->FeatureRevision == Revision)) return;
			Limits = Impl->Limits;
		}
		auto Described = Registry.InvokeAll<IToolset>([](auto& Toolset) { return Toolset.DescribeToolset(); });
		decltype(Impl->Routes) Routes;
		decltype(Impl->Toolsets) Toolsets;
		std::set<std::string> ClaimedSets;
		std::vector<FToolDiagnostic> Diagnostics;
		size_t Count = 0, Bytes = 0;
		bool OverBudget = Described.Invocations.size() > Limits.MaxCatalogToolsets;
		for (auto& Registration : Described.Invocations)
		{
			if (OverBudget) break;
			if (!Registration.WasInvoked() || !Registration.Value)
			{
				if (Diagnostics.size() < 64) Diagnostics.push_back(Diagnostic(EToolError::InternalError,
					std::format("Toolset registration {} failed to describe tools.", Registration.RegistrationIdentity)));
				continue;
			}
			auto& Set = *Registration.Value;
			if (Set.Tools.size() > Limits.MaxCatalogTools - Count) { OverBudget = true; break; }
			Count += Set.Tools.size();
			if (auto Check = ValidateToolsetDescriptor(Set); !Check)
			{
				// A valid name still claims its namespace even when the group metadata is invalid.
				if (ValidateToolName(Set.Name))
				{
					ClaimedSets.insert(Set.Name);
					Toolsets.erase(Set.Name);
					std::erase_if(Routes, [&](const auto& Pair) { return Pair.first.Toolset == Set.Name; });
				}
				if (Diagnostics.size() < 64) Diagnostics.push_back(BoundedDiagnostic(Check.error()));
				continue;
			}
			if (!ClaimedSets.insert(Set.Name).second)
			{
				Toolsets.erase(Set.Name);
				std::erase_if(Routes, [&](const auto& Pair) { return Pair.first.Toolset == Set.Name; });
				if (Diagnostics.size() < 64) Diagnostics.push_back(Diagnostic(EToolError::InvalidDescriptor,
					std::format("Toolset {} has multiple registrations.", Set.Name)));
				continue;
			}
			const size_t SetBytes = sizeof(FEditorToolsetCatalogEntry) + Set.Name.capacity() + Set.Description.capacity();
			if (SetBytes > Limits.MaxCatalogBytes - Bytes) { OverBudget = true; break; }
			Bytes += SetBytes;
			auto& Published = Toolsets[Set.Name];
			Published.Descriptor.Name = Set.Name;
			Published.Descriptor.Description = Set.Description;
			Published.ToolsetIdentity = Registration.RegistrationIdentity;
			std::set<std::string> ClaimedTools;
			for (auto& Descriptor : Set.Tools)
			{
				if (!ValidateToolName(Descriptor.Name))
				{
					if (Diagnostics.size() < 64) Diagnostics.push_back(Diagnostic(EToolError::InvalidDescriptor, "Invalid local tool name."));
					continue;
				}
				const FToolAddress Address{Set.Name, Descriptor.Name};
				if (!ClaimedTools.insert(Descriptor.Name).second)
				{
					Routes.erase(Address);
					if (Diagnostics.size() < 64) Diagnostics.push_back(Diagnostic(EToolError::InvalidDescriptor,
						std::format("Tool {}/{} is duplicated.", Set.Name, Descriptor.Name)));
					continue;
				}
				if (auto Check = ValidateToolDescriptor(Descriptor); !Check)
				{
					if (Diagnostics.size() < 64) Diagnostics.push_back(BoundedDiagnostic(Check.error()));
					continue;
				}
				auto Route = std::make_shared<FImpl::FRoute>();
				Route->ToolsetIdentity = Registration.RegistrationIdentity;
				Route->Bytes = sizeof(FImpl::FRoute) + Set.Name.size() + Descriptor.Name.capacity()
					+ Descriptor.Description.capacity() + SchemaBytes(Descriptor.Input) + SchemaBytes(Descriptor.Output);
				if (Route->Bytes > Limits.MaxCatalogBytes - Bytes) { OverBudget = true; break; }
				Bytes += Route->Bytes;
				Route->Descriptor = std::move(Descriptor);
				Routes.emplace(Address, std::move(Route));
			}
		}
		if (OverBudget)
		{
			Routes.clear(); Toolsets.clear(); Diagnostics = {Diagnostic(EToolError::LimitExceeded, "Tool catalog capacity exceeded.")};
		}
		std::scoped_lock Lock(Impl->Mutex);
		if (!Impl->Open()) return;
		Impl->Routes = std::move(Routes);
		Impl->Toolsets = std::move(Toolsets);
		Impl->CatalogDiagnostics = std::move(Diagnostics);
		Impl->FeatureRevision = Revision;
		Impl->HasCatalog = true;
		++Impl->CatalogRevision;
	}

	auto FEditorToolService::GetCatalog() const -> FEditorToolCatalog
	{
		std::scoped_lock Lock(Impl->Mutex);
		FEditorToolCatalog Result{Impl->CatalogRevision, Impl->FeatureRevision};
		if (!Impl->Open()) { Result.Diagnostics = {Diagnostic(EToolError::ToolUnavailable, "Editor tool service is closed.")}; return Result; }
		Result.Diagnostics = Impl->CatalogDiagnostics;
		for (const auto& [Name, Set] : Impl->Toolsets)
		{
			auto& Entry = Result.Toolsets.emplace_back(Set);
			for (const auto& [Address, Route] : Impl->Routes)
				if (Address.Toolset == Name) Entry.Descriptor.Tools.push_back(Route->Descriptor);
		}
		return Result;
	}
	auto FEditorToolService::Submit(const FToolRequest& Request, EToolSource Source) -> std::expected<FEditorToolInvocationId, FToolDiagnostic>
	{
		if (auto Check = ValidateRequest(Request, Source); !Check) return std::unexpected(BoundedDiagnostic(Check.error()));
		std::scoped_lock Lock(Impl->Mutex);
		Impl->Prune();
		if (!Impl->Open()) { Impl->CloseLocked(); return std::unexpected(Diagnostic(EToolError::ToolUnavailable, "Editor tool service is closed.")); }
		auto Admitted = Impl->Admit(Request, Source);
		if (!Admitted) return std::unexpected(Admitted.error());
		auto Entry = std::move(*Admitted);
		const auto Bytes = Entry->Bytes;
		if (Impl->Queue.size() >= Impl->Limits.MaxQueued || Bytes > Impl->Limits.MaxQueuedBytes - Impl->QueuedBytes) return std::unexpected(Diagnostic(EToolError::Busy, "Tool queue capacity exceeded."));
		const auto Id = Impl->NextSequence++;
		Impl->SetContext(*Entry, Id, Source);
		Impl->Invocations.emplace(Id, std::move(Entry)); Impl->Queue.push_back(Id); Impl->QueuedBytes += Bytes;
		return FEditorToolInvocationId{Impl->Session, Id};
	}
	auto FEditorToolService::GetInvocation(FEditorToolInvocationId Id) -> std::expected<FEditorToolInvocation, FToolDiagnostic>
	{
		std::scoped_lock Lock(Impl->Mutex);
		if (!Impl->Open()) Impl->CloseLocked();
		Impl->Prune();
		if (Id.Session != Impl->Session || Id.Sequence == 0 || Id.Sequence >= Impl->NextSequence) return std::unexpected(Diagnostic(EToolError::TargetNotFound, "Unknown invocation identity."));
		const auto Found = Impl->Invocations.find(Id.Sequence);
		if (Found == Impl->Invocations.end()) return FEditorToolInvocation{EToolInvocationState::Expired, Failure(EToolError::InvocationExpired, "Invocation result expired.")};
		return FEditorToolInvocation{Found->second->State, Found->second->Result};
	}
	auto FEditorToolService::Cancel(FEditorToolInvocationId Id) -> FToolValidationResult
	{
		std::scoped_lock Lock(Impl->Mutex);
		if (!Impl->Open()) Impl->CloseLocked();
		Impl->Prune();
		if (Id.Session != Impl->Session || Id.Sequence == 0 || Id.Sequence >= Impl->NextSequence) return std::unexpected(Diagnostic(EToolError::TargetNotFound, "Unknown invocation identity."));
		const auto Found = Impl->Invocations.find(Id.Sequence);
		if (Found == Impl->Invocations.end()) return std::unexpected(Diagnostic(EToolError::InvocationExpired, "Invocation result expired."));
		auto& Entry = *Found->second;
		if (Entry.State == EToolInvocationState::Queued || Entry.State == EToolInvocationState::Running) Entry.Cancellation.Cancel();
		if (Entry.State == EToolInvocationState::Queued)
		{
			Impl->QueuedBytes -= Entry.Bytes;
			std::erase(Impl->Queue, Id.Sequence);
			Impl->Complete(Id.Sequence, Failure(EToolError::Cancelled, "Invocation cancelled before execution."), EToolInvocationState::Cancelled);
		}
		return {};
	}
	auto FEditorToolService::Configure(const FEditorToolLimits& Limits) -> FToolValidationResult
	{
		if (!IsInGameThread()) return std::unexpected(Diagnostic(EToolError::WrongThread, "Tool configuration requires GameThread."));
		if (!Limits.MaxQueued || !Limits.MaxCompleted || !Limits.MaxCatalogTools || !Limits.MaxCatalogToolsets || !Limits.MaxDispatchPerTick
			|| !Limits.MaxCatalogBytes || !Limits.MaxQueuedBytes || !Limits.MaxCompletedBytes || Limits.TickBudget.count() <= 0 || Limits.ResultTTL.count() < 0)
			return std::unexpected(Diagnostic(EToolError::InvalidArguments, "Invalid tool service limits."));
		std::scoped_lock Lock(Impl->Mutex);
		if (!Impl->Open()) return std::unexpected(Diagnostic(EToolError::ToolUnavailable, "Tool service closed."));
		if (Impl->Executing || !Impl->Queue.empty()) return std::unexpected(Diagnostic(EToolError::Busy, "Configure requires an idle tool service."));
		Impl->Limits = Limits; Impl->HasCatalog = false;
		Impl->Routes.clear(); Impl->Toolsets.clear(); Impl->CatalogDiagnostics.clear(); ++Impl->CatalogRevision;
		Impl->Prune(); return {};
	}
	auto FEditorToolService::Call(DEditorEngine& Editor, const FToolRequest& Request, EToolSource Source,
		bool AvailabilityOnly, FToolAvailability* Availability) -> FToolResult
	{
		if (!CanDispatch(Editor)) return Failure(EToolError::Busy, "Editor tool dispatch is not at an idle safe point.");
		FExecutingScope Scope(Impl->Executing);
		RefreshCatalog();
		if (auto Check = ValidateRequest(Request, Source); !Check) return {EToolStatus::Failed, {}, {BoundedDiagnostic(Check.error())}};
		std::unique_ptr<FImpl::FInvocation> Entry;
		{
			std::scoped_lock Lock(Impl->Mutex);
			auto Admitted = Impl->Admit(Request, Source);
			if (!Admitted) return {EToolStatus::Failed, {}, {Admitted.error()}};
			Entry = std::move(*Admitted);
			if (Entry->Bytes > Impl->Limits.MaxQueuedBytes) return Failure(EToolError::Busy, "Tool input exceeds the service byte budget.");
			Impl->SetContext(*Entry, Impl->NextSequence++, Source);
			Entry->State = EToolInvocationState::Running;
		}
		return Impl->Run(Editor, *Entry, AvailabilityOnly, Availability);
	}
	auto FEditorToolService::Drain(DEditorEngine& Editor) -> void
	{
		if (!CanDispatch(Editor)) return;
		FExecutingScope Scope(Impl->Executing);
		RefreshCatalog();
		FEditorToolLimits Limits;
		{ std::scoped_lock Lock(Impl->Mutex); Limits = Impl->Limits; Impl->Prune(); }
		const auto Start = FClock::now();
		for (size_t Count = 0; Count < Limits.MaxDispatchPerTick && (Count == 0 || FClock::now() - Start < Limits.TickBudget); ++Count)
		{
			if (!HostReady(Editor)) break;
			FImpl::FInvocation* Entry;
			uint64 Id;
			{
				std::scoped_lock Lock(Impl->Mutex);
				if (!Impl->Open()) { Impl->CloseLocked(); break; }
				if (Impl->Queue.empty()) break;
				Id = Impl->Queue.front(); Impl->Queue.pop_front();
				Entry = Impl->Invocations.at(Id).get(); Impl->QueuedBytes -= Entry->Bytes;
				Entry->State = EToolInvocationState::Running;
			}
			auto Result = Impl->Run(Editor, *Entry, false, nullptr);
			std::scoped_lock Lock(Impl->Mutex);
			Impl->Complete(Id, std::move(Result));
		}
	}
	auto FEditorToolService::Close() noexcept -> void { std::scoped_lock Lock(Impl->Mutex); Impl->CloseLocked(); }
}

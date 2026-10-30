#include "Tools/EditorToolService.h"
#include "Tools/EditorScriptPump.h"
#include "Editor/EditorEngine.h"
#include "DObject/Object.h"
#include "DObject/WeakObjectPtr.h"
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
			if (!ValidateToolsetName(Request.Address.Toolset) || !ValidateToolName(Request.Address.Tool) || static_cast<uint8>(Source) > static_cast<uint8>(EToolSource::External))
				return std::unexpected(Diagnostic(EToolError::InvalidArguments, "Invalid tool name or source."));
			return ValidateToolValue(Request.Arguments);
		}
		auto HostReady(DEditorEngine& Host) -> bool
		{
			return Host.GetEditorSubsystemState() == ESubsystemState::Ready
				&& !Host.AreEditorSubsystemsUpdating() && Host.CanTransact();
		}
		struct FInvocationBinding;
		thread_local FInvocationBinding* GBinding = nullptr;
		struct FInvocationBinding
		{
			const FToolContext& Context;
			FInvocationBinding* Previous = GBinding;
			explicit FInvocationBinding(const FToolContext& InContext) : Context(InContext) { GBinding = this; }
			~FInvocationBinding() { GBinding = Previous; }
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
		struct FAccounting
		{
			std::atomic<size_t> Count{0}, Bytes{0};
		};
		static constexpr size_t TerminalReserve = 1024;
		struct FCharge
		{
			std::shared_ptr<FAccounting> Accounting;
			size_t Bytes = TerminalReserve;
			explicit FCharge(std::shared_ptr<FAccounting> InAccounting) : Accounting(std::move(InAccounting))
			{ ++Accounting->Count; Accounting->Bytes += Bytes; }
			~FCharge() { Accounting->Bytes -= Bytes; --Accounting->Count; }
		};
		struct FInvocation
		{
			FToolRequest Request;
			std::shared_ptr<const FRoute> Route;
			FToolCancellationSource Cancellation;
			FToolContext Context;
			EToolInvocationState State = EToolInvocationState::Queued;
			bool Dispatching = false;
			FToolFuture Raw;
			FToolPromise Completion = FToolPromise{};
			FToolFuture Future = Completion.GetFuture().Share();
			std::shared_ptr<FCharge> Charge;
			FFutureSubscription Observation;
			FFutureReadiness Ready;
			size_t Bytes = 0;
			FClock::time_point Completed;
		};
		// Retaining the native endpoint also retains the subsystem's code/work gate.
		std::shared_ptr<const FSubsystemWorkGate> Gate;
		// Resolved only on GameThread while this endpoint is open; never redirects to a replacement editor.
		TWeakObjectPtr<DEditorEngine> Editor;
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
		std::shared_ptr<FAccounting> Accounting = std::make_shared<FAccounting>();
		size_t PendingCount = 0, PendingBytes = 0;
		size_t QueuedBytes = 0;
		size_t CompletedBytes = 0;
		bool HasCatalog = false;
		bool Closed = false;
		bool Executing = false; // GameThread only.
		bool ScriptExecuting = false; // Protected by Mutex.
		uint64 PythonControlIdentity = 0;
		FPythonJobControl PythonControl;

		FImpl(DEditorEngine& InEditor, std::shared_ptr<const FSubsystemWorkGate> InGate) : Gate(std::move(InGate)), Editor(&InEditor)
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
			if (Entry.State == EToolInvocationState::Running) { --PendingCount; PendingBytes -= Entry.Bytes; }
			Entry.Observation.Reset(); Entry.Raw = {};
			Entry.Route.reset(); Entry.Request = {};
			const auto Bytes = ResultBytes(Result);
			if (Entry.Charge)
			{
				if (Bytes > Limits.MaxCompletedBytes || (Bytes > Entry.Charge->Bytes
					&& Bytes - Entry.Charge->Bytes > Limits.MaxCompletedBytes - std::min(Limits.MaxCompletedBytes, Accounting->Bytes.load())))
					Result = Failure(EToolError::LimitExceeded, "Live tool results exceed the service byte budget.");
				const auto ChargedBytes = std::max(TerminalReserve, ResultBytes(Result));
				Accounting->Bytes += ChargedBytes - Entry.Charge->Bytes; Entry.Charge->Bytes = ChargedBytes;
			}
			Entry.Completion.TrySetValue(std::move(Result));
			Entry.State = State;
			Entry.Completed = FClock::now();
			Entry.Bytes = ResultBytes(*Entry.Future.GetResultShared());
			CompletedBytes += Entry.Bytes;
			Completed.push_back(Id);
			Prune();
		}
		auto CloseLocked() -> void
		{
			if (Closed) return;
			Closed = true;
			PythonControl = {}; PythonControlIdentity = 0;
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
			std::vector<uint64> Running;
			for (auto& [Id, Entry] : Invocations) if (Entry->State == EToolInvocationState::Running) { Entry->Cancellation.Cancel(); if (!Entry->Dispatching) Running.push_back(Id); }
			for (auto Id : Running) Complete(Id, Failure(EToolError::Cancelled, "Editor closed; pending completion detached without rollback."), EToolInvocationState::Cancelled);
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
		auto Run(DEditorEngine& Host, FInvocation& Entry, bool AvailabilityOnly, FToolAvailability* Availability) -> FToolFuture
		{
			if (!Gate->IsOpen() || Entry.Cancellation.GetToken().IsCancellationRequested()) return ReadyToolResult(Failure(EToolError::Cancelled, "Tool admission closed."));
			auto Call = FModularFeatureRegistry::Get().InvokeRegistered<IToolset>(Entry.Route->ToolsetIdentity, [&](auto& Toolset) {
				FInvocationBinding Binding(Entry.Context);
				auto Available = Toolset.CheckAvailability(Entry.Request, Entry.Context);
				if (!ValidDiagnostics(Available.Diagnostics)) return ReadyToolResult(Failure(EToolError::LimitExceeded, "Toolset availability diagnostics exceed the contract."));
				if (!Available.bAvailable && Available.Diagnostics.empty()) Available.Diagnostics.push_back(Diagnostic(EToolError::ToolUnavailable, "Tool is unavailable in the current state."));
				if (Availability) *Availability = Available;
				if (!Gate->IsOpen() || Entry.Context.Cancellation.IsCancellationRequested() || Entry.Context.Client.Cancellation.IsCancellationRequested()) return ReadyToolResult(Failure(EToolError::Cancelled, "Tool cancelled before execution."));
				if (!Available.bAvailable) return ReadyToolResult({EToolStatus::Failed, {}, std::move(Available.Diagnostics)});
				if (!HostReady(Host)) return ReadyToolResult(Failure(EToolError::Busy, "Editor left the dispatch safe point during availability checking."));
				if (AvailabilityOnly) return ReadyToolResult({EToolStatus::Succeeded});
				return Toolset.Call(Entry.Request, Entry.Context);
			});
			if (Call.Status == EFeatureInvokeStatus::Unavailable) return ReadyToolResult(Failure(EToolError::ToolUnavailable, "The admitted toolset is no longer registered."));
			if (!Call.WasInvoked() || !Call.Value) return ReadyToolResult(Failure(EToolError::InternalError, "Toolset threw an exception."));
			return std::move(*Call.Value);
		}
		auto Resolve(FInvocation& Entry, bool AvailabilityOnly = false) -> FToolResult
		{
			if (!Entry.Raw.IsValid()) return Failure(EToolError::InvalidOutput, "Provider returned an invalid Future.");
			if (!Entry.Raw.IsReady()) return Failure(EToolError::InvalidOutput, "Synchronous provider returned a pending Future; effects are not rolled back.");
			if (auto Value = Entry.Raw.GetResultShared()) return ValidateResult(Entry, *Value, AvailabilityOnly);
			if (Entry.Raw.GetState() == EFutureState::Canceled) return Failure(EToolError::Cancelled, "Provider acknowledged cancellation.");
			return Failure(EToolError::InternalError, "Provider completion failed or was abandoned.");
		}
		auto ReserveTerminal(FInvocation& Entry) -> bool
		{
			Prune();
			// Receipt ownership can be dropped under pressure; consumer pins remain charged.
			while (!Completed.empty() && (Accounting->Count >= Limits.MaxRetained
				|| TerminalReserve > Limits.MaxCompletedBytes - std::min(Limits.MaxCompletedBytes, Accounting->Bytes.load())))
			{
				const auto Id = Completed.front(); Completed.pop_front();
				CompletedBytes -= Invocations.at(Id)->Bytes; Invocations.erase(Id);
			}
			if (Accounting->Count >= Limits.MaxRetained
				|| TerminalReserve > Limits.MaxCompletedBytes - std::min(Limits.MaxCompletedBytes, Accounting->Bytes.load())) return false;
			Entry.Charge = std::make_shared<FCharge>(Accounting);
			Entry.Future = Entry.Future.WithLifetime(Entry.Charge);
			return true;
		}
		// Caller holds Mutex; provider execution has already returned.
		auto FinishDispatch(uint64 Id, FToolFuture Raw) -> void
		{
			auto& Entry = *Invocations.at(Id);
			Entry.Raw = std::move(Raw); Entry.Dispatching = false;
			if (!Open() && Entry.Raw.IsValid() && !Entry.Raw.IsReady())
			{ Complete(Id, Failure(EToolError::Cancelled, "Editor closed during dispatch; pending completion detached."), EToolInvocationState::Cancelled); return; }
			if (!Entry.Raw.IsValid() || Entry.Raw.IsReady() || !Entry.Route->Descriptor.bMayPend) Complete(Id, Resolve(Entry));
			else Entry.Observation = Entry.Raw.ObserveReady(Entry.Ready);
		}
		auto ValidateResult(const FInvocation& Entry, const FToolResult& Result, bool AvailabilityOnly = false) -> FToolResult
		{
			if (!ValidDiagnostics(Result.Diagnostics)) return Failure(EToolError::LimitExceeded, "Toolset result diagnostics exceed the contract.");
			if (Result.Status != EToolStatus::Succeeded && Result.Status != EToolStatus::Failed)
				return Failure(EToolError::InvalidOutput, "Invalid terminal tool status.");
			if (auto Check = ValidateToolValue(Result.Data); !Check) return Failure(EToolError::InvalidOutput, Check.error().Message);
			if (!AvailabilityOnly && Result.Status == EToolStatus::Succeeded)
				if (auto Check = ValidateToolOutput(Entry.Route->Descriptor, Result.Data); !Check)
				{
					auto Item = Check.error();
					if (Item.Code != EToolError::LimitExceeded) Item.Code = EToolError::InvalidOutput;
					return {EToolStatus::Failed, {}, {BoundedDiagnostic(std::move(Item))}};
				}
			// Bound raw trees before sizing/copying; unused reserved capacity is still charged.
			if (ResultBytes(Result) > Limits.MaxCompletedBytes) return Failure(EToolError::LimitExceeded, "Toolset result exceeds the service byte budget.");
			auto Validated = Result;
			if (Validated.Status == EToolStatus::Failed && Validated.Diagnostics.empty()) Validated.Diagnostics.push_back(Diagnostic(EToolError::DomainFailure, "Tool failed without a diagnostic."));
			if (ResultBytes(Validated) > Limits.MaxCompletedBytes) return Failure(EToolError::LimitExceeded, "Toolset result exceeds the service byte budget.");
			return Validated;
		}
	};

	auto GetToolEditor(const FToolContext& Context) -> DEditorEngine*
	{
		return IsInGameThread() && GBinding && &GBinding->Context == &Context ? GEditor : nullptr;
	}
	FEditorToolService::FEditorToolService(DEditorEngine& Editor, std::shared_ptr<const FSubsystemWorkGate> Gate)
		: Impl(std::make_unique<FImpl>(Editor, std::move(Gate))) {}
	FEditorToolService::~FEditorToolService() = default;
	auto FEditorToolService::CanDispatch() const -> bool
	{
		require(IsInGameThread());
		std::scoped_lock Lock(Impl->Mutex);
		if (Impl->Executing || !Impl->Open()) return false;
		auto* Editor = Impl->Editor.Get();
		return Editor && HostReady(*Editor);
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
				if (ValidateToolsetName(Set.Name))
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
	auto FEditorToolService::Submit(const FToolRequest& Request, EToolSource Source, const FToolClientContext& Client) -> std::expected<FEditorToolInvocationId, FToolDiagnostic>
	{
		if (Client.Id.size() > 128 || Client.Id.find('\0') != std::string::npos)
			return std::unexpected(Diagnostic(EToolError::InvalidArguments, "Invalid tool client identity."));
		if (Client.Cancellation.IsCancellationRequested())
			return std::unexpected(Diagnostic(EToolError::Cancelled, "Tool client session is closed."));
		if (auto Check = ValidateRequest(Request, Source); !Check) return std::unexpected(BoundedDiagnostic(Check.error()));
		std::scoped_lock Lock(Impl->Mutex);
		Impl->Prune();
		if (!Impl->Open()) { Impl->CloseLocked(); return std::unexpected(Diagnostic(EToolError::ToolUnavailable, "Editor tool service is closed.")); }
		auto Admitted = Impl->Admit(Request, Source);
		if (!Admitted) return std::unexpected(Admitted.error());
		auto Entry = std::move(*Admitted);
		const auto Bytes = Entry->Bytes;
		const bool DetachedControl = Impl->ScriptExecuting && Impl->PythonControl && Request.Address.Toolset == "editor.python"
			&& (Request.Address.Tool == "status" || Request.Address.Tool == "cancel")
			&& Entry->Route->ToolsetIdentity == Impl->PythonControlIdentity;
		if (Bytes > Impl->Limits.MaxQueuedBytes || Bytes > Impl->Limits.MaxPendingBytes || (!DetachedControl
			&& (Impl->Queue.size() >= Impl->Limits.MaxQueued || Bytes > Impl->Limits.MaxQueuedBytes - Impl->QueuedBytes)))
			return std::unexpected(Diagnostic(EToolError::Busy, "Tool queue capacity exceeded."));
		if (!Impl->ReserveTerminal(*Entry)) return std::unexpected(Diagnostic(EToolError::Busy, "Live tool result capacity exceeded."));
		const auto Id = Impl->NextSequence++;
		Impl->SetContext(*Entry, Id, Source);
		Entry->Context.Client = Client;
		// During a script, only these two controls bypass Tick. No provider or
		// DObject is called: the pump's feature scope pins the published code.
		if (DetachedControl)
		{
			FToolResult Result;
			try { Result = Impl->PythonControl(Entry->Request, Entry->Context); }
			catch (...) { Result = Failure(EToolError::InternalError, "Python job control failed."); }
			Result = Impl->ValidateResult(*Entry, std::move(Result));
			Impl->Invocations.emplace(Id, std::move(Entry));
			Impl->Complete(Id, std::move(Result));
		}
		else
		{
			Impl->Invocations.emplace(Id, std::move(Entry)); Impl->Queue.push_back(Id); Impl->QueuedBytes += Bytes;
		}
		return FEditorToolInvocationId{Impl->Session, Id};
	}
	auto FEditorToolService::GetInvocation(FEditorToolInvocationId Id) -> std::expected<FEditorToolInvocation, FToolDiagnostic>
	{
		std::scoped_lock Lock(Impl->Mutex);
		if (!Impl->Open()) Impl->CloseLocked();
		Impl->Prune();
		if (Id.Session != Impl->Session || Id.Sequence == 0 || Id.Sequence >= Impl->NextSequence) return std::unexpected(Diagnostic(EToolError::TargetNotFound, "Unknown invocation identity."));
		const auto Found = Impl->Invocations.find(Id.Sequence);
		if (Found == Impl->Invocations.end()) return FEditorToolInvocation{EToolInvocationState::Expired,
			ReadyToolResult(Failure(EToolError::InvocationExpired, "Invocation result expired.")).GetResultShared()};
		auto Result = Found->second->Future.GetResultShared();
		return FEditorToolInvocation{Found->second->State, std::move(Result)};
	}
	auto FEditorToolService::RetainInvocation(FEditorToolInvocationId Id) -> std::expected<FToolFuture, FToolDiagnostic>
	{
		std::scoped_lock Lock(Impl->Mutex);
		if (!Impl->Open()) Impl->CloseLocked();
		Impl->Prune();
		if (Id.Session != Impl->Session || !Id.Sequence || Id.Sequence >= Impl->NextSequence)
			return std::unexpected(Diagnostic(EToolError::TargetNotFound, "Unknown invocation identity."));
		const auto Found = Impl->Invocations.find(Id.Sequence);
		if (Found == Impl->Invocations.end()) return std::unexpected(Diagnostic(EToolError::InvocationExpired, "Invocation result expired."));
		return Found->second->Future;
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
		if (!Limits.MaxPending || !Limits.MaxPendingBytes || !Limits.MaxRetained || !Limits.MaxQueued || !Limits.MaxCompleted || !Limits.MaxCatalogTools || !Limits.MaxCatalogToolsets || !Limits.MaxDispatchPerTick
			|| !Limits.MaxCatalogBytes || !Limits.MaxQueuedBytes || !Limits.MaxCompletedBytes || Limits.TickBudget.count() <= 0 || Limits.ResultTTL.count() < 0)
			return std::unexpected(Diagnostic(EToolError::InvalidArguments, "Invalid tool service limits."));
		std::scoped_lock Lock(Impl->Mutex);
		if (!Impl->Open()) return std::unexpected(Diagnostic(EToolError::ToolUnavailable, "Tool service closed."));
		if (Impl->Executing || !Impl->Queue.empty() || Impl->PendingCount) return std::unexpected(Diagnostic(EToolError::Busy, "Configure requires an idle tool service."));
		if (Impl->Accounting->Count > Limits.MaxRetained || Impl->Accounting->Bytes > Limits.MaxCompletedBytes)
			return std::unexpected(Diagnostic(EToolError::Busy, "Configure would undercount live consumer results."));
		Impl->Limits = Limits; Impl->HasCatalog = false;
		Impl->Routes.clear(); Impl->Toolsets.clear(); Impl->CatalogDiagnostics.clear(); ++Impl->CatalogRevision;
		Impl->Prune(); return {};
	}
	auto FEditorToolService::Call(const FToolRequest& Request, EToolSource Source,
		bool AvailabilityOnly, FToolAvailability* Availability, const FToolClientContext& Client,
		FToolCancellationToken Cancellation) -> FToolResult
	{
		if (Client.Id.size() > 128 || Client.Id.find('\0') != std::string::npos)
			return Failure(EToolError::InvalidArguments, "Invalid tool client identity.");
		if (!CanDispatch()) return Failure(EToolError::Busy, "Editor tool dispatch is not at an idle safe point.");
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
			Entry->Context.Client = Client; Entry->Context.Cancellation = Cancellation;
			Entry->State = EToolInvocationState::Running;
		}
		if (!AvailabilityOnly && Entry->Route->Descriptor.bMayPend) return Failure(EToolError::Busy, "CallNow rejects tools that may pend before provider dispatch.");
		Entry->Raw = Impl->Run(*Impl->Editor.Get(), *Entry, AvailabilityOnly, Availability);
		return Impl->Resolve(*Entry, AvailabilityOnly);
	}
	auto FEditorToolService::SubmitNow(const FToolRequest& Request, EToolSource Source,
		const FToolClientContext& Client, FToolCancellationToken Cancellation) -> std::expected<FEditorToolSubmission, FToolDiagnostic>
	{
		if (!CanDispatch()) return std::unexpected(Diagnostic(EToolError::Busy, "Editor is outside the tool safe point."));
		if (Client.Id.size() > 128 || Client.Id.find('\0') != std::string::npos)
			return std::unexpected(Diagnostic(EToolError::InvalidArguments, "Invalid client identity."));
		if (Client.Cancellation.IsCancellationRequested() || Cancellation.IsCancellationRequested())
			return std::unexpected(Diagnostic(EToolError::Cancelled, "Invocation owner is cancelled."));
		if (auto Check = ValidateRequest(Request, Source); !Check) return std::unexpected(BoundedDiagnostic(Check.error()));
		FExecutingScope Scope(Impl->Executing); RefreshCatalog();
		FImpl::FInvocation* Entry;
		FEditorToolSubmission Submission;
		{
			std::scoped_lock Lock(Impl->Mutex);
			auto Admitted = Impl->Admit(Request, Source); if (!Admitted) return std::unexpected(Admitted.error());
			auto Owned = std::move(*Admitted);
			if (Impl->PendingCount >= Impl->Limits.MaxPending || Owned->Bytes > Impl->Limits.MaxPendingBytes - Impl->PendingBytes
				|| Owned->Bytes > Impl->Limits.MaxQueuedBytes || !Impl->ReserveTerminal(*Owned))
				return std::unexpected(Diagnostic(EToolError::Busy, "Invocation capacity exceeded before dispatch."));
			Submission.Id = {Impl->Session, Impl->NextSequence++};
			Impl->SetContext(*Owned, Submission.Id.Sequence, Source); Owned->Context.Client = Client;
			Owned->Context.Cancellation = Owned->Cancellation.GetToken().WithParent(Cancellation);
			Owned->State = EToolInvocationState::Running; Owned->Dispatching = true;
			++Impl->PendingCount; Impl->PendingBytes += Owned->Bytes;
			Submission.Future = Owned->Future; Entry = Owned.get();
			Impl->Invocations.emplace(Submission.Id.Sequence, std::move(Owned));
		}
		auto Raw = Impl->Run(*Impl->Editor.Get(), *Entry, false, nullptr);
		{ std::scoped_lock Lock(Impl->Mutex); Impl->FinishDispatch(Submission.Id.Sequence, std::move(Raw)); }
		return Submission;
	}
	auto FEditorToolService::Drain() -> void
	{
		if (!CanDispatch()) return;
		FExecutingScope Scope(Impl->Executing);
		RefreshCatalog();
		FEditorToolLimits Limits;
		{ std::scoped_lock Lock(Impl->Mutex); Limits = Impl->Limits; Impl->Prune(); }
		// Producer notifications only mark readiness. Final validation/publication belongs here.
		{
			std::scoped_lock Lock(Impl->Mutex);
			std::vector<uint64> Ready;
			for (auto& [Id, Entry] : Impl->Invocations)
				if (Entry->State == EToolInvocationState::Running && Entry->Ready.IsReady() && Entry->Raw.IsReady()) Ready.push_back(Id);
			for (auto Id : Ready) Impl->Complete(Id, Impl->Resolve(*Impl->Invocations.at(Id)));
		}
		const auto Start = FClock::now();
		for (size_t Count = 0; Count < Limits.MaxDispatchPerTick && (Count == 0 || FClock::now() - Start < Limits.TickBudget); ++Count)
		{
			if (!HostReady(*Impl->Editor.Get())) break;
			FImpl::FInvocation* Entry;
			uint64 Id;
			{
				std::scoped_lock Lock(Impl->Mutex);
				if (!Impl->Open()) { Impl->CloseLocked(); break; }
				if (Impl->Queue.empty()) break;
				if (Impl->PendingCount >= Limits.MaxPending || Impl->Invocations.at(Impl->Queue.front())->Bytes > Limits.MaxPendingBytes - Impl->PendingBytes) break;
				Id = Impl->Queue.front(); Impl->Queue.pop_front();
				Entry = Impl->Invocations.at(Id).get(); Impl->QueuedBytes -= Entry->Bytes;
				++Impl->PendingCount; Impl->PendingBytes += Entry->Bytes; Entry->Dispatching = true;
				Entry->State = EToolInvocationState::Running;
			}
			auto Raw = Impl->Run(*Impl->Editor.Get(), *Entry, false, nullptr);
			std::scoped_lock Lock(Impl->Mutex);
			Impl->FinishDispatch(Id, std::move(Raw));
		}
	}
	auto FEditorToolService::GetWorkGate() const -> std::shared_ptr<const FSubsystemWorkGate> { return Impl->Gate; }
	auto FEditorToolService::BindPythonJobControl(uint64 Identity, FPythonJobControl Control) -> void
	{
		require(IsInGameThread());
		std::scoped_lock Lock(Impl->Mutex);
		if (!Impl->Open()) return;
		Impl->PythonControlIdentity = Identity; Impl->PythonControl = std::move(Control);
	}
	auto FEditorToolService::PumpScripts(const std::shared_ptr<FEditorToolService>& Endpoint) -> void
	{
		if (!CanDispatch()) return;
		RefreshCatalog();
		{
			std::scoped_lock Lock(Impl->Mutex);
			if (Impl->ScriptExecuting) return;
			Impl->ScriptExecuting = true;
		}
		(void)FModularFeatureRegistry::Get().InvokeAll<IEditorScriptPump>([&](auto& Pump) {
			if (CanDispatch()) Pump.Pump(*Impl->Editor.Get(), Endpoint);
		});
		std::scoped_lock Lock(Impl->Mutex); Impl->ScriptExecuting = false;
	}
	auto FEditorToolService::Close() noexcept -> void { std::scoped_lock Lock(Impl->Mutex); Impl->CloseLocked(); }
}

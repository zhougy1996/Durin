#include "AssetBuild/BuildCache.h"
#include "AssetBuild/BuildHost.h"
#include "AssetBuild/BuildRegistry.h"

namespace Durin::Asset::Build
{
	namespace
	{
		auto SetError(std::string* OutError, std::string Message) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto IsCanonicalIdentityPart(std::string_view Value) -> bool
		{
			if (Value.empty()) return false;
			return std::ranges::all_of(Value, [](char Character) {
				return (Character >= 'a' && Character <= 'z')
					|| (Character >= 'A' && Character <= 'Z')
					|| (Character >= '0' && Character <= '9')
					|| Character == '.' || Character == '_' || Character == '-';
			});
		}

		struct FRegisteredFunction
		{
			std::shared_ptr<FModuleOwnedResourceLease> RegistryResource;
			FLocalBuildFunction Function;
			std::shared_ptr<FBuildRequestOwner> Owner;
			FModuleOwnedCallbackGate OwnerGate;
			uint64 Generation = 0;
		};

		std::mutex GFunctionMutex;
		std::unordered_map<std::string, FRegisteredFunction> GFunctions;
		uint64 GNextFunctionGeneration = 1;

		struct FRegisteredService
		{
			FModuleOwnedResourceLease RegistryResource;
			FBuildServiceContribution Contribution;
			uint64 Generation = 0;
			bool bStarted = false;
		};

		std::mutex GHostMutex;
		std::unordered_map<std::string, FRegisteredService> GServices;
		uint64 GNextServiceGeneration = 1;
		bool GHostRunning = false;
	}

	struct FBuildRequestOwner::FState
	{
		mutable std::mutex Mutex;
		std::condition_variable Condition;
		uint64 AcceptedRequestCount = 0;
		uint64 CompletedRequestCount = 0;
		uint32 ActiveRequestCount = 0;
		bool bAcceptingRequests = true;
		bool bCanceled = false;
		std::vector<std::string> Diagnostics;
	};

	auto FBuildValue::FromOwned(std::string InName, std::vector<uint8> InBytes)
		-> FBuildValue
	{
		FBuildValue Result;
		Result.Name = std::move(InName);
		Result.ContentIdentity = FXxHash128::HashBuffer(InBytes);
		Result.Bytes = std::make_shared<const std::vector<uint8>>(std::move(InBytes));
		return Result;
	}

	auto IsValidBuildFunctionIdentity(
		const FBuildFunctionIdentity& Identity, std::string* OutError) -> bool
	{
		if (!IsCanonicalIdentityPart(Identity.Owner))
			return SetError(OutError, "Build function owner is not canonical.");
		if (!IsCanonicalIdentityPart(Identity.Name))
			return SetError(OutError, "Build function name is not canonical.");
		return true;
	}

	auto BuildFunctionIdentityString(const FBuildFunctionIdentity& Identity) -> std::string
	{
		return Identity.Owner + "::" + Identity.Name;
	}

	auto ValidateBuildDefinition(
		const FBuildDefinition& Definition, std::string* OutError) -> bool
	{
		if (!IsValidBuildFunctionIdentity(Definition.Function, OutError)) return false;
		if (Definition.ImplementationIdentity.empty())
			return SetError(OutError, "Build implementation identity is empty.");
		if (Definition.RecipeIdentity.empty())
			return SetError(OutError, "Build recipe identity is empty.");
		if (Definition.TargetPlatform.empty() || Definition.TargetProfile.empty())
			return SetError(OutError, "Build target facts are incomplete.");
		std::unordered_set<std::string> Names;
		for (const FBuildValue& Input : Definition.Inputs)
		{
			if (!Input.IsValid()) return SetError(OutError, "Build input is invalid.");
			if (!Names.insert(std::string(Input.GetName())).second)
				return SetError(OutError, "Build input names must be unique.");
		}
		return true;
	}

	FBuildRequestOwner::FBuildRequestOwner()
		: State(std::make_shared<FState>())
	{
	}

	FBuildRequestOwner::~FBuildRequestOwner()
	{
		Cancel();
		Wait(30.0);
	}

	auto BeginBuildRequest(FBuildRequestOwner& Owner) -> bool
	{
		std::lock_guard Lock(Owner.State->Mutex);
		if (!Owner.State->bAcceptingRequests || Owner.State->bCanceled) return false;
		++Owner.State->AcceptedRequestCount;
		++Owner.State->ActiveRequestCount;
		return true;
	}

	auto CompleteBuildRequest(FBuildRequestOwner& Owner, std::string_view Diagnostic) -> void
	{
		std::lock_guard Lock(Owner.State->Mutex);
		if (Owner.State->ActiveRequestCount > 0) --Owner.State->ActiveRequestCount;
		++Owner.State->CompletedRequestCount;
		if (!Diagnostic.empty()) Owner.State->Diagnostics.emplace_back(Diagnostic);
		Owner.State->Condition.notify_all();
	}

	auto FBuildRequestOwner::CloseAdmission() -> void
	{
		std::lock_guard Lock(State->Mutex);
		State->bAcceptingRequests = false;
	}

	auto FBuildRequestOwner::Cancel() -> void
	{
		std::lock_guard Lock(State->Mutex);
		State->bAcceptingRequests = false;
		State->bCanceled = true;
		State->Condition.notify_all();
	}

	auto FBuildRequestOwner::IsCanceled() const -> bool
	{
		std::lock_guard Lock(State->Mutex);
		return State->bCanceled;
	}

	auto FBuildRequestOwner::Wait(double TimeoutSeconds) -> bool
	{
		std::unique_lock Lock(State->Mutex);
		return State->Condition.wait_for(Lock,
			std::chrono::duration<double>(std::max(TimeoutSeconds, 0.0)),
			[this] { return State->ActiveRequestCount == 0; });
	}

	auto FBuildRequestOwner::GetSnapshot() const -> FBuildRequestOwnerSnapshot
	{
		std::lock_guard Lock(State->Mutex);
		return {
			.AcceptedRequestCount = State->AcceptedRequestCount,
			.CompletedRequestCount = State->CompletedRequestCount,
			.ActiveRequestCount = State->ActiveRequestCount,
			.bAcceptingRequests = State->bAcceptingRequests,
			.bCanceled = State->bCanceled,
			.Diagnostics = State->Diagnostics};
	}

	FBuildFunctionRegistration::~FBuildFunctionRegistration() { Reset(); }

	FBuildFunctionRegistration::FBuildFunctionRegistration(
		FBuildFunctionRegistration&& Other) noexcept
		: Identity(std::move(Other.Identity)), Generation(std::exchange(Other.Generation, 0)),
		  Owner(std::move(Other.Owner))
	{
	}

	auto FBuildFunctionRegistration::operator=(
		FBuildFunctionRegistration&& Other) noexcept -> FBuildFunctionRegistration&
	{
		if (this == &Other) return *this;
		Reset();
		Identity = std::move(Other.Identity);
		Generation = std::exchange(Other.Generation, 0);
		Owner = std::move(Other.Owner);
		return *this;
	}

	auto FBuildFunctionRegistration::Reset() -> void
	{
		if (Generation == 0) return;
		if (Owner)
		{
			Owner->CloseAdmission();
			Owner->Cancel();
			Owner->Wait(30.0);
		}
		std::lock_guard Lock(GFunctionMutex);
		const std::string Key = BuildFunctionIdentityString(Identity);
		const auto It = GFunctions.find(Key);
		if (It != GFunctions.end() && It->second.Generation == Generation)
			GFunctions.erase(It);
		Generation = 0;
		Owner.reset();
	}

	auto RegisterLocalBuildFunction(
		FBuildFunctionIdentity Identity, FLocalBuildFunction Function,
		FModuleOwnedCallbackGate OwnerGate,
		std::shared_ptr<FBuildRequestOwner> Owner, std::string* OutError)
		-> FBuildFunctionRegistration
	{
		if (!IsValidBuildFunctionIdentity(Identity, OutError)) return {};
		if (!Function) return SetError(OutError, "Local Build function is empty."), FBuildFunctionRegistration{};
		if (!OwnerGate.IsValid())
			return SetError(OutError, "Local Build function module owner gate is invalid."), FBuildFunctionRegistration{};
		if (!Owner) Owner = std::make_shared<FBuildRequestOwner>();
		const std::string Key = BuildFunctionIdentityString(Identity);
		std::lock_guard Lock(GFunctionMutex);
		if (GFunctions.contains(Key))
			return SetError(OutError, "Build function identity is already registered."), FBuildFunctionRegistration{};
		auto Resource = OwnerGate.RetainResource();
		if (!Resource)
			return SetError(OutError, "Local Build function owner is retiring."), FBuildFunctionRegistration{};
		const uint64 Generation = GNextFunctionGeneration++;
		GFunctions.emplace(Key, FRegisteredFunction{
			std::make_shared<FModuleOwnedResourceLease>(std::move(Resource)),
			std::move(Function), Owner, std::move(OwnerGate), Generation});
		FBuildFunctionRegistration Registration;
		Registration.Identity = std::move(Identity);
		Registration.Generation = Generation;
		Registration.Owner = std::move(Owner);
		return Registration;
	}

	auto ExecuteLocalBuildFunction(
		const FBuildDefinition& Definition, const FBuildPolicy& Policy,
		FBuildFunctionResult& OutResult, FBuildTerminalCallback TerminalCallback,
		std::string* OutError) -> bool
	{
		if (!ValidateBuildDefinition(Definition, OutError)) return false;
		FRegisteredFunction Entry;
		FModuleOwnedResourceLease Lifetime;
		{
			std::lock_guard Lock(GFunctionMutex);
			const auto It = GFunctions.find(BuildFunctionIdentityString(Definition.Function));
			if (It == GFunctions.end()) return SetError(OutError, "Build function is not registered.");
			Lifetime = It->second.OwnerGate.RetainResource();
			if (!Lifetime) return SetError(OutError, "Build function owner is retiring.");
			Entry = It->second;
		}
		auto Invocation = Entry.OwnerGate.TryEnter();
		if (!Invocation) return SetError(OutError, "Build function owner is retiring.");
		if (!BeginBuildRequest(*Entry.Owner)) return SetError(OutError, "Build request owner is not accepting requests.");
		try
		{
			OutResult = Entry.Function(Definition, Policy, *Entry.Owner);
		}
		catch (const std::exception& Error)
		{
			OutResult = {.Diagnostic = Error.what()};
		}
		catch (...)
		{
			OutResult = {.Diagnostic = "Local Build function threw an unknown exception."};
		}
		CompleteBuildRequest(*Entry.Owner, OutResult.Diagnostic);
		Entry.Function = {};
		if (TerminalCallback) TerminalCallback(OutResult);
		return true;
	}

	auto IsLocalBuildFunctionRegistered(const FBuildFunctionIdentity& Identity) -> bool
	{
		std::lock_guard Lock(GFunctionMutex);
		return GFunctions.contains(BuildFunctionIdentityString(Identity));
	}

	auto GetRegisteredLocalBuildFunctionCount() -> uint32
	{
		std::lock_guard Lock(GFunctionMutex);
		return static_cast<uint32>(GFunctions.size());
	}

	FBuildCacheClient::FBuildCacheClient(Asset::FDerivedDataObjectStore& InStore)
		: StoreTarget(&InStore)
	{
	}

	auto FBuildCacheClient::Query(
		std::string_view Key, std::string ValueName,
		const FBuildPolicy& Policy) const -> FBuildCacheQueryResult
	{
		if (!Policy.bQueryCache) return {};
		std::vector<uint8> Bytes;
		const Asset::FDerivedDataObjectReadResult Read = StoreTarget->Read(Key, Bytes);
		if (Read.Status == Asset::EDerivedDataObjectReadStatus::Hit)
			return {EBuildCacheQueryStatus::Hit,
				FBuildValue::FromOwned(std::move(ValueName), std::move(Bytes)), {}};
		if (Read.Status == Asset::EDerivedDataObjectReadStatus::Missing)
			return {EBuildCacheQueryStatus::Missing, {}, Read.Message};
		return {EBuildCacheQueryStatus::StorageError, {}, Read.Message};
	}

	auto FBuildCacheClient::Store(
		std::string_view Key, const FBuildValue& Value,
		const FBuildPolicy& Policy, std::string* OutError) const -> bool
	{
		if (!Policy.bStoreBuildResult) return true;
		std::string Error;
		const bool bStored = Value.IsValid() && StoreTarget->Write(Key, Value.GetBytes(), &Error);
		if (!bStored && Policy.bRequireStoreSuccess) return SetError(OutError, std::move(Error));
		if (!bStored && OutError) *OutError = std::move(Error);
		return bStored || !Policy.bRequireStoreSuccess;
	}

	FBuildServiceRegistration::~FBuildServiceRegistration() { Reset(); }

	FBuildServiceRegistration::FBuildServiceRegistration(
		FBuildServiceRegistration&& Other) noexcept
		: Identity(std::move(Other.Identity)), Generation(std::exchange(Other.Generation, 0))
	{
	}

	auto FBuildServiceRegistration::operator=(
		FBuildServiceRegistration&& Other) noexcept -> FBuildServiceRegistration&
	{
		if (this == &Other) return *this;
		Reset();
		Identity = std::move(Other.Identity);
		Generation = std::exchange(Other.Generation, 0);
		return *this;
	}

	auto FBuildServiceRegistration::Reset() -> void
	{
		if (Generation == 0) return;
		FRegisteredService Service;
		bool bFound = false;
		{
			std::lock_guard Lock(GHostMutex);
			const auto It = GServices.find(Identity);
			if (It != GServices.end() && It->second.Generation == Generation)
			{
				Service = std::move(It->second);
				GServices.erase(It);
				bFound = true;
			}
		}
		if (bFound && Service.bStarted)
		{
			if (Service.Contribution.StopAdmission) Service.Contribution.StopAdmission();
			if (Service.Contribution.Wait) Service.Contribution.Wait(30.0);
			if (Service.Contribution.Drain) Service.Contribution.Drain();
		}
		Generation = 0;
	}

	auto RegisterBuildServiceContribution(
		FBuildServiceContribution Contribution, std::string* OutError)
		-> FBuildServiceRegistration
	{
		if (!IsCanonicalIdentityPart(Contribution.Identity))
			return SetError(OutError, "Build service identity is not canonical."), FBuildServiceRegistration{};
		if (!Contribution.OwnerGate.IsValid())
			return SetError(OutError, "Build service module owner gate is invalid."), FBuildServiceRegistration{};
		std::lock_guard Lock(GHostMutex);
		if (GServices.contains(Contribution.Identity))
			return SetError(OutError, "Build service identity is already registered."), FBuildServiceRegistration{};
		FModuleOwnedResourceLease Resource = Contribution.OwnerGate.RetainResource();
		if (!Resource)
			return SetError(OutError, "Build service module owner is retiring."), FBuildServiceRegistration{};
		const uint64 Generation = GNextServiceGeneration++;
		const std::string Identity = Contribution.Identity;
		FRegisteredService Service{
			std::move(Resource), std::move(Contribution), Generation, false};
		if (GHostRunning && Service.Contribution.Start)
		{
			auto Invocation = Service.Contribution.OwnerGate.TryEnter();
			if (!Invocation || !Service.Contribution.Start())
				return SetError(OutError, "Build service failed to start."), FBuildServiceRegistration{};
			Service.bStarted = true;
		}
		GServices.emplace(Identity, std::move(Service));
		FBuildServiceRegistration Registration;
		Registration.Identity = Identity;
		Registration.Generation = Generation;
		return Registration;
	}

	auto InitializeBuildHost(std::string* OutError) -> bool
	{
		std::lock_guard Lock(GHostMutex);
		if (GHostRunning) return true;
		std::vector<FRegisteredService*> Started;
		for (auto& [Identity, Service] : GServices)
		{
			auto Invocation = Service.Contribution.OwnerGate.TryEnter();
			if (!Invocation || (Service.Contribution.Start && !Service.Contribution.Start()))
			{
				for (FRegisteredService* Item : Started)
				{
					if (Item->Contribution.StopAdmission) Item->Contribution.StopAdmission();
					if (Item->Contribution.Drain) Item->Contribution.Drain();
					Item->bStarted = false;
				}
				return SetError(OutError, "Build service failed during host startup.");
			}
			Service.bStarted = true;
			Started.push_back(&Service);
		}
		GHostRunning = true;
		return true;
	}

	auto PumpBuildHostCompletions(uint32 MaximumCount) -> uint32
	{
		struct FPump
		{
			FModuleOwnedResourceLease Lifetime;
			FModuleOwnedCallbackGate Gate;
			std::function<uint32(uint32)> Callback;
		};
		std::vector<FPump> Pumps;
		{
			std::lock_guard Lock(GHostMutex);
			if (!GHostRunning) return 0;
			for (const auto& [Identity, Service] : GServices)
				if (Service.bStarted && Service.Contribution.PumpCompletions)
				{
					auto Lifetime = Service.Contribution.OwnerGate.RetainResource();
					if (Lifetime) Pumps.push_back({std::move(Lifetime),
						Service.Contribution.OwnerGate, Service.Contribution.PumpCompletions});
				}
		}
		uint32 Pumped = 0;
		for (auto& Pump : Pumps)
		{
			if (Pumped >= MaximumCount) break;
			auto Invocation = Pump.Gate.TryEnter();
			if (Invocation) Pumped += Pump.Callback(MaximumCount - Pumped);
			Pump.Callback = {};
		}
		return Pumped;
	}

	auto WaitForBuildHost(double TimeoutSeconds) -> bool
	{
		struct FWait
		{
			FModuleOwnedResourceLease Lifetime;
			FModuleOwnedCallbackGate Gate;
			std::function<bool(double)> Callback;
		};
		std::vector<FWait> Waits;
		{
			std::lock_guard Lock(GHostMutex);
			for (const auto& [Identity, Service] : GServices)
				if (Service.bStarted && Service.Contribution.Wait)
				{
					auto Lifetime = Service.Contribution.OwnerGate.RetainResource();
					if (Lifetime) Waits.push_back({std::move(Lifetime),
						Service.Contribution.OwnerGate, Service.Contribution.Wait});
				}
		}
		const auto Deadline = std::chrono::steady_clock::now()
			+ std::chrono::duration<double>(std::max(TimeoutSeconds, 0.0));
		for (auto& Wait : Waits)
		{
			const double Remaining = std::chrono::duration<double>(
				Deadline - std::chrono::steady_clock::now()).count();
			auto Invocation = Wait.Gate.TryEnter();
			if (Remaining < 0.0 || !Invocation || !Wait.Callback(Remaining)) return false;
			Wait.Callback = {};
		}
		return true;
	}

	auto GetBuildHostSnapshot() -> FBuildHostSnapshot
	{
		std::lock_guard Lock(GHostMutex);
		FBuildHostSnapshot Result;
		Result.ServiceCount = static_cast<uint32>(GServices.size());
		Result.bAcceptingRequests = GHostRunning;
		for (const auto& [Identity, Service] : GServices)
		{
			if (!Service.bStarted || !Service.Contribution.Snapshot) continue;
			auto Invocation = Service.Contribution.OwnerGate.TryEnter();
			if (!Invocation) continue;
			const auto [Queued, Running, Bytes] = Service.Contribution.Snapshot();
			Result.QueuedRequestCount += Queued;
			Result.RunningRequestCount += Running;
			Result.InFlightEstimatedBytes += Bytes;
		}
		return Result;
	}

	auto ShutdownBuildHost() -> void
	{
		struct FShutdownService
		{
			FModuleOwnedResourceLease Lifetime;
			FBuildServiceContribution Contribution;
		};
		std::vector<FShutdownService> Services;
		{
			std::lock_guard Lock(GHostMutex);
			if (!GHostRunning) return;
			GHostRunning = false;
			for (auto& [Identity, Service] : GServices)
			{
				if (Service.bStarted)
				{
					auto Lifetime = Service.Contribution.OwnerGate.RetainResource();
					if (Lifetime) Services.push_back({std::move(Lifetime), Service.Contribution});
				}
				Service.bStarted = false;
			}
		}
		std::ranges::sort(Services, [](const auto& Left, const auto& Right) {
			return Left.Contribution.DrainOrder > Right.Contribution.DrainOrder;
		});
		for (auto& Service : Services)
		{
			auto Invocation = Service.Contribution.OwnerGate.TryEnter();
			if (Invocation && Service.Contribution.StopAdmission)
				Service.Contribution.StopAdmission();
		}
		for (auto& Service : Services)
		{
			auto Invocation = Service.Contribution.OwnerGate.TryEnter();
			if (Invocation && Service.Contribution.Wait)
				Service.Contribution.Wait(30.0);
		}
		for (auto& Service : Services)
		{
			auto Invocation = Service.Contribution.OwnerGate.TryEnter();
			if (Invocation && Service.Contribution.Drain)
				Service.Contribution.Drain();
			Service.Contribution = {};
		}
	}
}

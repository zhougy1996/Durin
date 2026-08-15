#include "AssetBuild/BuildSession.h"
#include "AssetBuild/BuildHost.h"
#include "DerivedDataObjectStore.h"

namespace Durin::Asset::Build
{
	namespace
	{
		auto SetError(std::string* OutError, std::string Message) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		enum class EBuildCacheQueryStatus : uint8
		{
			Hit,
			Missing,
			StorageError,
			Skipped
		};

		struct FBuildCacheQueryResult
		{
			EBuildCacheQueryStatus Status = EBuildCacheQueryStatus::Skipped;
			FBuildValue Value;
			std::string Diagnostic;
		};

		// Adapts one session's opaque values to the physical AssetCore object store.
		class FBuildCacheClient
		{
		public:
			explicit FBuildCacheClient(Asset::FDerivedDataObjectStore& InStore)
				: StoreTarget(&InStore)
			{
			}

			auto Query(
				std::string_view Key, std::string ValueName,
				const FBuildCachePolicy& Policy) const -> FBuildCacheQueryResult
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

			auto Store(
				std::string_view Key, const FBuildValue& Value,
				const FBuildCachePolicy& Policy, std::string* OutError = nullptr) const -> bool
			{
				if (!Policy.bStoreBuildResult) return true;
				std::string Error;
				const bool bStored = Value.IsValid()
					&& StoreTarget->Write(Key, Value.GetBytes(), &Error);
				if (!bStored && Policy.bRequireStoreSuccess)
					return SetError(OutError, std::move(Error));
				if (!bStored && OutError) *OutError = std::move(Error);
				return bStored || !Policy.bRequireStoreSuccess;
			}

		private:
			Asset::FDerivedDataObjectStore* StoreTarget = nullptr;
		};

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

		struct FRegisteredBuildFunction
		{
			std::shared_ptr<IBuildFunction> Function;
			FModuleOwnedCallbackGate Gate;
			FModuleOwnedResourceLease Resource;
			uint64 Generation = 0;
		};
		std::mutex GFunctionMutex;
		std::unordered_map<std::string, FRegisteredBuildFunction> GFunctions;
		uint64 GNextFunctionGeneration = 1;

		auto FunctionKey(const FBuildFunctionIdentity& Identity) -> std::string
		{
			return std::format("{}@{}", Identity.Name, Identity.Version);
		}

		auto Fail(EBuildFailurePhase Phase, std::string Message) -> FBuildOutput
		{
			return {.Status = EBuildStatus::Failed, .FailurePhase = Phase,
				.Diagnostic = std::move(Message)};
		}

		template<typename F>
		auto MeasureNanoseconds(uint64& OutNanoseconds, F&& Operation)
		{
			const auto Start = std::chrono::steady_clock::now();
			struct FStopwatch
			{
				std::chrono::steady_clock::time_point Start;
				uint64& Output;
				~FStopwatch()
				{
					Output = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - Start).count());
				}
			} Stopwatch{Start, OutNanoseconds};
			return std::forward<F>(Operation)();
		}
	}

	auto FBuildValue::FromOwned(std::string InName, std::vector<uint8> InBytes)
		-> FBuildValue
	{
		FBuildValue Result;
		Result.Name = std::move(InName);
		Result.ContentIdentity = FXxHash128::HashBuffer(InBytes);
		Result.Bytes = std::make_shared<const std::vector<uint8>>(std::move(InBytes));
		return Result;
	}

	auto FBuildKey::FromString(std::string_view InValue, std::string* OutError) -> FBuildKey
	{
		FBuildKey Result;
		if (InValue.size() != 32 || !std::ranges::all_of(InValue, [](char Character) {
			return Character >= '0' && Character <= '9'
				|| Character >= 'a' && Character <= 'f';
		}))
		{
			if (OutError) *OutError = "Build key must be a lowercase 128-bit hexadecimal identity.";
			return Result;
		}
		Result.Value.assign(InValue);
		if (OutError) OutError->clear();
		return Result;
	}

	auto FBuildDefinition::GetInput(std::string_view Name) const -> const FBuildValue*
	{
		const auto It = std::ranges::find(Inputs, Name, &FBuildValue::GetName);
		return It == Inputs.end() ? nullptr : &*It;
	}

	auto FBuildDefinition::GetTargetFact(std::string_view Name) const
		-> std::optional<std::string_view>
	{
		for (const auto& [FactName, Value] : TargetFacts)
			if (FactName == Name) return Value;
		return std::nullopt;
	}

	auto ParseBuildTargetFactUInt32(std::string_view Text, uint32& OutValue) -> bool
	{
		const auto [End, Error] = std::from_chars(
			Text.data(), Text.data() + Text.size(), OutValue);
		return Error == std::errc{} && End == Text.data() + Text.size();
	}

	FBuildDefinitionBuilder::FBuildDefinitionBuilder(
		FBuildFunctionIdentity InFunction, std::string InExpectedValueName)
	{
		Definition.Function = std::move(InFunction);
		Definition.ExpectedValueName = std::move(InExpectedValueName);
	}

	auto FBuildDefinitionBuilder::SetKey(
		FBuildKey InKey, std::span<const uint8> CanonicalKeyInput)
		-> FBuildDefinitionBuilder&
	{
		Definition.Key = std::move(InKey);
		KeyInput.assign(CanonicalKeyInput.begin(), CanonicalKeyInput.end());
		return *this;
	}

	auto FBuildDefinitionBuilder::AddInput(FBuildValue Value)
		-> FBuildDefinitionBuilder&
	{
		if (!Value.IsValid()) Error = "Build input is invalid.";
		else if (Definition.GetInput(Value.GetName())) Error = "Build input names must be unique.";
		else Definition.Inputs.push_back(std::move(Value));
		return *this;
	}

	auto FBuildDefinitionBuilder::AddTargetFact(std::string Name, std::string Value)
		-> FBuildDefinitionBuilder&
	{
		if (!IsCanonicalIdentityPart(Name) || Value.empty())
			Error = "Build target fact is invalid.";
		else if (std::ranges::find(Definition.TargetFacts, Name,
				[](const auto& Item) -> const std::string& { return Item.first; })
			!= Definition.TargetFacts.end()) Error = "Build target fact names must be unique.";
		else Definition.TargetFacts.emplace_back(std::move(Name), std::move(Value));
		return *this;
	}

	auto FBuildDefinitionBuilder::Build(
		FBuildDefinition& OutDefinition, std::string* OutError) const -> bool
	{
		std::string Validation = Error;
		if (Validation.empty() && (!Definition.Function.IsValid()
			|| !IsCanonicalIdentityPart(Definition.Function.Name)))
			Validation = "Build function identity is invalid.";
		if (Validation.empty() && !Definition.Key.IsValid()) Validation = "Build key is invalid.";
		if (Validation.empty() && !IsCanonicalIdentityPart(Definition.ExpectedValueName))
			Validation = "Expected build value name is invalid.";
		if (Validation.empty() && !KeyInput.empty()
			&& FXxHash128::HashBuffer(KeyInput).ToString() != Definition.Key.ToString())
			Validation = "Build key does not agree with canonical key input bytes.";
		if (!Validation.empty())
		{
			if (OutError) *OutError = std::move(Validation);
			return false;
		}
		OutDefinition = Definition;
		OutDefinition.bHasLocalInputs = !Definition.Inputs.empty();
		if (OutError) OutError->clear();
		return true;
	}

	FBuildFunctionRegistration::~FBuildFunctionRegistration() { Reset(); }
	FBuildFunctionRegistration::FBuildFunctionRegistration(
		FBuildFunctionRegistration&& Other) noexcept
		: Identity(std::move(Other.Identity)), Generation(std::exchange(Other.Generation, 0)) {}
	auto FBuildFunctionRegistration::operator=(FBuildFunctionRegistration&& Other) noexcept
		-> FBuildFunctionRegistration&
	{
		if (this != &Other)
		{
			Reset();
			Identity = std::move(Other.Identity);
			Generation = std::exchange(Other.Generation, 0);
		}
		return *this;
	}
	auto FBuildFunctionRegistration::Reset() -> void
	{
		if (!Generation) return;
		FRegisteredBuildFunction Removed;
		{
			std::lock_guard Lock(GFunctionMutex);
			const auto It = GFunctions.find(FunctionKey(Identity));
			if (It != GFunctions.end() && It->second.Generation == Generation)
			{
				Removed = std::move(It->second);
				GFunctions.erase(It);
			}
		}
		Generation = 0;
	}

	auto RegisterBuildFunction(
		FBuildFunctionIdentity Identity, std::shared_ptr<IBuildFunction> Function,
		FModuleOwnedCallbackGate OwnerGate, std::string* OutError)
		-> FBuildFunctionRegistration
	{
		if (!Identity.IsValid() || !IsCanonicalIdentityPart(Identity.Name))
			return SetError(OutError, "Build function identity is invalid."), FBuildFunctionRegistration{};
		if (!Function)
			return SetError(OutError, "Build function is invalid."), FBuildFunctionRegistration{};
		const FBuildFunctionConfig Config = Function->GetConfig();
		if (Config.CacheRoot.empty() || Config.ExpectedValueName.empty()
			|| Config.MaximumValueBytes == 0)
			return SetError(OutError, "Build function cache configuration is invalid."), FBuildFunctionRegistration{};
		auto Resource = OwnerGate.IsValid() ? OwnerGate.RetainResource() : FModuleOwnedResourceLease{};
		if (OwnerGate.IsValid() && !Resource)
			return SetError(OutError, "Build function module owner is retiring."), FBuildFunctionRegistration{};
		std::lock_guard Lock(GFunctionMutex);
		const std::string Key = FunctionKey(Identity);
		if (GFunctions.contains(Key))
			return SetError(OutError, "Build function identity is already registered."), FBuildFunctionRegistration{};
		const uint64 Generation = GNextFunctionGeneration++;
		GFunctions.emplace(Key, FRegisteredBuildFunction{
			std::move(Function), OwnerGate, std::move(Resource), Generation});
		FBuildFunctionRegistration Result;
		Result.Identity = std::move(Identity);
		Result.Generation = Generation;
		if (OutError) OutError->clear();
		return Result;
	}

	auto FBuildSession::Build(const FBuildDefinition& Definition,
		const FBuildPolicy& Policy, const FBuildCancellationToken* Cancellation) const
		-> FBuildOutput
	{
		if (!Definition.GetFunction().IsValid() || !Definition.GetKey().IsValid()
			|| Definition.GetExpectedValueName().empty())
			return Fail(EBuildFailurePhase::Request, "Build definition is incomplete.");
		if (Policy.bAllowLocalBuild && !Definition.HasLocalInputs())
			return Fail(EBuildFailurePhase::Request, "Local build requires definition inputs.");
		if (Cancellation && Cancellation->IsCanceled())
			return {.Status = EBuildStatus::Canceled, .FailurePhase = EBuildFailurePhase::Request,
				.Diagnostic = "Build request was canceled."};

		std::shared_ptr<IBuildFunction> Function;
		FModuleOwnedCallbackGate Gate;
		FModuleOwnedResourceLease Resource;
		{
			std::lock_guard Lock(GFunctionMutex);
			const auto It = GFunctions.find(FunctionKey(Definition.GetFunction()));
			if (It == GFunctions.end())
				return Fail(EBuildFailurePhase::FunctionLookup, "Build function is not registered.");
			Function = It->second.Function;
			Gate = It->second.Gate;
			if (Gate.IsValid()) Resource = Gate.RetainResource();
		}
		if (Gate.IsValid() && !Resource) return Fail(EBuildFailurePhase::FunctionLookup,
			"Build function module owner is retiring.");
		const FBuildFunctionConfig Config = Function->GetConfig();
		if (Config.ExpectedValueName != Definition.GetExpectedValueName())
			return Fail(EBuildFailurePhase::Request, "Build value contract does not match function configuration.");
		Asset::FDerivedDataObjectStore Store(Config.CacheRoot, Config.MaximumValueBytes);
		FBuildCacheClient Cache(Store);
		FBuildOutput Result;
		auto FailResult = [&](EBuildFailurePhase Phase, std::string Message) -> FBuildOutput {
			Result.Status = EBuildStatus::Failed;
			Result.FailurePhase = Phase;
			Result.Diagnostic = std::move(Message);
			return Result;
		};
		if (Policy.bQueryCache)
		{
			Result.bCacheQueried = true;
			const auto Query = MeasureNanoseconds(Result.PhaseDurations.CacheQueryNanoseconds,
				[&] { return Cache.Query(Definition.GetKey().ToString(),
					Config.ExpectedValueName, {.bQueryCache = true}); });
			if (Query.Status == EBuildCacheQueryStatus::Hit)
			{
				std::string Error;
				bool bValid = false;
				try
				{
					if (Gate.IsValid())
					{
						auto Invocation = Gate.TryEnter();
						if (!Invocation) return FailResult(EBuildFailurePhase::FunctionLookup,
							"Build function module owner is retiring.");
						bValid = MeasureNanoseconds(
							Result.PhaseDurations.CachedValueValidationNanoseconds,
							[&] { return Function->Validate(Definition, Query.Value, Error); });
					}
					else bValid = MeasureNanoseconds(
						Result.PhaseDurations.CachedValueValidationNanoseconds,
						[&] { return Function->Validate(Definition, Query.Value, Error); });
				}
				catch (const std::exception& Exception) { Error = Exception.what(); }
				catch (...) { Error = "Build function validation threw an unknown exception."; }
				if (bValid)
				{
					Result.Status = EBuildStatus::CacheHit;
					Result.Origin = EBuildValueOrigin::Cache;
					if (Policy.bReturnData) Result.Value = Query.Value;
					return Result;
				}
				Result.FailurePhase = EBuildFailurePhase::CachedValueValidation;
				Result.Diagnostic = std::move(Error);
			}
			else if (Query.Status == EBuildCacheQueryStatus::StorageError)
			{
				Result.FailurePhase = EBuildFailurePhase::CacheQuery;
				Result.Diagnostic = Query.Diagnostic;
			}
			else if (Query.Status == EBuildCacheQueryStatus::Missing)
				Result.Diagnostic = Query.Diagnostic;
		}
		if (!Policy.bAllowLocalBuild)
		{
			Result.Status = EBuildStatus::CacheMiss;
			return Result;
		}
		if (Cancellation && Cancellation->IsCanceled())
		{
			Result.Status = EBuildStatus::Canceled;
			Result.FailurePhase = EBuildFailurePhase::LocalBuild;
			Result.Diagnostic = "Build request was canceled.";
			return Result;
		}
		FBuildValue BuiltValue;
		std::string Error;
		{
			try
			{
				Result.bLocalBuildExecuted = true;
				bool bBuilt = false;
				if (Gate.IsValid())
				{
					auto Invocation = Gate.TryEnter();
					if (!Invocation) return FailResult(EBuildFailurePhase::FunctionLookup,
						"Build function module owner is retiring.");
					bBuilt = MeasureNanoseconds(Result.PhaseDurations.LocalBuildNanoseconds,
						[&] { return Function->Build(
							FBuildContext(Definition, Cancellation), BuiltValue, Error); });
				}
				else bBuilt = MeasureNanoseconds(Result.PhaseDurations.LocalBuildNanoseconds,
					[&] { return Function->Build(
						FBuildContext(Definition, Cancellation), BuiltValue, Error); });
				if (!bBuilt)
				{
					if (Cancellation && Cancellation->IsCanceled())
					{
						Result.Status = EBuildStatus::Canceled;
						Result.FailurePhase = EBuildFailurePhase::LocalBuild;
						Result.Diagnostic = std::move(Error);
						return Result;
					}
					return FailResult(EBuildFailurePhase::LocalBuild, std::move(Error));
				}
			}
			catch (const std::exception& Exception)
				{ return FailResult(EBuildFailurePhase::LocalBuild, Exception.what()); }
			catch (...) { return FailResult(EBuildFailurePhase::LocalBuild, "Build function threw an unknown exception."); }
			bool bValidated = false;
			if (Gate.IsValid())
			{
				auto Invocation = Gate.TryEnter();
				if (!Invocation) return FailResult(EBuildFailurePhase::FunctionLookup,
					"Build function module owner is retiring.");
				bValidated = MeasureNanoseconds(
					Result.PhaseDurations.BuiltValueValidationNanoseconds,
					[&] { return Function->Validate(Definition, BuiltValue, Error); });
			}
			else bValidated = MeasureNanoseconds(
				Result.PhaseDurations.BuiltValueValidationNanoseconds,
				[&] { return Function->Validate(Definition, BuiltValue, Error); });
			if (BuiltValue.GetName() != Config.ExpectedValueName
				|| BuiltValue.GetSize() > Config.MaximumValueBytes || !bValidated)
				return FailResult(EBuildFailurePhase::BuiltValueValidation,
					Error.empty() ? "Built value violates the function output contract." : std::move(Error));
		}
		Result.Status = EBuildStatus::Built;
		Result.Origin = EBuildValueOrigin::Local;
		Result.FailurePhase = EBuildFailurePhase::None;
		Result.Diagnostic.clear();
		Result.bLocalBuildExecuted = true;
		if (Cancellation && Cancellation->IsCanceled())
		{
			Result.Status = EBuildStatus::Canceled;
			Result.FailurePhase = EBuildFailurePhase::CacheStore;
			Result.Diagnostic = "Build request was canceled before cache store.";
			return Result;
		}
		if (Policy.bStoreBuildResult)
		{
			std::string StoreError;
			const bool bStored = MeasureNanoseconds(Result.PhaseDurations.CacheStoreNanoseconds,
				[&] { return Cache.Store(Definition.GetKey().ToString(), BuiltValue,
					{.bStoreBuildResult = true, .bRequireStoreSuccess = Policy.bRequireStoreSuccess},
					&StoreError); });
			Result.StoreDiagnostic = StoreError;
			if (!bStored)
			{
				Result.Status = EBuildStatus::Failed;
				Result.FailurePhase = EBuildFailurePhase::CacheStore;
				Result.Diagnostic = std::move(StoreError);
				return Result;
			}
			if (StoreError.empty() && Config.CleanupBudgetBytes)
			{
				const auto Cleanup = Store.CleanupToBudget(
					Config.CleanupBudgetBytes, Config.CleanupDeleteLimit);
				Result.StoreDiagnostic = Cleanup.Message;
			}
		}
		if (Policy.bReturnData) Result.Value = std::move(BuiltValue);
		return Result;
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

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "DerivedDataCache/DerivedDataCache.h"

namespace Durin::DerivedData
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

		struct FRegisteredBuildFunction
		{
			std::shared_ptr<IBuildFunction> Function;
			FBuildFunctionConfig Config;
			FCacheBucket CacheBucket;
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

	auto FBuildValue::FromOwned(std::string InName, std::vector<std::byte> InBytes)
		-> FBuildValue
	{
		FBuildValue Result;
		Result.Name = std::move(InName);
		Result.ContentIdentity = FXxHash128::HashBuffer(InBytes);
		Result.Bytes = std::make_shared<const std::vector<std::byte>>(std::move(InBytes));
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
		FBuildKey InKey, std::span<const std::byte> CanonicalKeyInput)
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
		const FCacheBucket CacheBucket = FCacheBucket::FromString(Config.CacheBucket);
		const bool bHasCleanupBudget = Config.CleanupBudgetBytes != 0;
		const bool bHasCleanupDeleteLimit = Config.CleanupDeleteLimit != 0;
		if (!CacheBucket.IsValid() || !IsCanonicalIdentityPart(Config.ExpectedValueName)
			|| Config.MaximumValueBytes == 0
			|| bHasCleanupBudget != bHasCleanupDeleteLimit)
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
			std::move(Function), Config, CacheBucket, OwnerGate, std::move(Resource), Generation});
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
		FBuildFunctionConfig Config;
		FCacheBucket CacheBucket;
		FModuleOwnedCallbackGate Gate;
		FModuleOwnedResourceLease Resource;
		{
			std::lock_guard Lock(GFunctionMutex);
			const auto It = GFunctions.find(FunctionKey(Definition.GetFunction()));
			if (It == GFunctions.end())
				return Fail(EBuildFailurePhase::FunctionLookup, "Build function is not registered.");
			Function = It->second.Function;
			Config = It->second.Config;
			CacheBucket = It->second.CacheBucket;
			Gate = It->second.Gate;
			if (Gate.IsValid()) Resource = Gate.RetainResource();
		}
		if (Gate.IsValid() && !Resource) return Fail(EBuildFailurePhase::FunctionLookup,
			"Build function module owner is retiring.");
		if (Config.ExpectedValueName != Definition.GetExpectedValueName())
			return Fail(EBuildFailurePhase::Request, "Build value contract does not match function configuration.");
		const FCacheKey CacheKey = FCacheKey::FromString(Definition.GetKey().ToString());
		if (!CacheKey.IsValid())
			return Fail(EBuildFailurePhase::Request, "Build key is invalid.");
		FDerivedDataCache& Cache = GetDerivedDataCache();
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
				[&] { return Cache.Get({CacheBucket, CacheKey, Config.MaximumValueBytes}); });
			if (Query.Status == ECacheGetStatus::Hit)
			{
				const std::span<const std::byte> CachedBytes = Query.Value.GetBytes();
				const FBuildValue CachedValue = FBuildValue::FromOwned(Config.ExpectedValueName,
					std::vector<std::byte>(CachedBytes.begin(), CachedBytes.end()));
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
							[&] { return Function->Validate(Definition, CachedValue, Error); });
					}
					else bValid = MeasureNanoseconds(
						Result.PhaseDurations.CachedValueValidationNanoseconds,
						[&] { return Function->Validate(Definition, CachedValue, Error); });
				}
				catch (const std::exception& Exception) { Error = Exception.what(); }
				catch (...) { Error = "Build function validation threw an unknown exception."; }
				if (bValid)
				{
					Result.Status = EBuildStatus::CacheHit;
					Result.Origin = EBuildValueOrigin::Cache;
					if (Policy.bReturnData) Result.Value = CachedValue;
					return Result;
				}
				Result.FailurePhase = EBuildFailurePhase::CachedValueValidation;
				Result.Diagnostic = std::move(Error);
			}
			else if (Query.Status != ECacheGetStatus::Miss)
			{
				Result.FailurePhase = EBuildFailurePhase::CacheQuery;
				Result.Diagnostic = Query.Diagnostic;
			}
			else
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
			const FCachePutResult Put = MeasureNanoseconds(Result.PhaseDurations.CacheStoreNanoseconds,
				[&] { return Cache.Put({CacheBucket, CacheKey, BuiltValue.GetBytes(),
					Config.MaximumValueBytes}); });
			Result.StoreDiagnostic = Put.Diagnostic;
			if (!Put && Policy.bRequireStoreSuccess)
			{
				Result.Status = EBuildStatus::Failed;
				Result.FailurePhase = EBuildFailurePhase::CacheStore;
				Result.Diagnostic = Put.Diagnostic;
				return Result;
			}
			if (Put && Config.CleanupBudgetBytes)
			{
				const auto Trim = Cache.Trim({CacheBucket,
					Config.CleanupBudgetBytes, Config.CleanupDeleteLimit});
				Result.StoreDiagnostic = Trim.Diagnostic;
			}
		}
		if (Policy.bReturnData) Result.Value = std::move(BuiltValue);
		return Result;
	}

}

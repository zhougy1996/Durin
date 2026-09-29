#include "DerivedDataBuildExecutionPrivate.h"
#include "DerivedDataBuildValidation.h"
#include "Logging/LogMacros.h"

namespace Durin::DerivedData
{
	namespace
	{
		auto NormalizeStatus(std::optional<FCacheKey>& Key, EBuildStatus Status) -> EBuildStatus
		{
			if (Key && !Key->IsValid()) Key.reset();
			return Key ? Status : EBuildStatus::None;
		}
	}
	auto FBuildCompleteParams::Error(std::optional<FCacheKey> Key, EBuildStatus Status) -> FBuildCompleteParams
	{ FBuildCompleteParams Result; Result.Status = EStatus::Error; Result.CacheKey = std::move(Key); Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status); return Result; }
	auto FBuildCompleteParams::Canceled(std::optional<FCacheKey> Key, EBuildStatus Status) -> FBuildCompleteParams
	{ FBuildCompleteParams Result; Result.Status = EStatus::Canceled; Result.CacheKey = std::move(Key); Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status); return Result; }
}

namespace Durin::DerivedData::Private
{
	auto FBuildCompletionAccess::Ok(FBuildOutput Output, FCacheKey Key, EBuildStatus Status) -> FBuildCompleteParams
	{
		if (!Output.IsValid() || !Key.IsValid()) return FBuildCompleteParams::Error(std::nullopt, Status);
		FBuildCompleteParams Result; Result.Status = Output.HasError() ? EStatus::Error : EStatus::Ok; Result.CacheKey = std::move(Key); Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status); Result.Output = std::move(Output); return Result;
	}
	auto FBuildCompletionAccess::Canceled(FBuildCompleteParams Completion) -> FBuildCompleteParams
	{ Completion.Status = EStatus::Canceled; Completion.Output.reset(); return Completion; }

	auto ExecuteBuild(const std::variant<FBuildDefinition, FBuildAction>& Request,
		const FBuildRegistrySnapshot& Registry, const std::shared_ptr<const IBuildInputResolver>& SessionResolver,
		const FBuildInputs& RequestInputs, const FBuildRequestOptions& Options,
		const FBuildServiceOptions& Service) -> FBuildCompleteParams
	{
		const FBuildPolicy& Policy = Options.Policy;
		const FBuildCancellation& Cancel = Options.Cancellation;
		EBuildStatus Status = EBuildStatus::None;
		std::optional<FCacheKey> CacheKey;
		const std::string_view FunctionName = std::holds_alternative<FBuildDefinition>(Request)
			? std::get<FBuildDefinition>(Request).GetFunctionName()
			: std::get<FBuildAction>(Request).GetFunction().Name;
		auto Canceled = [&] { return FBuildCompleteParams::Canceled(CacheKey, Status); };
		auto CacheFailure = [&](std::string_view Operation, const FCacheError& Error) noexcept
		{
			DURIN_WARN_CATEGORY("DerivedData", "{} cache {} [{}]: {}", FunctionName,
				Operation, CacheKey ? CacheKey->ToString() : std::string("unkeyed"), Error.Diagnostic);
		};
		auto Failed = [&](std::string_view Message)
		{
			DURIN_ERROR_CATEGORY("DerivedData", "{} build failed [{}]: {}", FunctionName,
				CacheKey ? CacheKey->ToString() : std::string("unkeyed"), Message);
			return FBuildCompleteParams::Error(CacheKey, Status);
		};
		try
		{
			if (Cancel.IsCancelled()) return Canceled();
			const auto Entry = Registry.Find(FunctionName); if (!Entry) return Failed("Build function is not registered.");
			std::optional<FBuildAction> ActionStorage;
			if (const auto* Existing = std::get_if<FBuildAction>(&Request)) { if (Existing->GetFunction() != Entry->Descriptor) return Failed("Build action does not match the registered function."); ActionStorage = *Existing; }
			else
			{
				const auto& Definition = std::get<FBuildDefinition>(Request); std::vector<FBuildInputReference> Identities;
				if (RequestInputs.IsValid()) Identities.assign(RequestInputs.GetIdentities().begin(), RequestInputs.GetIdentities().end());
				else if (!Definition.GetSources().empty()) { if (!SessionResolver) return Failed("No build input resolver is available."); auto Described = SessionResolver->Describe(Definition.GetSources(), Cancel); if (!Described) return Failed(Described.error().Description); Identities = std::move(*Described); }
				FBuildActionBuilder ActionBuilder(Definition, Entry->Descriptor); for (auto& Identity : Identities) ActionBuilder.AddInput(std::move(Identity)); auto Created = std::move(ActionBuilder).Build(); if (!Created) return Failed("Resolved metadata does not match the build definition."); ActionStorage = std::move(*Created);
			}
			const FBuildAction& Action = *ActionStorage; CacheKey = Action.GetKey();
			if (Policy.QueryCache && !Policy.ForceBuild)
			{
				FCacheGetResult Stored = std::optional<FCacheRecord>{};
				try { Stored = (Service.Cache ? *Service.Cache : GetCache()).Get({Action.GetKey(), Policy.OutputLimits, Policy.MaximumEncodedBytes}); } catch (...) { Stored = std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache query threw."}); }
				if (Cancel.IsCancelled()) return Canceled(); if (!Stored) CacheFailure("read", Stored.error());
				else if (*Stored) { auto Output = (**Stored).ToOutput(Action.GetKey(), Policy.OutputLimits); if (!Output) CacheFailure("read", Output.error()); else { Status |= EBuildStatus::CacheQueryHit; return FBuildCompletionAccess::Ok(std::move(*Output), Action.GetKey(), Status); } }
			}
			if (!Policy.BuildLocal) return Failed("No usable cache value is available and local build is disabled.");
			std::expected<std::vector<FBuildInput>, FBuildInputError> Inputs = std::vector<FBuildInput>{};
			if (!Action.GetInputs().empty()) { if (RequestInputs.IsValid()) Inputs = FBuildExecutionAccess::Resolve(RequestInputs, Cancel); else if (SessionResolver) Inputs = SessionResolver->Resolve(Action.GetInputs(), Cancel); else Inputs = std::unexpected(FBuildInputError{"Build input payloads are unavailable."}); }
			if (Cancel.IsCancelled()) return Canceled(); if (!Inputs) return Failed(Inputs.error().Description); if (Inputs->size() != Action.GetInputs().size()) return Failed("Resolved input count mismatch.");
			std::ranges::sort(*Inputs, {}, [](const FBuildInput& I) -> const std::string& { return I.Identity.Name; }); uint64 Total = 0;
			for (size_t Index = 0; Index < Inputs->size(); ++Index)
			{
				auto& Input = (*Inputs)[Index]; if (Input.Identity != Action.GetInputs()[Index]) return Failed("Resolved input identity mismatch."); std::ranges::sort(Input.Values, {}, &FBuildInputValue::Name); std::string_view Previous;
				if (Input.Values.size() > std::min<uint32>(Policy.InputLimits.MaximumValues, FBuildInput::MaximumValues)
					|| Input.Metadata.GetSize() > Policy.InputLimits.MaximumMetadataBytes
					|| Input.Metadata.GetSize() > Policy.InputLimits.MaximumTotalBytes - Total) return Failed("Resolved input table or metadata budget exceeded.");
				Total += Input.Metadata.GetSize();
				for (const auto& Value : Input.Values) { if (!Private::IsBuildValueIdentifier(Value.Name) || (!Previous.empty() && Previous >= Value.Name)) return Failed("Resolved input has invalid or duplicate value names."); Previous = Value.Name; if (Value.Data.GetSize() > Policy.InputLimits.MaximumTotalBytes - Total) return Failed("Resolved input byte budget exceeded."); Total += Value.Data.GetSize(); }
			}
			Status |= EBuildStatus::BuildLocal;
			FBuildOutputBuilder Builder = FBuildExecutionAccess::MakeOutputBuilder(Entry->Descriptor.OutputType, Entry->Descriptor.OutputSchema, Policy.OutputLimits); auto Context = FBuildExecutionAccess::MakeContext(Action, *Inputs, Builder, Cancel, Policy.MaximumWorkingSetBytes); Entry->Function->Build(Context); if (Cancel.IsCancelled()) return Canceled(); Inputs->clear(); auto Output = std::move(Builder).Build(); if (!Output) return Failed(Output.error());
			if (Policy.StoreOnBuild && !Output->HasLogs())
			{
				try
				{
					auto Record = FCacheRecord::FromOutput(
						Action.GetKey(), *Output, Policy.PersistenceLimits);
					if (!Record) CacheFailure("write", Record.error());
					else
					{
						auto Put = (Service.Cache ? *Service.Cache : GetCache()).Put(
							{std::move(*Record), Policy.MaximumEncodedBytes});
						if (!Put) CacheFailure("write", Put.error());
					}
				}
				catch (...)
				{
					CacheFailure("write",
						{ECacheError::StorageFailure, "Cache persistence threw."});
				}
			}
			if (Cancel.IsCancelled()) return Canceled(); return FBuildCompletionAccess::Ok(std::move(*Output), Action.GetKey(), Status);
		}
		catch (const std::exception& E) { return Failed(E.what()); }
		catch (...) { return Failed("Build execution threw an exception."); }
	}
}

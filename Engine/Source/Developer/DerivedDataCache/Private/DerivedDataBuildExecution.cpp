#include "DerivedDataBuildExecutionPrivate.h"
#include "DerivedDataBuildValidation.h"
#include "Logging/LogMacros.h"

namespace Durin::DerivedData
{
	namespace
	{
		auto NormalizeExecutionFlags(std::optional<FCacheKey>& Key, EBuildExecutionFlags ExecutionFlags) -> EBuildExecutionFlags
		{
			if (Key && !Key->IsValid()) Key.reset();
			return Key ? ExecutionFlags : EBuildExecutionFlags::None;
		}
	}
	auto FBuildCompleteParams::Error(std::optional<FCacheKey> Key, EBuildExecutionFlags ExecutionFlags, std::string Diagnostic, std::optional<FBuildOutput> DiagnosticOutput) -> FBuildCompleteParams
	{ FBuildCompleteParams Result; Result.Status = EStatus::Error; Result.CacheKey = std::move(Key); Result.ExecutionFlags = NormalizeExecutionFlags(Result.CacheKey, ExecutionFlags); Diagnostic.resize(std::min<size_t>(Diagnostic.size(), 4096)); Result.Diagnostic = std::move(Diagnostic); Result.Output = std::move(DiagnosticOutput); return Result; }
	auto FBuildCompleteParams::Canceled(std::optional<FCacheKey> Key, EBuildExecutionFlags ExecutionFlags) -> FBuildCompleteParams
	{ FBuildCompleteParams Result; Result.Status = EStatus::Canceled; Result.CacheKey = std::move(Key); Result.ExecutionFlags = NormalizeExecutionFlags(Result.CacheKey, ExecutionFlags); return Result; }
}

namespace Durin::DerivedData::Private
{
	auto FBuildCompletionAccess::Ok(FBuildOutput Output, FCacheKey Key, EBuildExecutionFlags ExecutionFlags) -> FBuildCompleteParams
	{
		if (!Output.IsValid() || !Key.IsValid()) return FBuildCompleteParams::Error(std::nullopt, ExecutionFlags);
		FBuildCompleteParams Result; Result.Status = Output.HasError() ? EStatus::Error : EStatus::Ok; Result.CacheKey = std::move(Key); Result.ExecutionFlags = NormalizeExecutionFlags(Result.CacheKey, ExecutionFlags); Result.Output = std::move(Output); return Result;
	}
	auto FBuildCompletionAccess::Canceled(FBuildCompleteParams Completion) -> FBuildCompleteParams
	{ Completion.Status = EStatus::Canceled; Completion.Output.reset(); Completion.Diagnostic.clear(); return Completion; }

	auto PrepareBuildAction(const std::variant<FBuildDefinition, FBuildAction>& Request,
		const FBuildRegistrySnapshot& Registry, const std::shared_ptr<const IBuildInputResolver>& Resolver,
		const FBuildInputs& Inputs, const FBuildCancellation& Cancel)
		-> std::expected<FBuildAction, FBuildCompleteParams>
	{
		const auto Name = std::holds_alternative<FBuildDefinition>(Request) ? std::get<FBuildDefinition>(Request).GetFunctionName() : std::get<FBuildAction>(Request).GetFunction().Name;
		auto Failure = [&](std::string_view Message) -> std::expected<FBuildAction, FBuildCompleteParams>
		{
			DURIN_ERROR_CATEGORY("DerivedData", "{} action preparation failed: {}", Name, Message);
			return std::unexpected(FBuildCompleteParams::Error(std::nullopt, EBuildExecutionFlags::None, std::string(Message)));
		};
		auto Canceled = [] { return std::unexpected(FBuildCompleteParams::Canceled(std::nullopt, EBuildExecutionFlags::None)); };
		try
		{
			if (Cancel.IsCancelled()) return Canceled();
			const auto Entry = Registry.Find(Name); if (!Entry) return Failure("Build function is not registered.");
			if (const auto* Existing = std::get_if<FBuildAction>(&Request))
			{
				if (Existing->GetFunction() != Entry->Descriptor) return Failure("Build action does not match the registered function.");
				return *Existing;
			}
			const auto& Definition = std::get<FBuildDefinition>(Request); std::vector<FBuildInputReference> Identities;
			if (Inputs.IsValid()) Identities.assign(Inputs.GetIdentities().begin(), Inputs.GetIdentities().end());
			else if (!Definition.GetSources().empty())
			{
				if (!Resolver) return Failure("No build input resolver is available.");
				auto Described = Resolver->Describe(Definition.GetSources(), Cancel);
				if (Cancel.IsCancelled()) return Canceled(); if (!Described) return Failure(Described.error().Description);
				Identities = std::move(*Described);
			}
			FBuildActionBuilder Builder(Definition, Entry->Descriptor);
			for (auto& Identity : Identities) Builder.AddInput(std::move(Identity));
			auto Created = std::move(Builder).Build(); if (!Created) return Failure("Resolved metadata does not match the build definition.");
			return std::move(*Created);
		}
		catch (const std::exception& E) { return Failure(E.what()); }
		catch (...) { return Failure("Action preparation threw."); }
	}

	auto ExecuteBuildLocal(const FBuildAction& Action,
		const FBuildRegistrySnapshot& Registry, const std::shared_ptr<const IBuildInputResolver>& SessionResolver,
		const FBuildInputs& RequestInputs, const FBuildRequestOptions& Options, const FBuildCancellation& Cancel) -> FBuildCompleteParams
	{
		const auto& Policy = Options.Policy;
		const auto CacheKey = Action.GetKey();
		EBuildExecutionFlags ExecutionFlags = EBuildExecutionFlags::None;
		auto Canceled = [&] { return FBuildCompleteParams::Canceled(CacheKey, ExecutionFlags); };
		auto Failed = [&](std::string_view Message, std::optional<FBuildOutput> DiagnosticOutput = {})
		{
			DURIN_ERROR_CATEGORY("DerivedData", "{} build failed [{}]: {}", Action.GetFunction().Name,
				CacheKey.ToString(), Message);
			return FBuildCompleteParams::Error(CacheKey, ExecutionFlags, std::string(Message), std::move(DiagnosticOutput));
		};
		try
		{
			if (Cancel.IsCancelled()) return Canceled();
			const auto Entry = Registry.Find(Action.GetFunction().Name);
			if (!Entry) return Failed("Build function is not registered.");
			if (!Policy.Has(EBuildPolicy::BuildLocal)) return Failed("No usable cache value is available and local build is disabled.");
			std::expected<std::vector<FBuildInput>, FBuildInputError> Inputs = std::vector<FBuildInput>{};
			if (!Action.GetInputs().empty()) { if (RequestInputs.IsValid()) Inputs = FBuildExecutionAccess::Resolve(RequestInputs, Cancel); else if (SessionResolver) Inputs = SessionResolver->Resolve(Action.GetInputs(), Cancel); else Inputs = std::unexpected(FBuildInputError{"Build input payloads are unavailable."}); }
			if (Cancel.IsCancelled()) return Canceled(); if (!Inputs) return Failed(Inputs.error().Description); if (Inputs->size() != Action.GetInputs().size()) return Failed("Resolved input count mismatch.");
			std::ranges::sort(*Inputs, {}, [](const FBuildInput& I) -> const std::string& { return I.Identity.Name; }); uint64 Total = 0;
			for (size_t Index = 0; Index < Inputs->size(); ++Index)
			{
				auto& Input = (*Inputs)[Index]; if (Input.Identity != Action.GetInputs()[Index]) return Failed("Resolved input identity mismatch."); std::ranges::sort(Input.Values, {}, &FBuildInputValue::Name); std::string_view Previous;
				if (Input.Values.size() > std::min<uint32>(Options.Limits.Input.MaximumValues, FBuildInput::MaximumValues)
					|| Input.Metadata.GetSize() > Options.Limits.Input.MaximumMetadataBytes
					|| Input.Metadata.GetSize() > Options.Limits.Input.MaximumTotalBytes - Total) return Failed("Resolved input table or metadata budget exceeded.");
				Total += Input.Metadata.GetSize();
				for (const auto& Value : Input.Values) { if (!Private::IsBuildValueIdentifier(Value.Name) || (!Previous.empty() && Previous >= Value.Name)) return Failed("Resolved input has invalid or duplicate value names."); Previous = Value.Name; if (Value.Data.GetSize() > Options.Limits.Input.MaximumTotalBytes - Total) return Failed("Resolved input byte budget exceeded."); Total += Value.Data.GetSize(); }
			}
			ExecutionFlags |= EBuildExecutionFlags::BuildLocal;
			FBuildOutputBuilder Builder = FBuildExecutionAccess::MakeOutputBuilder(Entry->Descriptor.OutputType, Entry->Descriptor.OutputSchema, Options.Limits.Output); auto Context = FBuildExecutionAccess::MakeContext(Action, *Inputs, Builder, Cancel); Entry->Function->Build(Context);
			if (Cancel.IsCancelled()) return Canceled();
			if (const auto& Failure = FBuildExecutionAccess::GetExecutionFailure(Context); Failure) {
				auto Partial = std::move(Builder).Build();
				if (!Partial || Partial->GetObservations().empty()) return Failed(*Failure);
				// Only transient observations survive execution failure, never partial products.
				auto Diagnostics = FBuildExecutionAccess::MakeOutputBuilder(Entry->Descriptor.OutputType, Entry->Descriptor.OutputSchema, Options.Limits.Output);
				for (const auto& Observation : Partial->GetObservations()) Diagnostics.AddObservation(Observation.Id, Observation.Object);
				auto Output = std::move(Diagnostics).Build();
				return Output ? Failed(*Failure, std::move(*Output)) : Failed(*Failure);
			}
			Inputs->clear(); auto Output = std::move(Builder).Build(); if (!Output) return Failed(Output.error());
			if (Cancel.IsCancelled()) return Canceled();
			return FBuildCompletionAccess::Ok(std::move(*Output), CacheKey, ExecutionFlags);
		}
		catch (const std::exception& E) { return Failed(E.what()); }
		catch (...) { return Failed("Build execution threw an exception."); }
	}
}

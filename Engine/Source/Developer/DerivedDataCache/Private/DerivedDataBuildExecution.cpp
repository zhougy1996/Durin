#include "DerivedDataBuildExecutionPrivate.h"
#include "DerivedDataBuildValidation.h"
#include "Logging/LogMacros.h"

namespace Durin::DerivedData
{
	auto FBuildCompleteParams::Error(FCacheKey Key, EBuildExecutionFlags Flags, std::string Diagnostic, FBuildOutput Observations) -> FBuildCompleteParams
	{
		FBuildOutputBuilder Builder(Observations.IsValid() ? std::string(Observations.GetSchema()) : "Build.Diagnostics", Observations.IsValid() ? Observations.GetSchemaVersion() : 1);
		if (Observations.IsValid()) for (const auto& Value : Observations.GetObservations()) Builder.AddObservation(Value.Id, Value.Object);
		Diagnostic.resize(std::min<size_t>(Diagnostic.size(), 4096));
		std::replace(Diagnostic.begin(), Diagnostic.end(), '\0', '?');
		Builder.AddLog("DerivedData", EBuildLogSeverity::Error, Diagnostic.empty() ? "Build failed." : std::move(Diagnostic));
		return {Key, std::move(Builder).Build().value(), Key.IsValid() ? Flags : EBuildExecutionFlags::None, EStatus::Error};
	}
	auto FBuildCompleteParams::Canceled(FCacheKey Key, EBuildExecutionFlags Flags, FBuildOutput Output) -> FBuildCompleteParams
	{
		if (!Output.IsValid() || !Output.GetValues().empty() || !Output.GetMetadata().empty()) {
			FBuildOutputBuilder Builder(Output.IsValid() ? std::string(Output.GetSchema()) : "Build.Diagnostics", Output.IsValid() ? Output.GetSchemaVersion() : 1);
			if (Output.IsValid()) {
				for (const auto& Observation : Output.GetObservations()) Builder.AddObservation(Observation.Id, Observation.Object);
				for (const auto& Message : Output.GetMessages()) Builder.AddMessage(Message.Severity, Message.Text);
				for (const auto& Log : Output.GetLogs()) Builder.AddLog(Log.Category, Log.Severity, Log.Text);
			}
			Output = std::move(Builder).Build().value();
		}
		return {Key, std::move(Output), Key.IsValid() ? Flags : EBuildExecutionFlags::None, EStatus::Canceled};
	}
}
namespace Durin::DerivedData::Private
{
	auto PrepareBuildAction(const FBuildDefinition& Definition,
		const FBuildRegistrySnapshot& Registry, std::span<const FBuildInputReference> Identities) -> std::expected<FBuildAction, std::string>
	{
		const auto Entry = Registry.Find(Definition.GetFunctionName()); if (!Entry) return std::unexpected("Build function is not registered.");
		FBuildActionBuilder Builder(Definition, Entry->Descriptor);
		for (const auto& Identity : Identities) Builder.AddInput(Identity);
		auto Result = std::move(Builder).Build();
		if (!Result) return std::unexpected("Resolved metadata does not match the build definition.");
		return std::move(*Result);
	}

	auto ExecuteBuildLocal(const FBuildAction& Action, const FBuildRegistrySnapshot& Registry,
		std::vector<FBuildInput> ResolvedInputs, const FBuildPolicy& Policy, const FBuildRequestLimits& Limits, IRequestOwner& Owner) -> FBuildCompleteParams
	{
		const auto CacheKey = Action.GetKey();
		EBuildExecutionFlags ExecutionFlags = EBuildExecutionFlags::None;
		auto Canceled = [&] { return FBuildCompleteParams::Canceled(CacheKey, ExecutionFlags); };
		auto Failed = [&](std::string_view Message, FBuildOutput DiagnosticOutput = {})
		{
			DURIN_ERROR_CATEGORY("DerivedData", "{} build failed [{}]: {}", Action.GetFunction().Name,
				CacheKey.ToString(), Message);
			return FBuildCompleteParams::Error(CacheKey, ExecutionFlags, std::string(Message), std::move(DiagnosticOutput));
		};
		try
		{
			if (Owner.IsCanceled()) return Canceled();
			const auto Entry = Registry.Find(Action.GetFunction().Name);
			if (!Entry) return Failed("Build function is not registered.");
			if (!Policy.Has(EBuildPolicy::BuildLocal)) return Failed("No usable cache value is available and local build is disabled.");
			auto* Inputs = &ResolvedInputs;
			if (Inputs->size() != Action.GetInputs().size()) return Failed("Resolved input count mismatch.");
			std::ranges::sort(*Inputs, {}, [](const FBuildInput& I) -> const std::string& { return I.Identity.Name; }); uint64 Total = 0;
			for (size_t Index = 0; Index < Inputs->size(); ++Index)
			{
				auto& Input = (*Inputs)[Index]; if (Input.Identity != Action.GetInputs()[Index]) return Failed("Resolved input identity mismatch."); std::ranges::sort(Input.Values, {}, &FBuildInputValue::Name); std::string_view Previous;
				if (Input.Values.size() > std::min<uint32>(Limits.Input.MaximumValues, FBuildInput::MaximumValues)
					|| Input.Metadata.GetSize() > Limits.Input.MaximumMetadataBytes
					|| Input.Metadata.GetSize() > Limits.Input.MaximumTotalBytes - Total) return Failed("Resolved input table or metadata budget exceeded.");
				Total += Input.Metadata.GetSize();
				for (const auto& Value : Input.Values) { if (!Private::IsBuildValueIdentifier(Value.Name) || (!Previous.empty() && Previous >= Value.Name)) return Failed("Resolved input has invalid or duplicate value names."); Previous = Value.Name; if (Value.Data.GetSize() > Limits.Input.MaximumTotalBytes - Total) return Failed("Resolved input byte budget exceeded."); Total += Value.Data.GetSize(); }
			}
			ExecutionFlags |= EBuildExecutionFlags::BuildLocal;
			FBuildOutputBuilder Builder = FBuildExecutionAccess::MakeOutputBuilder(Entry->Descriptor.OutputType, Entry->Descriptor.OutputSchema, Limits.Output); auto Context = FBuildExecutionAccess::MakeContext(Action, *Inputs, Builder, Owner); Entry->Function->Build(Context);
			if (Owner.IsCanceled()) {
				auto Output = std::move(Builder).Build();
				return FBuildCompleteParams::Canceled(CacheKey, ExecutionFlags, Output ? std::move(*Output) : FBuildOutput{});
			}
			if (const auto& Failure = FBuildExecutionAccess::GetExecutionFailure(Context); Failure) {
				auto Partial = std::move(Builder).Build();
				if (!Partial || Partial->GetObservations().empty()) return Failed(*Failure);
				// Only transient observations survive execution failure, never partial products.
				auto Diagnostics = FBuildExecutionAccess::MakeOutputBuilder(Entry->Descriptor.OutputType, Entry->Descriptor.OutputSchema, Limits.Output);
				for (const auto& Observation : Partial->GetObservations()) Diagnostics.AddObservation(Observation.Id, Observation.Object);
				auto Output = std::move(Diagnostics).Build();
				return Output ? Failed(*Failure, std::move(*Output)) : Failed(*Failure);
			}
			Inputs->clear(); auto Output = std::move(Builder).Build(); if (!Output) return Failed(Output.error());
			if (Owner.IsCanceled()) return FBuildCompleteParams::Canceled(CacheKey, ExecutionFlags, std::move(*Output));
			const auto Status = Output->HasError() ? EStatus::Error : EStatus::Ok;
			return {CacheKey, std::move(*Output), ExecutionFlags, Status};
		}
		catch (const std::exception& E) { return Failed(E.what()); }
		catch (...) { return Failed("Build execution threw an exception."); }
	}
}

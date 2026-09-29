#include "DerivedDataBuildExecutionPrivate.h"
#include "DerivedDataBuildValidation.h"
#include "Logging/LogMacros.h"
#include <chrono>

namespace Durin::DerivedData
{
	namespace
	{
		auto OperationName(EBuildOperation Operation) -> std::string_view
		{
			switch (Operation)
			{
			case EBuildOperation::CacheQuery: return "read";
			case EBuildOperation::Decode: return "decode";
			case EBuildOperation::Record: return "record";
			case EBuildOperation::Encode: return "encode";
			case EBuildOperation::Compress: return "compress";
			case EBuildOperation::CacheStore: return "write";
			default: return "execution";
			}
		}
		auto NormalizeStatus(std::optional<FCacheKey>& Key, EBuildStatus Status) -> EBuildStatus
		{
			if (Key && !Key->IsValid()) Key.reset();
			if (!Key) return EBuildStatus(uint32(Status) & ~(uint32(EBuildStatus::CacheKey) | uint32(EBuildStatus::CacheQuery) | uint32(EBuildStatus::CacheQueryHit) | uint32(EBuildStatus::BuildLocal) | uint32(EBuildStatus::CacheStore)));
			Status |= EBuildStatus::CacheKey; if (HasBuildStatus(Status, EBuildStatus::CacheQueryHit)) Status |= EBuildStatus::CacheQuery; if (HasBuildStatus(Status, EBuildStatus::CacheStore)) Status |= EBuildStatus::BuildLocal; return Status;
		}
	}
	auto FBuildCompleteParams::Error(std::optional<FCacheKey> Key, EBuildStatus Status, FBuildExecutionReport Report) -> FBuildCompleteParams
	{ FBuildCompleteParams Result; Result.Status = EStatus::Error; Result.CacheKey = std::move(Key); Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status); Result.Report = std::move(Report); return Result; }
	auto FBuildCompleteParams::Canceled(std::optional<FCacheKey> Key, EBuildStatus Status, FBuildExecutionReport Report) -> FBuildCompleteParams
	{ FBuildCompleteParams Result; Result.Status = EStatus::Canceled; Result.CacheKey = std::move(Key); Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status); Result.Report = std::move(Report); return Result; }
}

namespace Durin::DerivedData::Private
{
	auto FBuildCompletionAccess::Ok(FBuildOutput Output, FCacheKey Key, EBuildStatus Status, FBuildExecutionReport Report) -> FBuildCompleteParams
	{
		if (!Output.IsValid() || !Key.IsValid()) return FBuildCompleteParams::Error(std::nullopt, Status, std::move(Report));
		FBuildCompleteParams Result; Result.Status = Output.HasError() ? EStatus::Error : EStatus::Ok; Result.CacheKey = std::move(Key); Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status); Result.Output = std::move(Output); Result.Report = std::move(Report); return Result;
	}
	auto FBuildCompletionAccess::Canceled(FBuildCompleteParams Completion) -> FBuildCompleteParams
	{ Completion.Status = EStatus::Canceled; Completion.Output.reset(); return Completion; }

	auto ExecuteBuild(const std::variant<FBuildDefinition, FBuildAction>& Request,
		const FBuildRegistrySnapshot& Registry, const std::shared_ptr<const IBuildInputResolver>& SessionResolver,
		const FBuildInputs& RequestInputs, const FBuildPolicy& Policy, const FBuildCancellation& Cancel,
		const FBuildServiceOptions& Service) -> FBuildCompleteParams
	{
		EBuildOperation Operation = EBuildOperation::Admission; EBuildStatus Status = EBuildStatus::None; FBuildExecutionReport Report; std::optional<FCacheKey> CacheKey; const FBuildAction* DiagnosticAction = nullptr;
		auto Canceled = [&] { return FBuildCompleteParams::Canceled(CacheKey, Status, std::move(Report)); };
		auto Diagnostic = [&](FCacheError Error) noexcept { try { if (Error.Diagnostic.size() > 4096) Error.Diagnostic.resize(4096); FBuildDiagnostic Value{Operation, std::move(Error)}; Report.Diagnostics.push_back(Value); if (DiagnosticAction && Service.Diagnostics) Service.Diagnostics(*DiagnosticAction, Value); else if (DiagnosticAction) DURIN_WARN_CATEGORY("DerivedData", "{} cache {} [{}]: {}", DiagnosticAction->GetFunction().Name, OperationName(Operation), DiagnosticAction->GetKey().ToString(), Value.Error.Diagnostic); } catch (...) {} };
		auto Failed = [&](std::string Message) { Diagnostic({ECacheError::StorageFailure, std::move(Message)}); return FBuildCompleteParams::Error(CacheKey, Status, std::move(Report)); };
		try
		{
			if (Cancel.IsCancelled()) return Canceled();
			const std::string_view FunctionName = std::holds_alternative<FBuildDefinition>(Request) ? std::get<FBuildDefinition>(Request).GetFunctionName() : std::get<FBuildAction>(Request).GetFunction().Name;
			const auto Entry = Registry.Find(FunctionName); if (!Entry) return Failed("Build function is not registered.");
			std::optional<FBuildAction> ActionStorage;
			if (const auto* Existing = std::get_if<FBuildAction>(&Request)) { Operation = EBuildOperation::Action; if (Existing->GetFunction() != Entry->Descriptor) return Failed("Build action does not match the registered function."); ActionStorage = *Existing; }
			else
			{
				const auto& Definition = std::get<FBuildDefinition>(Request); std::vector<FBuildInputReference> Identities;
				if (RequestInputs.IsValid()) Identities.assign(RequestInputs.GetIdentities().begin(), RequestInputs.GetIdentities().end());
				else if (!Definition.GetSources().empty()) { Operation = EBuildOperation::Describe; if (!SessionResolver) return Failed("No build input resolver is available."); auto Described = SessionResolver->Describe(Definition.GetSources(), Cancel); if (!Described) return Failed(std::move(Described.error().Description)); Identities = std::move(*Described); }
				Operation = EBuildOperation::Action; FBuildActionBuilder ActionBuilder(Definition, Entry->Descriptor); for (auto& Identity : Identities) ActionBuilder.AddInput(std::move(Identity)); auto Created = std::move(ActionBuilder).Build(); if (!Created) return Failed("Resolved metadata does not match the build definition."); ActionStorage = std::move(*Created);
			}
			const FBuildAction& Action = *ActionStorage; DiagnosticAction = &Action; CacheKey = Action.GetKey(); Status |= EBuildStatus::CacheKey;
			if (Policy.QueryCache && !Policy.ForceBuild)
			{
				Operation = EBuildOperation::CacheQuery; Status |= EBuildStatus::CacheQuery; FCacheGetResult Stored = std::optional<FSharedByteBuffer>{};
				try { Stored = Service.Cache.Get ? Service.Cache.Get({Action.GetKey(), Policy.MaximumEncodedBytes}) : GetCache().Get({Action.GetKey(), Policy.MaximumEncodedBytes}); } catch (...) { Stored = std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache query threw."}); }
				if (Cancel.IsCancelled()) return Canceled(); if (!Stored) Diagnostic(std::move(Stored.error()));
				else if (*Stored) { Operation = EBuildOperation::Decode; auto Record = FCacheRecord::Decode(Action.GetKey(), std::move(**Stored), Policy.OutputLimits, Policy.MaximumEncodedBytes); if (!Record) Diagnostic(std::move(Record.error())); else { auto Output = Record->ToOutput(Action.GetKey(), Policy.OutputLimits); if (!Output) Diagnostic(std::move(Output.error())); else { Status |= EBuildStatus::CacheQueryHit; return FBuildCompletionAccess::Ok(std::move(*Output), Action.GetKey(), Status, std::move(Report)); } } }
			}
			if (!Policy.BuildLocal) return Failed("No usable cache value is available and local build is disabled.");
			Operation = EBuildOperation::Resolve; std::expected<std::vector<FBuildInput>, FBuildInputError> Inputs = std::vector<FBuildInput>{};
			if (!Action.GetInputs().empty()) { if (RequestInputs.IsValid()) Inputs = FBuildExecutionAccess::Resolve(RequestInputs, Cancel); else if (SessionResolver) Inputs = SessionResolver->Resolve(Action.GetInputs(), Cancel); else Inputs = std::unexpected(FBuildInputError{"Build input payloads are unavailable."}); }
			if (Cancel.IsCancelled()) return Canceled(); if (!Inputs) return Failed(std::move(Inputs.error().Description)); if (Inputs->size() != Action.GetInputs().size()) return Failed("Resolved input count mismatch.");
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
			Operation = EBuildOperation::Build; Status |= EBuildStatus::BuildLocal; auto Metric = [&](std::string_view Name, uint64 Value) noexcept { try { if (Report.Metrics.size() < FBuildExecutionReport::MaximumMetrics) Report.Metrics.push_back({std::string(Name.substr(0, FBuildExecutionReport::MaximumMetricNameBytes)), Value}); if (Service.Metrics) Service.Metrics(Name, Value); } catch (...) {} };
			FBuildOutputBuilder Builder = FBuildExecutionAccess::MakeOutputBuilder(Entry->Descriptor.OutputType, Entry->Descriptor.OutputSchema, Policy.OutputLimits); auto Context = FBuildExecutionAccess::MakeContext(Action, *Inputs, Builder, Cancel, Metric, Policy.MaximumWorkingSetBytes); Entry->Function->Build(Context); if (Cancel.IsCancelled()) return Canceled(); Inputs->clear(); auto Output = std::move(Builder).Build(); if (!Output) return Failed(std::move(Output.error()));
			if (Policy.StoreOnBuild)
			{
				Status |= EBuildStatus::CacheStore; const auto Start = std::chrono::steady_clock::now();
				try { Operation = EBuildOperation::Record; auto Record = Service.Cache.MakeRecord ? Service.Cache.MakeRecord(Action.GetKey(), *Output, Policy.PersistenceLimits) : FCacheRecord::FromOutput(Action.GetKey(), *Output, Policy.PersistenceLimits); if (!Record) Diagnostic(std::move(Record.error())); else { Operation = EBuildOperation::Encode; auto Encoded = Service.Cache.Encode ? Service.Cache.Encode(*Record, Policy.MaximumEncodedBytes) : Record->Encode(Policy.MaximumEncodedBytes); if (!Encoded) Diagnostic(std::move(Encoded.error())); else { if (Service.CompressRecords) { Operation = EBuildOperation::Compress; Encoded = Service.Cache.Compress ? Service.Cache.Compress(*Encoded, Policy.MaximumEncodedBytes) : FCacheRecord::CompressEncoded(*Encoded, Policy.MaximumEncodedBytes); } if (!Encoded) Diagnostic(std::move(Encoded.error())); else { Operation = EBuildOperation::CacheStore; auto Put = Service.Cache.Put ? Service.Cache.Put({Action.GetKey(), *Encoded, Policy.MaximumEncodedBytes}) : GetCache().Put({Action.GetKey(), *Encoded, Policy.MaximumEncodedBytes}); if (!Put) Diagnostic(std::move(Put.error())); } } } } catch (...) { Diagnostic({ECacheError::StorageFailure, "Cache persistence threw."}); }
				Report.PersistenceNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count();
			}
			if (Cancel.IsCancelled()) return Canceled(); return FBuildCompletionAccess::Ok(std::move(*Output), Action.GetKey(), Status, std::move(Report));
		}
		catch (const std::exception& E) { return Failed(E.what()); }
		catch (...) { return Failed("Build execution threw an exception."); }
	}
}

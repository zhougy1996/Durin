#include "DerivedDataBuildExecutionPrivate.h"
#include "DerivedDataBuildValidation.h"
#include "Logging/LogMacros.h"
#include <chrono>

namespace Durin::DerivedData
{
	namespace
	{
		auto NormalizeStatus(std::optional<FCacheKey>& Key, EBuildStatus Status) -> EBuildStatus
		{
			if (Key && !Key->IsValid()) Key.reset();
			if (!Key)
				return EBuildStatus(uint32(Status) & ~(uint32(EBuildStatus::CacheKey)
					| uint32(EBuildStatus::CacheQuery) | uint32(EBuildStatus::CacheQueryHit)
					| uint32(EBuildStatus::BuildLocal) | uint32(EBuildStatus::CacheStore)));
			Status |= EBuildStatus::CacheKey;
			if (HasBuildStatus(Status, EBuildStatus::CacheQueryHit)) Status |= EBuildStatus::CacheQuery;
			if (HasBuildStatus(Status, EBuildStatus::CacheStore)) Status |= EBuildStatus::BuildLocal;
			return Status;
		}
	}
	auto FBuildCompleteParams::Error(FBuildFailure Failure, std::optional<FCacheKey> Key,
		EBuildStatus Status, FBuildExecutionReport Report) -> FBuildCompleteParams
	{
		Failure.BoundDescription();
		FBuildCompleteParams Result;
		Result.Status = EStatus::Error;
		Result.CacheKey = std::move(Key);
		Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status);
		Result.Failure = std::move(Failure);
		Result.Report = std::move(Report);
		return Result;
	}
	auto FBuildCompleteParams::Canceled(std::optional<FCacheKey> Key,
		EBuildStatus Status, FBuildExecutionReport Report) -> FBuildCompleteParams
	{
		FBuildCompleteParams Result;
		Result.Status = EStatus::Canceled;
		Result.CacheKey = std::move(Key);
		Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status);
		Result.Report = std::move(Report);
		return Result;
	}
}

namespace Durin::DerivedData::Private
{
	auto FBuildCompletionAccess::Ok(FBuildOutput Output,
		std::shared_ptr<const FBuildValidationReceipt> ValidationReceipt, FCacheKey Key,
		EBuildStatus Status, FBuildExecutionReport Report) -> FBuildCompleteParams
	{
		if (!Output.IsValid() || !Key.IsValid())
			return FBuildCompleteParams::Error({.Reason = EBuildFailureReason::InternalFailure,
				.Operation = EBuildOperation::Validate,
				.Description = "Validated build completion is invalid."}, std::nullopt, Status, std::move(Report));
		FBuildCompleteParams Result;
		Result.Status = EStatus::Ok;
		Result.CacheKey = std::move(Key);
		Result.BuildStatus = NormalizeStatus(Result.CacheKey, Status);
		Result.Output = std::move(Output);
		Result.ValidationReceipt = std::move(ValidationReceipt);
		Result.Report = std::move(Report);
		return Result;
	}
	auto FBuildCompletionAccess::Canceled(FBuildCompleteParams Completion) -> FBuildCompleteParams
	{
		Completion.Status = EStatus::Canceled;
		Completion.Output.reset();
		Completion.Failure.reset();
		Completion.ValidationReceipt.reset();
		return Completion;
	}

	namespace
	{
		auto OperationName(EBuildOperation Operation) -> std::string_view
		{
			switch (Operation)
			{
			case EBuildOperation::CacheQuery: return "read";
			case EBuildOperation::Decode: return "decode";
			case EBuildOperation::Validate: return "validate";
			case EBuildOperation::Record: return "record";
			case EBuildOperation::Encode: return "encode";
			case EBuildOperation::Compress: return "compress";
			case EBuildOperation::CacheStore: return "write";
			default: return "execution";
			}
		}
	}

	auto ExecuteBuild(const std::variant<FBuildDefinition, FBuildAction>& Request,
		const FBuildRegistrySnapshot& Registry,
		const std::shared_ptr<const IBuildInputResolver>& SessionResolver,
		const FBuildInputs& RequestInputs, const FBuildPolicy& Policy,
		const FBuildCancellation& Cancel, const FBuildServiceOptions& Service)
		-> FBuildCompleteParams
	{
		EBuildOperation Operation = EBuildOperation::Admission;
		EBuildStatus Status = EBuildStatus::None;
		FBuildExecutionReport Report;
		std::optional<FCacheKey> CacheKey;
		auto Canceled = [&] { return FBuildCompleteParams::Canceled(CacheKey, Status, std::move(Report)); };
		auto Failed = [&](FBuildFailure Failure, EBuildOperation At) {
			Failure.Operation = At;
			return FBuildCompleteParams::Error(std::move(Failure), CacheKey, Status, std::move(Report));
		};
		try
		{
			if (Cancel.IsCancelled()) return Canceled();
			const std::string_view FunctionName = std::holds_alternative<FBuildDefinition>(Request)
				? std::get<FBuildDefinition>(Request).GetFunctionName()
				: std::get<FBuildAction>(Request).GetFunction().Name;
			const auto Entry = Registry.Find(FunctionName);
			if (!Entry) return Failed({.Reason = EBuildFailureReason::InternalFailure,
				.Description = "Build function is not registered."}, Operation);

			std::optional<FBuildAction> ActionStorage;
			if (const auto* Existing = std::get_if<FBuildAction>(&Request))
			{
				Operation = EBuildOperation::Action;
				if (Existing->GetFunction() != Entry->Descriptor)
					return Failed({.Description = "Build action does not match the registered function."}, Operation);
				ActionStorage = *Existing;
			}
			else
			{
				const auto& Definition = std::get<FBuildDefinition>(Request);
				std::vector<FBuildInputReference> Identities;
				if (RequestInputs.IsValid())
					Identities.assign(RequestInputs.GetIdentities().begin(), RequestInputs.GetIdentities().end());
				else if (!Definition.GetSources().empty())
				{
					Operation = EBuildOperation::Describe;
					if (!SessionResolver) return Failed({.Reason = EBuildFailureReason::InputUnavailable,
						.Description = "No build input resolver is available."}, Operation);
					auto Described = SessionResolver->Describe(Definition.GetSources(), Cancel);
					if (Cancel.IsCancelled()) return Canceled();
					if (!Described) return Failed(std::move(Described.error()), Operation);
					Identities = std::move(*Described);
				}
				Operation = EBuildOperation::Action;
				auto Created = FBuildAction::TryCreate(Definition, Entry->Descriptor, std::move(Identities));
				if (!Created) return Failed({.Description = "Resolved metadata does not match the build definition."}, Operation);
				ActionStorage = std::move(*Created);
			}
			const FBuildAction& Action = *ActionStorage;
			CacheKey = Action.GetKey();
			Status |= EBuildStatus::CacheKey;

			auto Diagnostic = [&](FCacheError Error) noexcept {
				try
				{
					if (Error.Diagnostic.size() > FBuildFailure::MaximumDescriptionBytes)
						Error.Diagnostic.resize(FBuildFailure::MaximumDescriptionBytes);
					FBuildDiagnostic Value{Operation, std::move(Error)};
					Report.Diagnostics.push_back(Value);
					if (Service.Diagnostics) Service.Diagnostics(Action, Value);
					else DURIN_WARN_CATEGORY("DerivedData", "{} cache {} [{}]: {}", FunctionName,
						OperationName(Operation), Action.GetKey().ToString(), Value.Error.Diagnostic);
				}
				catch (...) {}
			};
			auto Validate = [&](const FBuildOutput& Output) -> FBuildValidationResult {
				if (Output.GetSchema() != Entry->Descriptor.OutputType || Output.GetSchemaVersion() != Entry->Descriptor.OutputSchema)
					return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InvalidOutput,
						.Description = "Output schema does not match the registered function."});
				if (auto Valid = Output.CheckLimits(Policy.OutputLimits); !Valid)
					return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InvalidOutput,
						.Description = std::move(Valid.error())});
				return Entry->Function->Validate(Action, Output, Cancel);
			};

			if (Policy.QueryCache && !Policy.ForceBuild)
			{
				Operation = EBuildOperation::CacheQuery;
				Status |= EBuildStatus::CacheQuery;
				const FCacheGetRequest Query{Action.GetKey(), Policy.MaximumEncodedBytes};
				FCacheGetResult Stored = std::optional<FSharedByteBuffer>{};
				try { Stored = Service.Cache.Get ? Service.Cache.Get(Query) : GetCache().Get(Query); }
				catch (const std::bad_alloc&) { Stored = std::unexpected(FCacheError{ECacheError::StorageFailure, "Allocation"}); }
				catch (const std::exception& Exception) { Stored = std::unexpected(FCacheError{ECacheError::StorageFailure, Exception.what()}); }
				catch (...) { Stored = std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache query threw an unknown exception."}); }
				if (Cancel.IsCancelled()) return Canceled();
				if (!Stored) Diagnostic(std::move(Stored.error()));
				else if (*Stored)
				{
					Operation = EBuildOperation::Decode;
					auto Record = FCacheRecord::Decode(Action.GetKey(), std::move(**Stored),
						Policy.OutputLimits, Policy.MaximumEncodedBytes);
					if (Cancel.IsCancelled()) return Canceled();
					if (!Record) Diagnostic(std::move(Record.error()));
					else
					{
						auto Output = Record->ToOutput(Action.GetKey(), Policy.OutputLimits);
						if (!Output) Diagnostic(std::move(Output.error()));
						else
						{
							Operation = EBuildOperation::Validate;
							auto Valid = Validate(*Output);
							if (Cancel.IsCancelled()) return Canceled();
							if (Valid)
							{
								Status |= EBuildStatus::CacheQueryHit;
								return FBuildCompletionAccess::Ok(std::move(*Output), std::move(*Valid),
									Action.GetKey(), Status, std::move(Report));
							}
							Diagnostic({ECacheError::Corrupt, std::move(Valid.error().Description)});
						}
					}
				}
			}
			if (!Policy.BuildLocal)
				return Failed({.Reason = EBuildFailureReason::InputUnavailable,
					.Description = "No usable cache value is available and local build is disabled."}, Operation);

			Operation = EBuildOperation::Resolve;
			std::expected<std::vector<FBuildInput>, FBuildFailure> Inputs = std::vector<FBuildInput>{};
			if (!Action.GetInputs().empty())
			{
				if (RequestInputs.IsValid()) Inputs = FBuildExecutionAccess::Resolve(RequestInputs, Cancel);
				else if (SessionResolver) Inputs = SessionResolver->Resolve(Action.GetInputs(), Cancel);
				else Inputs = std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InputUnavailable,
					.Description = "Build input payloads are unavailable."});
			}
			if (Cancel.IsCancelled()) return Canceled();
			if (!Inputs) return Failed(std::move(Inputs.error()), Operation);
			if (Inputs->size() != Action.GetInputs().size())
				return Failed({.Description = "Resolved input count mismatch."}, Operation);
			std::ranges::sort(*Inputs, {}, [](const FBuildInput& Input) { return std::string_view(Input.Identity.Name); });
			uint64 Total = 0, Metadata = 0, Values = 0;
			for (size_t Index = 0; Index < Inputs->size(); ++Index)
			{
				if (Cancel.IsCancelled()) return Canceled();
				auto& Input = (*Inputs)[Index];
				if (Input.Identity != Action.GetInputs()[Index]) return Failed({.Description = "Resolved input identity mismatch."}, Operation);
				const uint64 MetadataLimit = std::min<uint64>(Policy.InputLimits.MaximumMetadataBytes, 4ull * 1024 * 1024);
				const uint64 ValueLimit = std::min<uint64>(Policy.InputLimits.MaximumValues, FBuildInput::MaximumValues);
				if (Input.Metadata.GetSize() > MetadataLimit - Metadata || Input.Values.size() > ValueLimit - Values)
					return Failed({.Reason = EBuildFailureReason::ResourceExhaustion, .Description = "Resolved input table budget exceeded."}, Operation);
				Metadata += Input.Metadata.GetSize(); Values += Input.Values.size();
				if (Input.Metadata.GetSize() > Policy.InputLimits.MaximumTotalBytes - Total)
					return Failed({.Reason = EBuildFailureReason::ResourceExhaustion, .Description = "Resolved input byte budget exceeded."}, Operation);
				Total += Input.Metadata.GetSize();
				std::ranges::sort(Input.Values, {}, &FBuildValue::Id);
				std::string_view Previous;
				for (const auto& Value : Input.Values)
				{
					if (!Private::IsBuildValueIdentifier(Value.Id) || (!Previous.empty() && Previous >= Value.Id))
						return Failed({.Description = "Resolved input has invalid or duplicate value IDs."}, Operation);
					Previous = Value.Id;
					if (Value.Data.GetSize() > Policy.InputLimits.MaximumTotalBytes - Total)
						return Failed({.Reason = EBuildFailureReason::ResourceExhaustion, .Description = "Resolved input byte budget exceeded."}, Operation);
					Total += Value.Data.GetSize();
				}
			}

			Operation = EBuildOperation::Build;
			Status |= EBuildStatus::BuildLocal;
			auto Metric = [&](std::string_view Name, uint64 Value) noexcept {
				try
				{
					if (Report.Metrics.size() < FBuildExecutionReport::MaximumMetrics)
						Report.Metrics.push_back({std::string(Name.substr(0,
							FBuildExecutionReport::MaximumMetricNameBytes)), Value});
				}
				catch (...) {}
				try { if (Service.Metrics) Service.Metrics(Name, Value); } catch (...) {}
			};
			FBuildContext Context(Action, *Inputs, Cancel, Metric, Policy.MaximumWorkingSetBytes);
			auto Output = Entry->Function->Build(Context);
			if (Cancel.IsCancelled()) return Canceled();
			if (!Output) return Failed(std::move(Output.error()), Operation);
			Inputs->clear();
			Operation = EBuildOperation::Validate;
			auto Valid = Validate(*Output);
			if (Cancel.IsCancelled()) return Canceled();
			if (!Valid) return Failed(std::move(Valid.error()), Operation);

			if (Policy.StoreOnBuild)
			{
				Status |= EBuildStatus::CacheStore;
				const auto Start = std::chrono::steady_clock::now();
				try
				{
					auto Persist = [&]() -> std::expected<void, FCacheError> {
						Operation = EBuildOperation::Record;
						auto Record = Service.Cache.MakeRecord ? Service.Cache.MakeRecord(Action.GetKey(), *Output, Policy.PersistenceLimits)
							: FCacheRecord::FromOutput(Action.GetKey(), *Output, Policy.PersistenceLimits);
						if (!Record) return std::unexpected(std::move(Record.error()));
						if (Cancel.IsCancelled()) return {};
						Operation = EBuildOperation::Encode;
						auto Encoded = Service.Cache.Encode ? Service.Cache.Encode(*Record, Policy.MaximumEncodedBytes)
							: Record->Encode(Policy.MaximumEncodedBytes);
						if (!Encoded) return std::unexpected(std::move(Encoded.error()));
						if (Cancel.IsCancelled()) return {};
						if (Service.CompressRecords)
						{
							Operation = EBuildOperation::Compress;
							Encoded = Service.Cache.Compress ? Service.Cache.Compress(*Encoded, Policy.MaximumEncodedBytes)
								: FCacheRecord::CompressEncoded(*Encoded, Policy.MaximumEncodedBytes);
							if (!Encoded) return std::unexpected(std::move(Encoded.error()));
							if (Cancel.IsCancelled()) return {};
						}
						Operation = EBuildOperation::CacheStore;
						const FCachePutRequest Put{Action.GetKey(), *Encoded, Policy.MaximumEncodedBytes};
						return Service.Cache.Put ? Service.Cache.Put(Put) : GetCache().Put(Put);
					};
					if (auto Saved = Persist(); !Saved && !Cancel.IsCancelled()) Diagnostic(std::move(Saved.error()));
				}
				catch (const std::bad_alloc&)
				{
					if (!Cancel.IsCancelled()) Diagnostic({ECacheError::StorageFailure, "Allocation"});
				}
				catch (const std::exception& Exception)
				{
					if (!Cancel.IsCancelled()) Diagnostic({ECacheError::StorageFailure, Exception.what()});
				}
				catch (...)
				{
					if (!Cancel.IsCancelled()) Diagnostic({ECacheError::StorageFailure, "Cache persistence threw an unknown exception."});
				}
				Report.PersistenceNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - Start).count();
			}
			if (Cancel.IsCancelled()) return Canceled();
			return FBuildCompletionAccess::Ok(std::move(*Output), std::move(*Valid),
				Action.GetKey(), Status, std::move(Report));
		}
		catch (const std::bad_alloc&)
		{
			return Failed({.Reason = EBuildFailureReason::ResourceExhaustion,
				.Description = "Allocation"}, Operation);
		}
		catch (...)
		{
			return Failed({.Reason = EBuildFailureReason::InternalFailure,
				.Description = "Build execution threw an exception."}, Operation);
		}
	}
}

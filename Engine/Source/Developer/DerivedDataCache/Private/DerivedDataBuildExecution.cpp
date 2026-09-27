#include "DerivedDataBuildExecution.h"
#include "DerivedDataBuildValidation.h"
#include "Logging/LogMacros.h"

namespace Durin::DerivedData
{
	namespace
	{
		auto CacheOperationName(EBuildSessionPhase Phase) -> std::string_view
		{
			switch (Phase)
			{
			case EBuildSessionPhase::Lookup: return "read";
			case EBuildSessionPhase::Decode: return "decode";
			case EBuildSessionPhase::Validate: return "validate";
			case EBuildSessionPhase::Record: return "record";
			case EBuildSessionPhase::Encode: return "encode";
			case EBuildSessionPhase::Compress: return "compress";
			case EBuildSessionPhase::Store: return "write";
			default: return "execution";
			}
		}
		auto Failure(FBuildError Error, EBuildSessionPhase Phase) -> FBuildResult
		{
			Error.Phase = Phase;
			Error.BoundDescription();
			return std::unexpected(std::move(Error));
		}
	}

	auto ExecuteBuildRequest(const FBuildDefinition& Definition, const FBuildRegistrySnapshot& Registry,
		const IBuildInputResolver& Resolver, const FBuildRequestPolicy& Policy, const FBuildCancellation& Cancel,
		const FBuildCacheOperations& Cache, const FBuildRunObserver& Observer) -> FBuildResult
	{
		EBuildSessionPhase Phase = EBuildSessionPhase::Admission;
		auto Enter = [&](EBuildSessionPhase Next) {
			Phase = Next;
			if (Observer.OnPhase) Observer.OnPhase(Phase);
			return !Cancel.IsCancelled();
		};
		auto Cancelled = [&] -> FBuildResult {
			return std::unexpected(FBuildError{.Phase = Phase, .Category = EBuildErrorCategory::Cancelled});
		};
		try
		{
			if (!Enter(Phase)) return Cancelled();
			const auto Entry = Registry.Find(Definition.GetFunctionName());
			if (!Entry) return Failure({.Description = "Build function is not registered."}, Phase);
			if (!Enter(EBuildSessionPhase::Describe)) return Cancelled();
			auto Identities = Resolver.Describe(Definition.GetSources(), Cancel);
			if (Cancel.IsCancelled()) return Cancelled();
			if (!Identities) return Failure(std::move(Identities.error()), Phase);
			if (!Enter(EBuildSessionPhase::Action)) return Cancelled();
			auto Action = FBuildAction::TryCreate(Definition, Entry->Descriptor, std::move(*Identities));
			if (!Action) return Failure({.Description = "Resolved metadata does not match the build definition."}, Phase);
			if (Observer.OnAction) Observer.OnAction(*Action);
			auto Issue = [&](FCacheError Error) noexcept {
				try
				{
					if (Error.Diagnostic.size() > FBuildError::MaximumDescriptionBytes) Error.Diagnostic.resize(FBuildError::MaximumDescriptionBytes);
					if (Observer.OnCacheIssue) Observer.OnCacheIssue(*Action, Phase, Error);
					else DURIN_WARN_CATEGORY("DerivedData", "{} cache {} [{}]: {}", Action->GetFunction().Name,
						CacheOperationName(Phase), Action->GetKey().ToString(), Error.Diagnostic);
				}
				catch (...) {} // Diagnostics cannot invalidate a usable output.
			};
			auto ValidateOutput = [&](const FBuildOutput& Output) -> std::expected<void, FBuildError> {
				if (Output.GetSchema() != Entry->Descriptor.OutputType || Output.GetSchemaVersion() != Entry->Descriptor.OutputSchema)
					return std::unexpected(FBuildError{.Category = EBuildErrorCategory::InvalidOutput, .Description = "Output schema does not match the registered function."});
				if (auto Valid = Output.CheckLimits(Policy.OutputLimits); !Valid)
					return std::unexpected(FBuildError{.Category = EBuildErrorCategory::InvalidOutput, .Description = std::move(Valid.error())});
				return Entry->Function->Validate(*Action, Output, Cancel);
			};
			if (Policy.ReadCache && !Policy.ForceRebuild)
			{
				if (!Enter(EBuildSessionPhase::Lookup)) return Cancelled();
				const FCacheGetRequest Request{Action->GetKey(), Policy.MaximumEncodedBytes};
				auto Stored = Cache.Get ? Cache.Get(Request) : GetCache().Get(Request);
				if (Cancel.IsCancelled()) return Cancelled();
				if (!Stored)
				{
					if (Stored.error().Code != ECacheError::Miss) Issue(std::move(Stored.error()));
				}
				else
				{
					if (!Enter(EBuildSessionPhase::Decode)) return Cancelled();
					auto Record = FCacheRecord::Decode(Action->GetKey(), std::move(*Stored), Policy.OutputLimits, Policy.MaximumEncodedBytes);
					if (Cancel.IsCancelled()) return Cancelled();
					if (!Record) Issue(std::move(Record.error()));
					else
					{
						auto Output = Record->ToOutput(Action->GetKey(), Policy.OutputLimits);
						if (Cancel.IsCancelled()) return Cancelled();
						if (!Output) Issue(std::move(Output.error()));
						else
						{
							if (!Enter(EBuildSessionPhase::Validate)) return Cancelled();
							auto Valid = ValidateOutput(*Output);
							if (Cancel.IsCancelled() || (!Valid && Valid.error().Category == EBuildErrorCategory::Cancelled)) return Cancelled();
							if (Valid)
							{
								if (Observer.OnCacheHit) Observer.OnCacheHit();
								if (Cancel.IsCancelled()) return Cancelled();
								return std::move(*Output);
							}
							Issue({ECacheError::Corrupt, std::move(Valid.error().Description)});
						}
					}
				}
			}
			if (!Enter(EBuildSessionPhase::Resolve)) return Cancelled();
			auto Inputs = Resolver.Resolve(Action->GetInputs(), Cancel);
			if (Cancel.IsCancelled()) return Cancelled();
			if (!Inputs) return Failure(std::move(Inputs.error()), Phase);
			if (Inputs->size() != Action->GetInputs().size()) return Failure({.Description = "Resolved input count mismatch."}, Phase);
			std::ranges::sort(*Inputs, {}, [](const FBuildInput& Input) { return std::string_view(Input.Identity.Name); });
			uint64 Total = 0, Metadata = 0, Values = 0;
			for (size_t Index = 0; Index < Inputs->size(); ++Index)
			{
				if (Cancel.IsCancelled()) return Cancelled();
				auto& Input = (*Inputs)[Index];
				if (Input.Identity != Action->GetInputs()[Index]) return Failure({.Description = "Resolved input identity mismatch."}, Phase);
				if (Input.Metadata.GetSize() > std::min<uint64>(Policy.InputLimits.MaximumMetadataBytes, 4ull * 1024 * 1024) - Metadata
					|| Input.Values.size() > std::min<uint64>(Policy.InputLimits.MaximumValues, FBuildInput::MaximumValues) - Values)
					return Failure({.Description = "Resolved input table budget exceeded."}, Phase);
				Metadata += Input.Metadata.GetSize(); Values += Input.Values.size();
				if (Input.Metadata.GetSize() > Policy.InputLimits.MaximumTotalBytes - Total)
					return Failure({.Description = "Resolved input byte budget exceeded."}, Phase);
				Total += Input.Metadata.GetSize();
				std::ranges::sort(Input.Values, {}, &FBuildValue::Id);
				std::string_view Previous;
				for (const auto& Value : Input.Values)
				{
					if (Cancel.IsCancelled()) return Cancelled();
					if (!Private::IsBuildValueIdentifier(Value.Id) || (!Previous.empty() && Previous >= Value.Id))
						return Failure({.Description = "Resolved input has invalid or duplicate value IDs."}, Phase);
					Previous = Value.Id;
					if (Value.Data.GetSize() > Policy.InputLimits.MaximumTotalBytes - Total)
						return Failure({.Description = "Resolved input byte budget exceeded."}, Phase);
					Total += Value.Data.GetSize();
				}
			}
			if (!Enter(EBuildSessionPhase::Build)) return Cancelled();
			FBuildContext Context(*Action, *Inputs, Cancel, Observer.OnMetric, Policy.MaximumWorkingSetBytes);
			auto Output = Entry->Function->Build(Context);
			if (Cancel.IsCancelled()) return Cancelled();
			if (!Output) return Failure(std::move(Output.error()), Phase);
			Inputs->clear(); // Output must own its blocks independently of resolved descriptors.
			if (!Enter(EBuildSessionPhase::Validate)) return Cancelled();
			auto Valid = ValidateOutput(*Output);
			if (Cancel.IsCancelled()) return Cancelled();
			if (!Valid) return Failure(std::move(Valid.error()), Phase);
			if (Policy.WriteCache)
			{
				// Every failure in optional persistence leaves the valid shared output intact.
				try
				{
					auto Persist = [&]() -> std::expected<void, FCacheError> {
						if (!Enter(EBuildSessionPhase::Record)) return {};
						auto Record = Cache.MakeRecord ? Cache.MakeRecord(Action->GetKey(), *Output, Policy.PersistenceLimits)
							: FCacheRecord::FromOutput(Action->GetKey(), *Output, Policy.PersistenceLimits);
						if (!Record) return std::unexpected(std::move(Record.error()));
						if (!Enter(EBuildSessionPhase::Encode)) return {};
						auto Encoded = Cache.Encode ? Cache.Encode(*Record, Policy.MaximumEncodedBytes) : Record->Encode(Policy.MaximumEncodedBytes);
						if (!Encoded) return std::unexpected(std::move(Encoded.error()));
						if (Policy.Compress)
						{
							if (!Enter(EBuildSessionPhase::Compress)) return {};
							Encoded = Cache.Compress ? Cache.Compress(*Encoded, Policy.MaximumEncodedBytes)
								: FCacheRecord::CompressEncoded(*Encoded, Policy.MaximumEncodedBytes);
							if (!Encoded) return std::unexpected(std::move(Encoded.error()));
						}
						if (!Enter(EBuildSessionPhase::Store)) return {};
						const FCachePutRequest Request{Action->GetKey(), *Encoded, Policy.MaximumEncodedBytes};
						return Cache.Put ? Cache.Put(Request) : GetCache().Put(Request);
					};
					if (auto Saved = Persist(); !Saved && !Cancel.IsCancelled()) Issue(std::move(Saved.error()));
				}
				catch (const std::bad_alloc&) { if (!Cancel.IsCancelled()) Issue({ECacheError::StorageFailure, "Allocation"}); }
			}
			if (Cancel.IsCancelled()) return Cancelled();
			return std::move(*Output);
		}
		catch (const std::bad_alloc&) { return Failure({.Category = EBuildErrorCategory::Unavailable, .Description = "Allocation"}, Phase); }
	}
}

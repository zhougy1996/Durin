#include "MaterialTranslation.h"
#if DURIN_WITH_EDITOR
#include "MaterialTranslationCodec.h"
#include "Asset/ResourceBuildSessions.h"
#include "Serialization/BinaryFormat.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin::MaterialBuildPrivate
{
	using namespace DerivedData;
	namespace
	{
		AssetPrivate::FResourceBuildSessions Sessions;
		std::atomic_uint64_t Producers = 0, Resolutions = 0, Hits = 0, ProducerMicroseconds = 0, GameThreadProducers = 0;
		auto InputReference(FXxHash128 Identity) -> FBuildInputReference { return {"Input", Identity, "Material.TranslationInput", 1, "Material.MIR", 1}; }
		auto Failure(FMaterialError Error) -> FMaterialTranslatedProgram
		{ FMaterialTranslatedProgram Product; Product.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Normalization, .Error = std::move(Error)}); return Product; }
		class FResolver final : public IBuildInputResolver
		{
		public:
			FSharedByteBuffer Bytes;
			FXxHash128 Identity;
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation&) const -> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override
			{
				if (Sources.size() != 1 || Sources.front().Name != "Input" || Sources.front().Source != "CapturedMIR") return std::unexpected(FBuildInputError{"Invalid material input reference."});
				return std::vector{InputReference(Identity)};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancellation) const -> std::expected<std::vector<FBuildInput>, FBuildInputError> override
			{
				++Resolutions;
				if (Cancellation.IsCancelled()) return std::vector<FBuildInput>{};
				if (Inputs.size() != 1 || Inputs.front() != InputReference(Identity) || FXxHash128::HashBuffer(Bytes.GetBytes()) != Identity)
					return std::unexpected(FBuildInputError{"Invalid captured material input."});
				return std::vector<FBuildInput>{{.Identity = Inputs.front(), .Values = {{"MIR", Bytes}}}};
			}
		};
		class FFunction final : public IBuildFunction
		{
		public:
			auto GetName() const -> std::string_view override { return "Material.Translate"; }
			auto GetVersion() const -> uint32 override { return 1; }
			auto Configure(FBuildConfigContext& Context) const -> void override
			{ Context.SetConstantsSchema(1); Context.SetOutput("Material.Translation", TranslationSchema); Context.SetCacheBucket(FCacheBucket::FromString("MaterialTranslation")); }
			auto Build(FBuildContext& Context) const -> void override
			{
				const auto* Version = Context.FindConstant<uint64>("Generator");
				const auto* Identity = Context.FindConstant<FXxHash128>("InputIdentity");
				const auto* Input = Context.FindInput("Input");
				if (!Version || *Version != GetMaterialBuildVersion() || !Identity || !Input || Input->Identity != InputReference(*Identity)
					|| Input->Values.size() != 1 || Input->Values.front().Name != "MIR" || FXxHash128::HashBuffer(Input->Values.front().Data.GetBytes()) != *Identity)
					return Context.FailExecution("Material translation input identity is invalid.");
				auto Decoded = DecodeTranslationInput(Input->Values.front().Data.GetBytes());
				if (!Decoded) return Context.FailExecution("Material translation input encoding is invalid.");
				if (Context.IsCancelled()) return;
				++Producers; if (GIsGameThreadIdInitialized && IsInGameThread()) ++GameThreadProducers;
				const auto Begin = std::chrono::steady_clock::now();
				auto Product = TranslatePreparedMaterialProgram(PrepareMaterialProgram(*Decoded));
				if (!Product) for (auto& Diagnostic : Product.Diagnostics)
					if (!Diagnostic.NodeId.IsValid() && (Diagnostic.LocationKind == EMaterialProgramDiagnosticLocationKind::Node || Diagnostic.LocationKind == EMaterialProgramDiagnosticLocationKind::Input))
					{
						const MIR::FSource* Source = nullptr;
						if (Product.IR.Nodes.empty())
						{
							const auto Found = std::ranges::find(Decoded->Sources, Diagnostic.LocationIndex, &MIR::FSource::ExpressionIndex);
							if (Found != Decoded->Sources.end()) Source = &*Found;
						}
						else
						{
							const auto Found = std::ranges::find(Product.Sources, Diagnostic.LocationIndex, [](const auto& Location) { return Location.Source.ExpressionIndex; });
							if (Found != Product.Sources.end()) Source = &Found->Source;
						}
						if (Source) Diagnostic.NodeId = Source->NodeId;
					}
				auto Encoded = EncodeTranslationProduct(Product, *Identity);
				if (!Encoded) return Context.FailExecution("Material translation output exceeds its schema or resource bound.");
				if (!DecodeTranslationProduct(Encoded->GetBytes(), *Identity, *Decoded)) return Context.FailExecution("Material translation output failed typed validation.");
				ProducerMicroseconds.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Begin).count());
				Context.AddValue(FValueId::FromName("Product"), std::move(*Encoded));
				FBinaryWriter Timing; Timing.WriteU64(Product.Timings.NormalizationMicroseconds); Timing.WriteU64(Product.Timings.GenerationMicroseconds);
				if (auto Observation = MakeBuildMetadata(FSharedByteBuffer::Take(Timing.TakeBytes()))) Context.AddObservation(FValueId::FromName("Timings"), std::move(*Observation));
			}
		};
		struct FRequest
		{
			FBuildDefinition Definition;
			std::shared_ptr<const FResolver> Resolver;
		};
		auto MakeRequest(const MIR::FCompilerInput& Input, std::shared_ptr<const FMaterialTranslationInput> Encoded = {}) -> std::optional<FRequest>
		{
			if (!Encoded)
			{
				auto Bytes = EncodeTranslationInput(Input); if (!Bytes) return {};
				Encoded = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{*Bytes, FXxHash128::HashBuffer(Bytes->GetBytes())});
			}
			auto Resolver = std::make_shared<FResolver>(); Resolver->Bytes = Encoded->Bytes; Resolver->Identity = Encoded->Identity;
			FBuildDefinitionBuilder Builder("Material.Translate");
			Builder.AddConstant("Generator", GetMaterialBuildVersion()).AddConstant("MIR", uint64(MIR::CurrentVersion))
				.AddConstant("Layout", uint64(CompiledMaterialRenderLayoutVersion)).AddConstant("Pass", uint64(CurrentMaterialPassContractVersion))
				.AddConstant("InputIdentity", Resolver->Identity).AddInput("Input", "CapturedMIR");
			auto Definition = std::move(Builder).Build(); if (!Definition) return {};
			return FRequest{std::move(*Definition), std::move(Resolver)};
		}
		auto ConsumerError(const MIR::FCompilerInput& Input) -> FMaterialError
		{
			if (auto Validation = ValidateMaterialNormalizationEnvironment(Input); !Validation) return Validation.Diagnostics.front().Error;
			if (Input.Environment.CompilerIdentity.empty() || Input.Environment.BuilderVersion != GetMaterialBuildVersion() || Input.Environment.AdapterVersion != GetMaterialCompileAdapterVersion()
				|| Input.Environment.Target.empty() || Input.Environment.Dependencies.size() > 64 || Input.Environment.CompilerIdentity.size() > MaterialProgramMaxStringBytes
				|| Input.Environment.Target.size() > MaterialProgramMaxStringBytes) return EMaterialIRError::InvalidCompilerEnvironment;
			if (Input.Sources.size() > MaterialFunctionMaxExpandedNodes) return EMaterialIRError::SourceMetadataCountExceedsBound;
			uint64 Bytes = 0;
			for (const auto& Source : Input.Sources)
			{
				Bytes += sizeof(Source) + Source.FunctionAssetPath.capacity() + Source.CallPath.capacity() * sizeof(FGuid);
				if (Source.ExpressionIndex >= Input.IR.Nodes.size() || Source.CallPath.size() > MaterialFunctionMaxCallDepth || Source.FunctionAssetPath.size() > MaterialProgramMaxStringBytes
					|| Bytes > MaterialFunctionMaxClosureBytes || (Source.InputIndex && *Source.InputIndex >= MaterialProgramMaxNodeInputCount) || (Source.UVFieldIndex && *Source.UVFieldIndex >= 4))
					return EMaterialIRError::SourceMetadataInvalidExceedsBound;
			}
			return {};
		}
		auto Options(const std::shared_ptr<const IBuildInputResolver>& Resolver, bool Force, FBuildCancellation Cancellation = {}, bool Query = true) -> FBuildRequestOptions
		{
			FBuildRequestOptions Value; Value.InputResolver = Resolver; Value.Policy.ForceBuild = Force; Value.Policy.QueryCache = Query; Value.Cancellation = std::move(Cancellation);
			Value.Policy.InputLimits.MaximumTotalBytes = TranslationInputBytes; Value.Policy.OutputLimits.MaximumTotalBytes = TranslationOutputBytes;
			Value.Policy.PersistenceLimits = Value.Policy.OutputLimits; Value.Policy.MaximumEncodedBytes = TranslationOutputBytes + 1024 * 1024;
			return Value;
		}
		auto Assemble(const FBuildCompleteParams& Completion, FXxHash128 Identity, const MIR::FCompilerInput& Input) -> std::optional<FMaterialTranslatedProgram>
		{
			const auto* Output = Completion.GetOutput();
			if (!Output || Output->GetSchema() != "Material.Translation" || Output->GetSchemaVersion() != TranslationSchema || Output->GetValues().size() != 1) return {};
			const auto* Value = Output->FindValue(FValueId::FromName("Product")); if (!Value) return {};
			auto Product = DecodeTranslationProduct(Value->GetData().GetBytes(), Identity, Input);
			if (Product)
			{
				if (HasBuildExecutionFlag(Completion.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit)) ++Hits;
				for (const auto& Observation : Output->GetObservations()) if (Observation.Id == FValueId::FromName("Timings"))
				{
					auto Bytes = Observation.Object.GetView().Find("Payload").AsBinary(); if (Bytes) { FBinaryReader Reader(Bytes->GetBytes()); Reader.ReadU64(Product->Timings.NormalizationMicroseconds); Reader.ReadU64(Product->Timings.GenerationMicroseconds); }
				}
			}
			return Product;
		}
	}
	auto MakeTranslationFunction() -> std::shared_ptr<const IBuildFunction> { return std::make_shared<FFunction>(); }
	auto GetTranslationStats() -> FTranslationStats { return {Producers.load(), Resolutions.load(), Hits.load(), ProducerMicroseconds.load(), GameThreadProducers.load()}; }
	auto RegisterTranslation() -> void
	{
		static std::once_flag Once; std::call_once(Once, [] { require(GetBuild().Register(MakeTranslationFunction()).has_value()); });
	}
	auto OpenTranslationSessions() -> void { RegisterTranslation(); Sessions.Open(); }
	auto SetTranslationServiceForTests(std::shared_ptr<IBuild> Service) -> void { require(Sessions.SetTaskService(std::move(Service)) == EBuildDrainResult::Drained); }
	auto CloseTranslationSessions() -> void { require(Sessions.Close() == EBuildDrainResult::Drained); }
	static auto SubmitValidatedTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner,
		std::function<void(FMaterialTranslatedProgram)> Completion, FBuildCancellation Cancellation, bool Query, std::shared_ptr<const FMaterialTranslationInput> EncodedInput, EBuildCacheExecution Execution)
		-> std::expected<FBuildRequest, FBuildAdmissionError>;
	auto TranslateMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialTranslatedProgram
	{
		if (auto Error = ConsumerError(Input); Error.HasError()) return Failure(Error);
		auto Bytes = EncodeTranslationInput(Input); if (!Bytes) return Failure(EMaterialIRError::CanonicalBytesExceedSupportedBound);
		auto Encoded = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{*Bytes, FXxHash128::HashBuffer(Bytes->GetBytes())});
		FMaterialTranslatedProgram Product; FRequestOwner Owner;
		auto Admitted = SubmitValidatedTranslation(std::make_shared<const MIR::FCompilerInput>(Input), Force, Owner,
			[&](auto Value) { Product = std::move(Value); }, {}, true, std::move(Encoded), EBuildCacheExecution::Inline);
		if (!Admitted) return Failure(FMaterialError::FromExternal(EMaterialCompileError::AdmissionRejected, Admitted.error().Description));
		require(Owner.Wait() == ERequestWaitResult::Completed);
		return Product;
	}
	static auto SubmitValidatedTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner,
		std::function<void(FMaterialTranslatedProgram)> Completion, FBuildCancellation Cancellation, bool Query, std::shared_ptr<const FMaterialTranslationInput> EncodedInput, EBuildCacheExecution Execution)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
	{
		RegisterTranslation(); auto Request = MakeRequest(*Input, std::move(EncodedInput)); if (!Request) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Capacity, "Material snapshot exceeds the input limit."});
		auto Session = Sessions.Get(Execution); if (!Session) return std::unexpected(Session.error());
		return (*Session)->Build(Request->Definition, Owner,
			[Input, Force, &Owner, Completion = std::move(Completion), Cancellation, Query, Execution, Encoded = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{Request->Resolver->Bytes, Request->Resolver->Identity}), Identity = Request->Resolver->Identity](FBuildCompleteParams Result) mutable {
				if (Result.GetStatus() == EStatus::Ok)
				{
					if (auto Product = Assemble(Result, Identity, *Input)) return Completion(std::move(*Product));
					if (Query && HasBuildExecutionFlag(Result.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit))
					{
						// Retry ownership joins the same owner; only the final typed callback is delivered.
						auto Retried = SubmitValidatedTranslation(Input, Force, Owner, Completion, Cancellation, false, Encoded, Execution);
						if (Retried) return;
						return Completion(Failure(FMaterialError::FromExternal(EMaterialCompileError::AdmissionRejected, Retried.error().Description)));
					}
				}
				if (Result.GetStatus() == EStatus::Canceled) return Completion({.Status = EMaterialProgramStatus::Canceled});
				std::string Diagnostic(Result.GetDiagnostic());
				Diagnostic.resize(std::min<size_t>(Diagnostic.size(), MaterialProgramMaxDiagnosticMessageBytes));
				if (const auto* Output = Result.GetOutput())
					for (const auto& Message : Output->GetMessages())
					{
						if (Diagnostic.size() >= MaterialProgramMaxDiagnosticMessageBytes) break;
						if (!Diagnostic.empty()) Diagnostic += '\n';
						Diagnostic.append(Message.Text, 0, MaterialProgramMaxDiagnosticMessageBytes - Diagnostic.size());
					}
				Completion(Failure(Diagnostic.empty() ? FMaterialError(EMaterialIRError::InvalidStructure)
					: FMaterialError::FromExternal(EMaterialCompileError::ShaderCompilerFailed, std::move(Diagnostic))));
			}, {}, Options(Request->Resolver, Force, Cancellation, Query));
	}
	auto SubmitTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner,
		std::function<void(FMaterialTranslatedProgram)> Completion, FBuildCancellation Cancellation, bool Query,
		std::shared_ptr<const FMaterialTranslationInput> EncodedInput, EBuildCacheExecution Execution, FMaterialError* ConsumerValidationError)
		-> std::expected<FBuildRequest, FBuildAdmissionError>
	{
		const auto Error = ConsumerError(*Input);
		if (ConsumerValidationError) *ConsumerValidationError = Error;
		if (Error.HasError()) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, FormatMaterialError(Error)});
		return SubmitValidatedTranslation(std::move(Input), Force, Owner, std::move(Completion), std::move(Cancellation), Query, std::move(EncodedInput), Execution);
	}

}
#endif

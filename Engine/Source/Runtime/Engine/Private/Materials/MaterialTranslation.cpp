#include "MaterialTranslation.h"
#if DURIN_WITH_EDITOR
#include "MaterialTranslationCodec.h"
#include "Asset/ResourceBuildSessions.h"
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
		auto Failure(std::string Message) -> FMaterialTranslatedProgram
		{ FMaterialTranslatedProgram Product; Product.Diagnostics.push_back({.Message = std::move(Message)}); return Product; }
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
		auto ConsumerError(const MIR::FCompilerInput& Input) -> std::string
		{
			if (auto Validation = ValidateMaterialNormalizationEnvironment(Input); !Validation) return FormatMaterialProgramDiagnostics(Validation.Diagnostics);
			if (Input.Environment.CompilerIdentity.empty() || Input.Environment.BuilderVersion != GetMaterialBuildVersion() || Input.Environment.AdapterVersion != GetMaterialCompileAdapterVersion()
				|| Input.Environment.Target.empty() || Input.Environment.Dependencies.size() > 64 || Input.Environment.CompilerIdentity.size() > MaterialProgramMaxStringBytes
				|| Input.Environment.Target.size() > MaterialProgramMaxStringBytes) return "Material compiler environment identity, target, pass contract, or dependency bounds are invalid.";
			if (Input.Sources.size() > MaterialFunctionMaxExpandedNodes) return "IR source metadata count exceeds its bound.";
			uint64 Bytes = 0;
			for (const auto& Source : Input.Sources)
			{
				Bytes += sizeof(Source) + Source.FunctionAssetPath.capacity() + Source.CallPath.capacity() * sizeof(FGuid);
				if (Source.ExpressionIndex >= Input.IR.Nodes.size() || Source.CallPath.size() > MaterialFunctionMaxCallDepth || Source.FunctionAssetPath.size() > MaterialProgramMaxStringBytes
					|| Bytes > MaterialFunctionMaxClosureBytes || (Source.InputIndex && *Source.InputIndex >= MaterialProgramMaxNodeInputCount) || (Source.UVFieldIndex && *Source.UVFieldIndex >= 4))
					return "IR source metadata is invalid or exceeds its bound.";
			}
			return {};
		}
		auto Options(const std::shared_ptr<const IBuildInputResolver>& Resolver, bool Force, bool Query = true) -> FBuildRequestOptions
		{
			FBuildRequestOptions Value; Value.InputResolver = Resolver; Value.Policy = Force || !Query ? EBuildPolicy::Build | EBuildPolicy::CacheStore : EBuildPolicy::Default;
			Value.Limits.Input.MaximumTotalBytes = TranslationInputBytes; Value.Limits.Output.MaximumTotalBytes = TranslationOutputBytes;
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
	static auto SubmitValidatedTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner, std::function<void(FMaterialTranslatedProgram)> Completion, bool Query, std::shared_ptr<const FMaterialTranslationInput> EncodedInput, EBuildCacheExecution Execution)
		-> std::expected<void, FBuildAdmissionError>;
	auto TranslateMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialTranslatedProgram
	{
		if (auto Error = ConsumerError(Input); !Error.empty()) return {.Diagnostics = {{.Message = std::move(Error)}}};
		auto Bytes = EncodeTranslationInput(Input); if (!Bytes) return Failure("Material IR canonical bytes exceed the supported byte bound.");
		auto Encoded = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{*Bytes, FXxHash128::HashBuffer(Bytes->GetBytes())});
		FMaterialTranslatedProgram Product; FRequestOwner Owner;
		auto Admitted = SubmitValidatedTranslation(std::make_shared<const MIR::FCompilerInput>(Input), Force, Owner,
			[&](auto Value) { Product = std::move(Value); }, true, std::move(Encoded), EBuildCacheExecution::Inline);
		if (!Admitted) return Failure(Admitted.error().Description);
		require(Owner.Wait() == ERequestWaitResult::Completed);
		return Product;
	}
	static auto SubmitValidatedTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner, std::function<void(FMaterialTranslatedProgram)> Completion, bool Query, std::shared_ptr<const FMaterialTranslationInput> EncodedInput, EBuildCacheExecution Execution)
		-> std::expected<void, FBuildAdmissionError>
	{
		RegisterTranslation(); auto Request = MakeRequest(*Input, std::move(EncodedInput)); if (!Request) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::Capacity, "Material snapshot exceeds the input limit."});
		auto Session = Sessions.Get(Execution); if (!Session) return std::unexpected(Session.error());
		(*Session)->Build(Request->Definition, Owner, [Input, Force, &Owner, Completion = std::move(Completion), Query, Execution, Encoded = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{Request->Resolver->Bytes, Request->Resolver->Identity}), Identity = Request->Resolver->Identity](FBuildCompleteParams Result) mutable {
				if (Result.GetStatus() == EStatus::Ok)
				{
					if (auto Product = Assemble(Result, Identity, *Input)) return Completion(std::move(*Product));
					if (Query && HasBuildExecutionFlag(Result.GetExecutionFlags(), EBuildExecutionFlags::CacheQueryHit))
					{
						// Retry ownership joins the same owner; only the final typed callback is delivered.
						auto Retried = SubmitValidatedTranslation(Input, Force, Owner, Completion, false, Encoded, Execution);
						if (Retried) return;
						return Completion(Failure(Retried.error().Description));
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
				Completion(Failure(Diagnostic.empty() ? "Material IR version, node count, or surface output count is invalid."
					: std::move(Diagnostic))); }, {}, Options(Request->Resolver, Force, Query));
		return {};
	}
	auto SubmitTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner, std::function<void(FMaterialTranslatedProgram)> Completion, bool Query, std::shared_ptr<const FMaterialTranslationInput> EncodedInput, EBuildCacheExecution Execution)
		-> std::expected<void, FBuildAdmissionError>
	{
		const auto Error = ConsumerError(*Input);
		if (!Error.empty()) return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::InvalidRequest, Error});
		return SubmitValidatedTranslation(std::move(Input), Force, Owner, std::move(Completion), Query, std::move(EncodedInput), Execution);
	}

}
#endif

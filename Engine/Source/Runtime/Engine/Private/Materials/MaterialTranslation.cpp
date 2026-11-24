#include "MaterialTranslation.h"
#if DURIN_WITH_EDITOR

#include "MaterialTranslationCodec.h"
#include "Asset/ResourceBuildSessions.h"
#include "CoreGlobals.h"
#include "DerivedDataBuildFunctionFactory.h"
#include "Threading/RunnableThread.h"

namespace Durin::MaterialBuildPrivate
{
	using namespace DerivedData;
	namespace
	{
		AssetPrivate::FResourceBuildSessions Sessions([] {
			DerivedData::FBuildServiceOptions Options;
			Options.BuildLimits.Input.MaximumTotalBytes = TranslationInputBytes;
			Options.BuildLimits.Output.MaximumTotalBytes = TranslationOutputBytes;
			return DerivedData::CreateBuildWithRegisteredFunctions(std::move(Options));
		});
		std::atomic_uint64_t Producers = 0, Resolutions = 0, Hits = 0, ProducerMicroseconds = 0, GameThreadProducers = 0;
		auto InputReference(FXxHash128 Identity) -> FBuildInputReference { return {"Input", Identity, "Material.TranslationInput", 1, "Material.MIR", 1}; }
		auto Failure(std::string Message) -> FMaterialTranslatedProgram
		{ FMaterialTranslatedProgram Product; Product.Diagnostics.push_back({.Message = std::move(Message)}); return Product; }
		class FResolver final : public IBuildInputResolver
		{
		public:
			explicit FResolver(std::shared_ptr<const FMaterialTranslationInput> Input) : Encoded(std::move(Input)) {}
			const std::shared_ptr<const FMaterialTranslationInput> Encoded;
			auto ResolveInputMeta(const FBuildDefinition& Definition, IRequestOwner& Owner, FOnBuildInputMetaResolved&& OnResolved) const -> void override
			{
				const auto Sources = Definition.GetSources();
				if (Owner.IsCanceled()) return OnResolved({{}, EStatus::Canceled});
				if (Sources.size() != 1 || Sources.front().Name != "Input" || Sources.front().Source != "CapturedMIR") return OnResolved({{}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Error});
				return OnResolved({std::vector{InputReference(Encoded->Identity)}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Ok});
			}
			auto ResolveInputData(const FBuildAction& Action, IRequestOwner& Owner, FOnBuildInputDataResolved&& OnResolved) const -> void override
			{
				const auto Inputs = Action.GetInputs();
				if (Owner.IsCanceled()) return OnResolved({{}, EStatus::Canceled});
				++Resolutions;
				if (Inputs.size() != 1 || Inputs.front() != InputReference(Encoded->Identity) || FXxHash128::HashBuffer(Encoded->Bytes.GetBytes()) != Encoded->Identity)
					return OnResolved({{}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Error});
				return OnResolved({std::vector<FBuildInput>{{.Identity = Inputs.front(), .Values = {{"MIR", Encoded->Bytes}}}}, Owner.IsCanceled() ? EStatus::Canceled : EStatus::Ok});
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
				if (Context.IsCanceled()) return;
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
			if (!Encoded) Encoded = CaptureTranslationInput(Input);
			if (!Encoded) return {};
			FBuildDefinitionBuilder Builder("Material.Translate");
			Builder.AddConstant("Generator", GetMaterialBuildVersion()).AddConstant("MIR", uint64(MIR::CurrentVersion))
				.AddConstant("Layout", uint64(CompiledMaterialRenderLayoutVersion)).AddConstant("Pass", uint64(CurrentMaterialPassContractVersion))
				.AddConstant("InputIdentity", Encoded->Identity).AddInput("Input", "CapturedMIR");
			auto Definition = std::move(Builder).Build(); if (!Definition) return {};
			return FRequest{std::move(*Definition), std::make_shared<FResolver>(std::move(Encoded))};
		}
		auto Assemble(const FBuildCompleteParams& Completion, FXxHash128 Identity, const MIR::FCompilerInput& Input) -> std::optional<FMaterialTranslatedProgram>
		{
			const auto& Output = Completion.Output;
			if (Output.GetSchema() != "Material.Translation" || Output.GetSchemaVersion() != TranslationSchema || Output.GetValues().size() != 1) return {};
			const auto* Value = Output.FindValue(FValueId::FromName("Product")); if (!Value) return {};
			auto Product = DecodeTranslationProduct(Value->GetData().GetBytes(), Identity, Input);
			if (Product)
			{
				if (HasBuildExecutionFlag(Completion.BuildStatus, EBuildExecutionFlags::CacheQueryHit)) ++Hits;
			}
			return Product;
		}
	}
	auto MakeTranslationFunction() -> std::shared_ptr<const IBuildFunction> { return std::make_shared<FFunction>(); }
	namespace
	{
		const TBuildFunctionFactory<MakeTranslationFunction> TranslationBuildFactory;
	}
	auto GetTranslationStats() -> FTranslationStats { return {Producers.load(), Resolutions.load(), Hits.load(), ProducerMicroseconds.load(), GameThreadProducers.load()}; }
	auto SetTranslationServiceForTests(std::shared_ptr<IBuild> Service) -> void { Sessions.SetService(std::move(Service)); }
	auto ReleaseTranslationSession() -> void { Sessions.Reset(); }
	static auto SubmitValidatedTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner, std::function<void(FMaterialTranslatedProgram)> Completion, bool Query, std::shared_ptr<const FMaterialTranslationInput> EncodedInput)
		-> void;
	auto TranslateMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialTranslatedProgram
	{
		if (auto Error = ValidateMaterialCompileRequest(Input); !Error.empty()) return {.Diagnostics = {{.Message = std::move(Error)}}};
		auto Encoded = CaptureTranslationInput(Input);
		if (!Encoded) return Failure("Material IR canonical bytes exceed the supported byte bound.");
		FMaterialTranslatedProgram Product; FRequestOwner Owner;
		SubmitValidatedTranslation(std::make_shared<const MIR::FCompilerInput>(Input), Force, Owner,
			[&](auto Value) { Product = std::move(Value); }, true, std::move(Encoded));
		Owner.Wait();
		return Product;
	}
	static auto SubmitValidatedTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner, std::function<void(FMaterialTranslatedProgram)> Completion, bool Query, std::shared_ptr<const FMaterialTranslationInput> EncodedInput)
		-> void
	{
		auto Request = MakeRequest(*Input, std::move(EncodedInput)); if (!Request) { Completion(Failure("Material snapshot exceeds the input limit.")); return; }
		auto Session = Sessions.Get(Request->Resolver);
		Session.Build(Request->Definition, {}, Force || !Query ? EBuildPolicy::Build | EBuildPolicy::CacheStore : EBuildPolicy::Default, Owner, [Input, Force, &Owner, Completion = std::move(Completion), Query, Encoded = Request->Resolver->Encoded](FBuildCompleteParams Result) mutable {
				if (Result.Status == EStatus::Ok)
				{
					if (auto Product = Assemble(Result, Encoded->Identity, *Input)) return Completion(std::move(*Product));
					if (Query && HasBuildExecutionFlag(Result.BuildStatus, EBuildExecutionFlags::CacheQueryHit))
					{
						// Retry ownership joins the same owner; only the final typed callback is delivered.
						SubmitValidatedTranslation(Input, Force, Owner, std::move(Completion), false, Encoded);
						return;
					}
				}
				if (Result.Status == EStatus::Canceled) return Completion({.Status = EMaterialProgramStatus::Canceled});
				std::string Diagnostic((Result.Output.GetLogs().empty() ? std::string_view{} : std::string_view(Result.Output.GetLogs().front().Text)));
				Diagnostic.resize(std::min<size_t>(Diagnostic.size(), MaterialProgramMaxDiagnosticMessageBytes));
				for (const auto& Message : Result.Output.GetMessages())
					{
						if (Diagnostic.size() >= MaterialProgramMaxDiagnosticMessageBytes) break;
						if (!Diagnostic.empty()) Diagnostic += '\n';
						Diagnostic.append(Message.Text, 0, MaterialProgramMaxDiagnosticMessageBytes - Diagnostic.size());
					}
				Completion(Failure(Diagnostic.empty() ? "Material IR version, node count, or surface output count is invalid."
					: std::move(Diagnostic))); });
	}
	auto SubmitTranslation(std::shared_ptr<const MIR::FCompilerInput> Input, bool Force, IRequestOwner& Owner, std::function<void(FMaterialTranslatedProgram)> Completion, bool Query, std::shared_ptr<const FMaterialTranslationInput> EncodedInput)
		-> void
	{
		const auto Error = ValidateMaterialCompileRequest(*Input);
		if (!Error.empty()) { Completion(Failure(Error)); return; }
		SubmitValidatedTranslation(std::move(Input), Force, Owner, std::move(Completion), Query, std::move(EncodedInput));
	}

}
#endif

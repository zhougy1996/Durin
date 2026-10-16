#include "Materials/Material.h"
#include "MaterialCompileRetryQueue.h"

#include "Asset/Asset.h"
#include "Asset/CookedPreparation.h"
#include "Asset/AssetCook.h"
#include "Asset/CookDependencies.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialParameterCollection.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#if DURIN_WITH_EDITOR
#include "TargetPlatform.h"
#endif

namespace Durin
{
	auto DMaterialInterface::BeginCacheForCookedPlatformData(FObjectCookContext& Context) -> void
	{
		Super::BeginCacheForCookedPlatformData(Context);
		if (IsDynamicInstance() || Context.CheckCancellation()) return;
		auto* Preparation = FCookPreparationContext::Get(*this, Context);
		if (!Preparation) { Context.Result = {EObjectCookError::InvalidInput, "Missing material Cook preparation context."}; return; }
		const FAssetBuildTaskContext Control{.Cancellation = FCancellationToken(Context.IsCancelled)};
		auto Built = PrepareCookedProgram(Preparation->GetTargetPlatform(), Preparation->Options.MaterialConfigurations, Control);
		if (Built.Status == EShaderBuildStatus::Canceled) { Context.Result = {EObjectCookError::Cancelled, {}}; return; }
		if (!Built)
			Preparation->Failure = {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::Preparation,
				.ObjectPath = GetObjectPath(), .VirtualPath = Context.Package, .Diagnostic = std::move(Built.Diagnostic)};
		else Preparation->Primary = std::move(*Built);
	}
	namespace
	{
		std::atomic<EMaterialQualityLevel> GMaterialQualityLevel{
			EMaterialQualityLevel::High};
	}

	auto GetMaterialQualityLevel() -> EMaterialQualityLevel
	{
		return GMaterialQualityLevel.load(std::memory_order_acquire);
	}

	auto SetMaterialQualityLevel(EMaterialQualityLevel Quality) -> bool
	{
		if (!IsSupportedMaterialQualityLevel(Quality)) return false;
		GMaterialQualityLevel.store(Quality, std::memory_order_release);
		return true;
	}

	auto DMaterialInterface::LoadCookedProgram() -> FMaterialOperationResult
	{
		auto FailCooked = [&](FMaterialError Error) -> FMaterialOperationResult {
			MaterialCookDiagnostic = Error;
			return {std::move(Error)};
		};
		auto Read = CookedProgramData.AcquireRead();
		if (!Read)
		{
			return FailCooked(FMaterialError::FromBulkRead(Read.Status, Read.Error));
		}
		const FByteView Bytes = Read.Lock.GetBytes();

		FMaterialStaticProperties PayloadProperties;
		std::shared_ptr<const FMaterialCompilerResult> ProgramCandidate;
		const ERHIFeatureLevel FeatureLevel = GDynamicRHI
			&& GDynamicRHI->RHIGetCapabilities()
			? GDynamicRHI->RHIGetCapabilities()->FeatureLevel
			: ERHIFeatureLevel::SM5;
		const auto Decoded = DecodeMaterialCookedProgramFamily(
			Bytes,
			GetActiveRHIBackend().value_or(
				ResolveRHIBackend(std::getenv("DURIN_RHI_BACKEND")).value_or(ERHIBackend::Vulkan)) == ERHIBackend::Metal
				? ECookTargetPlatform::MacOS : ECookTargetPlatform::Win64,
			ECookTargetProfile::Game,
			GetMaterialQualityLevel(), FeatureLevel, {},
			PayloadProperties, ProgramCandidate);
		if (!Decoded)
		{
			return FailCooked(Decoded.Error);
		}
		auto ExpectedProperties = GetStaticProperties();
		ExpectedProperties.OpacityMaskThreshold = CanonicalizeMaterialShaderProperties(ExpectedProperties).OpacityMaskThreshold;
		if (PayloadProperties != ExpectedProperties)
		{
			return FailCooked(
				EMaterialCookError::StaticPropertiesMismatch);
		}
		for (const auto& Parameter : ProgramCandidate->ActiveParameters)
		{
			const auto* Definition = FindParameterDefinition(Parameter.Id);
			if (!Definition || Definition->Type != Parameter.Type)
			{
				return FailCooked(EMaterialCookError::ParameterContractMismatch);
			}
		}
		if (!ProgramCandidate->ActiveCollections.empty())
		{
			auto ResolvedProgram = std::make_shared<FMaterialCompilerResult>(
				*ProgramCandidate);
			for (auto& Required : ResolvedProgram->ActiveCollections)
			{
				// Empty paths exist only for detached/transient compiler fixtures; real
				// package-authored collection expressions always capture an exact path.
				if (Required.AssetPath.empty()) continue;
				FObjectPath Path;
				if (!FObjectPath::TryCreate(Required.AssetPath, Path))
					return FailCooked(
						EMaterialCookError::CollectionContractMismatch);
				auto Collection = LoadObject<DMaterialParameterCollection>(Path);
				if (!Collection)
					return FailCooked(
						EMaterialCookError::CollectionContractMismatch);
				auto Current = (*Collection)->BuildLayout();
				if (!Current || !Required.HasCompatibleSchema(*Current))
					return FailCooked(
						EMaterialCookError::CollectionContractMismatch);
				Required.DefaultPayload = std::move(Current->DefaultPayload);
			}
			ProgramCandidate = std::move(ResolvedProgram);
		}
		Read.Lock.Reset();

		CompilationOwner.RenderLayer.CompiledProgram = std::move(ProgramCandidate);
		CompilationOwner.RenderLayer.StaticProperties = PayloadProperties;
		Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(this));
		CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Ready;
		CompilationOwner.MaterialCompileStatus.ResultCategory =
			EMaterialCompileResultCategory::None;
		CompilationOwner.MaterialCompileStatus.CacheOutcome =
			EMaterialCompileCacheOutcome::None;
		CompilationOwner.MaterialCompileStatus.RequestGeneration = 1;
		CompilationOwner.MaterialCompileStatus.RequestedIdentity =
			CompilationOwner.RenderLayer.CompiledProgram->Identity;
		CompilationOwner.MaterialCompileStatus.CompiledIdentity =
			CompilationOwner.RenderLayer.CompiledProgram->Identity;
		CompilationOwner.MaterialCompileStatus.Target = CompilationOwner.RenderLayer.CompiledProgram->Target;
		CompilationOwner.MaterialCompileDiagnostics.clear();
		MaterialCookDiagnostic = {};
		PublishMaterialRenderProxyState();
		return {};
	}

	auto DMaterialInterface::PrepareCookedProgram(ECookTargetPlatform TargetPlatform,
		std::span<const FMaterialCookConfiguration> Requested, const FAssetBuildTaskContext& Control)
		-> TShaderValueResult<FBulkData>
	{
		if (Control.IsCancelled()) return FShaderCompletion::Canceled();
		if (IsDynamicInstance()) return FBulkData{};
		const auto Configurations = ResolveMaterialCookConfigurations(Requested);
		if (!Configurations) return FShaderCompletion::Failure("Material Cook configuration is unsupported.");
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (GetAssetRuntimeConfiguration().GetCookTargetPlatform() != TargetPlatform || !CookedProgramData.HasData())
				return FShaderCompletion::Failure("Material cooked program data is unavailable or targets another platform.");
			auto Read = CookedProgramData.AcquireRead();
			if (!Read) return FShaderCompletion::Failure("Material cooked program data could not be read.");
			std::vector<std::shared_ptr<const FMaterialCompilerResult>> Variants;
			FMaterialStaticProperties Properties;
			for (const auto& Configuration : *Configurations)
			{
				if (Control.IsCancelled()) return FShaderCompletion::Canceled();
				std::shared_ptr<const FMaterialCompilerResult> Program;
				const auto Decoded = DecodeMaterialCookedProgramFamily(Read.Lock.GetBytes(), TargetPlatform,
					ECookTargetProfile::Game, Configuration.Quality, Configuration.FeatureLevel, {}, Properties, Program);
				if (!Decoded) return FShaderCompletion::Failure(FormatMaterialError(Decoded.Error));
				Variants.push_back(std::move(Program));
			}
			std::vector<const FMaterialCompilerResult*> Programs;
			for (const auto& Variant : Variants) Programs.push_back(Variant.get());
			FByteBuffer Bytes;
			const auto Encoded = EncodeMaterialCookedProgramFamily(Programs, Properties,
				TargetPlatform, ECookTargetProfile::Game, Bytes);
			if (!Encoded) return FShaderCompletion::Failure(FormatMaterialError(Encoded.Error));
			auto Bulk = FBulkData::TryCreateDetached(Bytes);
			if (!Bulk) return FShaderCompletion::Failure(FormatBulkDataError(Bulk.error()));
			if (Control.IsCancelled()) return FShaderCompletion::Canceled();
			return std::move(*Bulk);
		}
#if DURIN_WITH_EDITOR
		const auto Target = ResolveTargetPlatform(TargetPlatform);
		if (!Target)
			return FShaderCompletion::Failure("Material Cook target or configuration is unsupported.");
		FMaterialCompilerEnvironment BaseEnvironment;
		if (auto Environment = BuildDefaultMaterialCompilerEnvironment(BaseEnvironment, Target->GetShaderTarget()); !Environment)
			return FShaderCompletion::Failure(FormatMaterialError(Environment.Error));
		std::vector<FMaterialCompilerResult> Variants;
		Variants.reserve(Configurations->size());
		for (const auto& Configuration : *Configurations)
		{
			if (Control.IsCancelled()) return FShaderCompletion::Canceled();
			auto Environment = BaseEnvironment;
			Environment.Quality = Configuration.Quality;
			Environment.FeatureLevel = Configuration.FeatureLevel;
			auto Snapshot = SnapshotMaterialCompilerInput(*this, std::move(Environment));
			if (!Snapshot) return FShaderCompletion::Failure(FormatMaterialProgramDiagnostics(Snapshot.Diagnostics, "Material cooked configuration snapshot failed."));
			auto Compiled = MIR::Compile(Snapshot.Snapshot->Input);
			if (Compiled.Status == EMaterialProgramStatus::Canceled || Control.IsCancelled()) return FShaderCompletion::Canceled();
			if (!Compiled) return FShaderCompletion::Failure(FormatMaterialProgramDiagnostics(Compiled.Diagnostics, "Material cooked configuration compilation failed."));
			Variants.push_back(std::move(Compiled));
		}
		if (Control.IsCancelled()) return FShaderCompletion::Canceled();
		std::vector<const FMaterialCompilerResult*> Programs;
		for (const auto& Variant : Variants) Programs.push_back(&Variant);
		FByteBuffer Bytes;
		const auto Encoded = EncodeMaterialCookedProgramFamily(Programs,
			CanonicalizeMaterialShaderProperties(GetStaticProperties()), TargetPlatform, ECookTargetProfile::Game, Bytes);
		if (!Encoded) return FShaderCompletion::Failure(FormatMaterialError(Encoded.Error));
		auto Bulk = FBulkData::TryCreateDetached(Bytes);
		if (!Bulk) return FShaderCompletion::Failure(FormatBulkDataError(Bulk.error()));
		if (Control.IsCancelled()) return FShaderCompletion::Canceled();
		return std::move(*Bulk);
#else
		return FShaderCompletion::Failure("Authored material Cook preparation is unavailable in this runtime.");
#endif
	}

	auto DMaterialInterface::SerializeCooked(FArchive& Ar) -> void
	{
		auto* Prepared = Ar.IsSaving() && !IsDynamicInstance()
			? AssetPrivate::FScopedCookedPreparation::Find(*this, Ar.GetTarget()) : nullptr;
		if (Ar.IsSaving() && !IsDynamicInstance() && !Prepared)
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Material serialization requires current prepared Cook data.");
			return;
		}
		Super::SerializeCooked(Ar);
		if (Ar.IsError() || IsDynamicInstance()) return;
		if (!ParseTargetPlatform(Ar.GetTarget().Platform) || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedTarget, "Material cooked program target is unsupported.");
			return;
		}
		FBulkData* Value = &CookedProgramData;
		if (Ar.IsSaving())
		{
			Value = &Prepared->Primary;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DMaterialInterface"),
			FName("ProgramData"), FArchiveLogicalTypeDescriptor::BulkData()});
		Value->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

}

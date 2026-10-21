#include "Materials/Material.h"
#include "Materials/MaterialShaderMap.h"
#include "MaterialCompileRetryQueue.h"
#include "Shader/ShaderCompilingManager.h"
#include "MaterialTranslationCodec.h"
#include "DObject/ObjectKey.h"

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
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) {
			const FAssetBuildTaskContext Control{.Cancellation = FCancellationToken(Context.IsCancelled)};
			auto Built = PrepareCookedProgram(Preparation->GetTargetPlatform(), Preparation->Options.MaterialConfigurations, Control);
			if (Built.Status == EShaderBuildStatus::Canceled) Context.Result = {EObjectCookError::Cancelled, {}};
			else if (!Built) Context.Result = {EObjectCookError::Preparation, std::string(Built.Error)};
			else Preparation->Primary = std::move(*Built);
			return;
		}
#if DURIN_WITH_EDITOR
		const auto Configurations = ResolveMaterialCookConfigurations(Preparation->Options.MaterialConfigurations);
		const auto Target = ResolveTargetPlatform(Preparation->GetTargetPlatform());
		if (!Configurations || !Target) { Context.Result = {EObjectCookError::InvalidInput, "Material Cook target or configuration is unsupported."}; return; }
		// Per variant: bounded snapshot, translation/assembly, program and family encoding overlap.
		if (!Preparation->ReserveWorkingSet(Configurations->size() * 64ull * 1024 * 1024)) return;
		FMaterialCompilerEnvironment BaseEnvironment;
		if (auto Environment = BuildDefaultMaterialCompilerEnvironment(BaseEnvironment, Target->GetCookedShaderTarget()); !Environment) {
			Context.Result = {EObjectCookError::Preparation, Environment.error()}; return;
		}
		BaseEnvironment.bIncludeEditorPasses = false;
		struct FMaterialCookWork
		{
			std::vector<MaterialBuildPrivate::FMaterialProgramCompileHandle> Compiles;
			std::vector<std::optional<FMaterialCompilerResult>> Results;
			std::mutex Mutex;
			std::atomic<bool> Failed = false;
			std::string Failure;
			FCancellationToken Cancellation;
			auto Cancel() -> void {
				Cancellation.Cancel();
				for (const auto& Compile : Compiles) FShaderCompilingManager::Get().CancelCompilation(Compile);
			}
		};
		auto Work = std::make_shared<FMaterialCookWork>();
		std::vector<std::shared_ptr<const MIR::FCompilerInput>> Inputs;
		std::vector<FMaterialFunctionOwnerStamp> FunctionOwners;
		Work->Results.resize(Configurations->size());
		for (const auto& Configuration : *Configurations) {
			if (Context.CheckCancellation()) return;
			auto Environment = BaseEnvironment; Environment.Quality = Configuration.Quality; Environment.FeatureLevel = Configuration.FeatureLevel;
			auto Snapshot = SnapshotMaterialCompilerInput(*this, std::move(Environment));
			if (!Snapshot) { Context.Result = {EObjectCookError::Preparation, FormatMaterialProgramDiagnostics(Snapshot.Diagnostics, "Material Cook snapshot failed.")}; return; }
			if (MaterialBuildPrivate::EstimateTranslationCaptureBytes(Snapshot.Snapshot->Input) > MaterialBuildPrivate::TranslationInputBytes) {
				Context.Result = {EObjectCookError::Limit, "Material Cook snapshot exceeds its reservation."}; return;
			}
			FunctionOwners.insert(FunctionOwners.end(), Snapshot.Snapshot->FunctionOwners.begin(), Snapshot.Snapshot->FunctionOwners.end());
			Inputs.push_back(std::make_shared<const MIR::FCompilerInput>(std::move(Snapshot.Snapshot->Input)));
		}
		struct FOwnerStamp { FObjectKey Owner; uint64 Authored, Dependency, Parent, Program; };
		std::vector<FOwnerStamp> Owners;
		for (const DMaterialInterface* Owner = this; Owner; Owner = Owner->GetParent()) {
			const auto& Status = Owner->GetMaterialCompileStatus();
			const auto* Material = Cast<DMaterial>(Owner);
			Owners.push_back({FObjectKey(Owner), Status.AuthoredRevision, Status.DependencyRevision, Status.ParentChainRevision,
				Material ? Material->GetMaterialProgramRevision() : 0});
		}
		Preparation->ValidatePreparedInputs = [Owners = std::move(Owners), Functions = std::move(FunctionOwners)] {
			if (!AreMaterialFunctionOwnersCurrent(Functions)) return false;
			for (const auto& Stamp : Owners) {
				const auto* Owner = Cast<DMaterialInterface>(ResolveObjectKey(Stamp.Owner));
				if (!Owner) return false;
				const auto& Status = Owner->GetMaterialCompileStatus();
				const auto* Material = Cast<DMaterial>(Owner);
				if (Stamp.Authored != Status.AuthoredRevision || Stamp.Dependency != Status.DependencyRevision
					|| Stamp.Parent != Status.ParentChainRevision || (Material && Stamp.Program != Material->GetMaterialProgramRevision())) return false;
			}
			return true;
		};
		Preparation->CancelWork = [Work] { Work->Cancel(); };
		Preparation->Lifecycle.WaitForCompletion = [Work, Preparation, Platform = Preparation->GetTargetPlatform(),
			Properties = CanonicalizeMaterialShaderProperties(GetStaticProperties())] {
			for (size_t Index = 0; Index < Work->Compiles.size(); ++Index) {
				const auto& Compile = Work->Compiles[Index];
				auto& Manager = FShaderCompilingManager::Get();
				while (Manager.IsCompiling(Compile)) {
					if (Preparation->Lifecycle.CheckCancellation() || Work->Failed.load()) Work->Cancel();
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				Manager.FinishCompilation(Compile);
			}
			if (Preparation->Lifecycle.CheckCancellation()) return;
			if (Work->Failed.load()) { Preparation->Lifecycle.Result = {EObjectCookError::Preparation, Work->Failure}; return; }
			std::vector<const FMaterialCompilerResult*> Programs;
			for (const auto& Result : Work->Results) {
				if (!Result || !*Result) { Preparation->Lifecycle.Result = {EObjectCookError::Cancelled, {}}; return; }
				Programs.push_back(&*Result);
			}
			FByteBuffer Bytes;
			const auto Encoded = EncodeMaterialCookedProgramFamily(Programs, Properties, Platform, ECookTargetProfile::Game, Bytes);
			if (!Encoded) { Preparation->Lifecycle.Result = {EObjectCookError::Preparation, Encoded.error()}; return; }
			auto Bulk = FBulkData::TryCreateDetached(Bytes);
			if (!Bulk) { Preparation->Lifecycle.Result = {EObjectCookError::Preparation, FormatBulkDataError(Bulk.error())}; return; }
			Preparation->Primary = std::move(*Bulk);
		};
		for (size_t Index = 0; Index < Inputs.size(); ++Index) {
			auto Submitted = FMaterialShaderMap::Compile(Inputs[Index],
				{.Cancellation = Work->Cancellation},
				[Work, Index](MaterialBuildPrivate::FMaterialBuildResult Build) {
					const auto& Result = *Build.Program;
					std::lock_guard Lock(Work->Mutex);
					if (!Result && Result.Status != EMaterialProgramStatus::Canceled && !Work->Failed.load()) {
						Work->Failure = FormatMaterialProgramDiagnostics(Result.Diagnostics, "Material Cook compilation failed.");
						Work->Failed.store(true);
					}
					Work->Results[Index] = Result;
				});
			if (!Submitted) {
				{ std::lock_guard Lock(Work->Mutex); Work->Failure = "Material Cook compile admission failed."; Work->Failed.store(true); }
				Work->Cancel(); break;
			}
			Work->Compiles.push_back(std::move(*Submitted));
		}
#else
		Context.Result = {EObjectCookError::Preparation, "Authored material Cook preparation is unavailable in this runtime."};
#endif
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

	auto DMaterialInterface::LoadCookedProgram() -> void
	{
		auto Read = CookedProgramData.AcquireRead();
		if (!Read)
		{
			MaterialCookDiagnostic = std::format("Material cooked payload read failed [bulk status {}].{}",
				static_cast<uint8>(Read.Status), !Read.Error ? " " + FormatPackageResourceReadError(Read.Error) : "");
			return;
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
			MaterialCookDiagnostic = Decoded.error();
			return;
		}
		Read.Lock.Reset();
		const auto Admitted = AdmitCookedProgram(PayloadProperties, std::move(ProgramCandidate));
		MaterialCookDiagnostic = Admitted ? std::string{} : Admitted.error();
	}

	auto DMaterialInterface::AdmitCookedProgram(const FMaterialStaticProperties& PayloadProperties,
		std::shared_ptr<const FMaterialCompilerResult> ProgramCandidate) -> std::expected<void, std::string>
	{
		auto ExpectedProperties = GetStaticProperties();
		ExpectedProperties.OpacityMaskThreshold = CanonicalizeMaterialShaderProperties(ExpectedProperties).OpacityMaskThreshold;
		if (PayloadProperties != ExpectedProperties)
		{
			return std::unexpected("payload static properties do not match package metadata.");
		}
		for (const auto& Parameter : ProgramCandidate->GetShaderMap().ActiveParameters)
		{
			const auto* Definition = FindParameterDefinition(Parameter.Id);
			if (!Definition || Definition->Type != Parameter.Type)
			{
				return std::unexpected("payload parameter contract does not match package metadata.");
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
					return std::unexpected("payload collection asset is missing or its schema does not match the cooked material.");
				auto Collection = LoadObject<DMaterialParameterCollection>(Path);
				if (!Collection)
					return std::unexpected("payload collection asset is missing or its schema does not match the cooked material.");
				auto Current = (*Collection)->BuildLayout();
				if (!Current || !Required.HasCompatibleSchema(*Current))
					return std::unexpected("payload collection asset is missing or its schema does not match the cooked material.");
				Required.DefaultPayload = std::move(Current->DefaultPayload);
			}
			ProgramCandidate = std::move(ResolvedProgram);
		}

		CompilationOwner.RenderLayer.CompiledProgram = std::move(ProgramCandidate);
		CompilationOwner.RenderLayer.StaticProperties = PayloadProperties;
		Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(this));
		CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Ready;
		CompilationOwner.MaterialCompileStatus.CacheOutcome =
			EMaterialCompileCacheOutcome::None;
		CompilationOwner.MaterialCompileStatus.RequestGeneration = 1;
		CompilationOwner.MaterialCompileStatus.RequestedIdentity =
			CompilationOwner.RenderLayer.CompiledProgram->GetShaderMap().ShaderMapId;
		CompilationOwner.MaterialCompileStatus.CompiledIdentity =
			CompilationOwner.RenderLayer.CompiledProgram->GetShaderMap().ShaderMapId;
		CompilationOwner.MaterialCompileStatus.Target = CompilationOwner.RenderLayer.CompiledProgram->GetShaderMap().Target;
		CompilationOwner.MaterialCompileDiagnostics.clear();
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
				if (!Decoded) return FShaderCompletion::Failure(Decoded.error());
				Variants.push_back(std::move(Program));
			}
			std::vector<const FMaterialCompilerResult*> Programs;
			for (const auto& Variant : Variants) Programs.push_back(Variant.get());
			FByteBuffer Bytes;
			const auto Encoded = EncodeMaterialCookedProgramFamily(Programs, Properties,
				TargetPlatform, ECookTargetProfile::Game, Bytes);
			if (!Encoded) return FShaderCompletion::Failure(Encoded.error());
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
		if (auto Environment = BuildDefaultMaterialCompilerEnvironment(BaseEnvironment, Target->GetCookedShaderTarget()); !Environment)
			return FShaderCompletion::Failure(Environment.error());
		BaseEnvironment.bIncludeEditorPasses = false;
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
		if (!Encoded) return FShaderCompletion::Failure(Encoded.error());
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

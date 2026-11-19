#include "Shader/ShaderCompilerCore.h"
#include "Materials/Material.h"
#include "Materials/MaterialShaderMap.h"
#include "MaterialPreparationQueue.h"
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
	namespace
	{
#if DURIN_WITH_EDITOR
		// Shared authored preparation; callers own admission, waiting and publication.
		auto BeginMaterialCookPreparation(DMaterialInterface& Material, FCookedPlatformData& Preparation) -> void
		{
			auto& Context = Preparation;
			const auto Configurations = ResolveMaterialCookConfigurations(Preparation.Options.MaterialConfigurations);
			const auto Target = Context.GetPlatform();
			if (!Configurations || !Target) { Context.Result = {EObjectCookError::InvalidInput, "Material Cook target or configuration is unsupported."}; return; }
			// Per variant: bounded snapshot, translation/assembly, program and family encoding overlap.
			if (!Preparation.ReserveWorkingSet(Configurations->size() * 64ull * 1024 * 1024)) return;
			FMaterialCompilerEnvironment BaseEnvironment;
			if (auto Environment = BuildDefaultMaterialCompilerEnvironment(BaseEnvironment, Target->GetCookedShaderTarget()); !Environment) {
				Context.Result = {EObjectCookError::Preparation, Environment.error()}; return;
			}
			BaseEnvironment.bIncludeEditorPasses = false;
			struct FMaterialCookWork
			{
				std::vector<MaterialBuildPrivate::FMaterialProgramCompileHandle> Compiles;
				std::vector<FMaterialCompilerResult> Results;
				std::mutex Mutex;
				std::atomic<bool> Failed = false;
				std::string Failure;
				Tasks::FCancellationToken Cancellation;
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
				auto Snapshot = SnapshotMaterialCompilerInput(Material, std::move(Environment));
				if (!Snapshot) { Context.Result = {EObjectCookError::Preparation, FormatMaterialProgramDiagnostics(Snapshot.Diagnostics, "Material Cook snapshot failed.")}; return; }
				FunctionOwners.insert(FunctionOwners.end(), Snapshot.Snapshot->FunctionOwners.begin(), Snapshot.Snapshot->FunctionOwners.end());
				Inputs.push_back(std::make_shared<const MIR::FCompilerInput>(std::move(Snapshot.Snapshot->Input)));
			}
			struct FOwnerStamp { FObjectKey Owner; uint64 Authored; };
			std::vector<FOwnerStamp> Owners;
			for (const DMaterialInterface* Owner = &Material; Owner; Owner = Owner->GetParent()) {
				const auto Revision = Owner->GetMaterialContentRevision();
				Owners.push_back({FObjectKey(Owner), Revision});
			}
			Preparation.ValidatePreparedInputs = [Owners = std::move(Owners), Functions = std::move(FunctionOwners), ShaderRevision = GetShaderReloadGeneration()] {
				if (ShaderRevision != GetShaderReloadGeneration()) return false;
				if (!AreMaterialFunctionOwnersCurrent(Functions)) return false;
				for (const auto& Stamp : Owners) {
					const auto* Owner = Cast<DMaterialInterface>(ResolveObjectKey(Stamp.Owner));
					if (!Owner) return false;
					const auto Revision = Owner->GetMaterialContentRevision();
					if (Stamp.Authored != Revision) return false;
				}
				return true;
			};
			Preparation.CancelWork = [Work] { Work->Cancel(); };
			Preparation.WaitWork = [Work] { for (const auto& Compile : Work->Compiles) FShaderCompilingManager::Get().FinishCompilation(Compile); };
			Preparation.PollWork = [Work, &Preparation, Platform = Preparation.GetTargetPlatform(),
				Properties = CanonicalizeMaterialShaderProperties(Material.GetStaticProperties())] {
				if (Preparation.CheckCancellation() || Work->Failed.load()) Work->Cancel();
				for (const auto& Compile : Work->Compiles)
					if (FShaderCompilingManager::Get().IsCompiling(Compile)) return false;
				for (const auto& Compile : Work->Compiles) FShaderCompilingManager::Get().FinishCompilation(Compile);
				if (Preparation.CheckCancellation()) return true;
				if (Work->Failed.load()) { Preparation.Result = {EObjectCookError::Preparation, Work->Failure}; return true; }
				std::vector<const FMaterialProgram*> Programs;
				for (const auto& Result : Work->Results) {
					if (!Result) { Preparation.Result = {EObjectCookError::Cancelled, {}}; return true; }
					Programs.push_back(&Result.Resource);
				}
				FByteBuffer Bytes;
				const auto Encoded = EncodeMaterialCookedProgramFamily(Programs, Properties, Platform, ECookTargetProfile::Game, Bytes);
				if (!Encoded) { Preparation.Result = {EObjectCookError::Preparation, Encoded.error()}; return true; }
				auto Bulk = FBulkData::TryCreateDetached(Bytes);
				if (!Bulk) { Preparation.Result = {EObjectCookError::Preparation, FormatBulkDataError(Bulk.error())}; return true; }
				Preparation.Primary = std::move(*Bulk);
				return true;
			};
			for (size_t Index = 0; Index < Inputs.size(); ++Index) {
				auto Submitted = MaterialBuildPrivate::FMaterialCompilation::Compile(Inputs[Index],
					{.IsCancellationRequested = [Weak = std::weak_ptr(Work)] { auto Owner = Weak.lock(); return !Owner || Owner->Cancellation.IsCanceled(); }},
					[Work, Index](MaterialBuildPrivate::FMaterialBuildResult Build) {
						const auto& Result = Build.Output;
						std::lock_guard Lock(Work->Mutex);
						if (!Result && Result.Status != EMaterialProgramStatus::Canceled && !Work->Failed.load()) {
							Work->Failure = FormatMaterialProgramDiagnostics(Result.Diagnostics, "Material Cook compilation failed.");
							Work->Failed.store(true);
						}
						Work->Results[Index] = std::move(Build.Output);
					});
				if (!Submitted) {
					{ std::lock_guard Lock(Work->Mutex); Work->Failure = "Material Cook compile admission failed."; Work->Failed.store(true); }
					Work->Cancel(); break;
				}
				Work->Compiles.push_back(std::move(Submitted));
			}
		}
#endif
	}

	auto DMaterialInterface::BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform) -> void
	{
		Super::BeginCacheForCookedPlatformData(TargetPlatform);
		FCookedPlatformData::Begin(*this, TargetPlatform, [this](FCookedPlatformData& Context) {
			if (IsDynamicInstance()) return;
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) {
				const FAssetBuildTaskContext Control{.IsCancellationRequested = [&Context] { return Context.CheckCancellation(); }};
				Context.Result = PrepareCookedProgram(Context.GetTargetPlatform(), Context.Options.MaterialConfigurations, Context.Primary, Control);
				return;
			}
#if DURIN_WITH_EDITOR
			BeginMaterialCookPreparation(*this, Context);
#else
			Context.Result = {EObjectCookError::Preparation, "Authored material Cook preparation is unavailable in this runtime."};
#endif
		});
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

	auto DMaterialInterface::ValidatePreparedMaterialProgram(const FMaterialStaticProperties& PayloadProperties,
		std::shared_ptr<const FMaterialProgram> ProgramCandidate) -> std::expected<std::shared_ptr<const FMaterialProgram>, std::string>
	{
		if (!ProgramCandidate || !ValidateMaterialProgram(*ProgramCandidate))
			return std::unexpected("payload material program contract is invalid.");
		FResolvedMaterialProperties Resolved;
		if (!ResolveMaterialProperties(*this, Resolved))
			return std::unexpected("payload material owner is unavailable.");
		const auto* Root = Cast<DMaterial>(ResolveObjectKey(Resolved.Root));
		if (!Root || Root->GetDomain() != ProgramCandidate->GetShaderMap().Domain)
			return std::unexpected("payload domain does not match package metadata.");
		const auto ExpectedProperties = CanonicalizeMaterialShaderProperties(GetStaticProperties());
		if (PayloadProperties != ExpectedProperties)
		{
			return std::unexpected("payload static properties do not match package metadata.");
		}
		for (const auto& Parameter : ProgramCandidate->GetShaderMap().ActiveParameters)
		{
			const auto* Definition = FindParameterDefinition(Parameter.Id);
			if (!Definition || !Parameter.MatchesValue(Definition->Value))
			{
				return std::unexpected("payload parameter contract does not match package metadata.");
			}
		}
		if (!ProgramCandidate->ActiveCollections.empty())
		{
			auto ResolvedProgram = std::make_shared<FMaterialProgram>(
				*ProgramCandidate);
			for (auto& Required : ResolvedProgram->ActiveCollections)
			{
				// Empty paths exist only for detached/transient compiler fixtures; real
				// package-authored collection expressions always capture an exact path.
				if (Required.AssetPath.empty()) continue;
				FSoftObjectPath Path;
				if (!FSoftObjectPath::TryCreate(Required.AssetPath, Path))
					return std::unexpected("payload collection asset is missing or its schema does not match the cooked material.");
				auto* Collection = LoadObject<DMaterialParameterCollection>(Path);
				if (!Collection)
					return std::unexpected("payload collection asset is missing or its schema does not match the cooked material.");
				auto Current = Collection->BuildLayout();
				if (!Current || !Required.HasCompatibleSchema(*Current))
					return std::unexpected("payload collection asset is missing or its schema does not match the cooked material.");
				Required.DefaultPayload = std::move(Current->DefaultPayload);
			}
			ProgramCandidate = std::move(ResolvedProgram);
		}

		return ProgramCandidate;
	}

	auto DMaterialInterface::AdmitCookedProgram(const FMaterialStaticProperties& PayloadProperties,
		std::shared_ptr<const FMaterialProgram> ProgramCandidate,
		std::shared_ptr<FMaterialResource> Resource) -> std::expected<void, std::string>
	{
		if (!ProgramCandidate) return std::unexpected("payload material program is unavailable.");
		if (!Resource)
		{
			Resource = GetCompilationResource(ProgramCandidate->FeatureLevel, ProgramCandidate->Quality, true);
			CompilationState.SelectedResource = Resource;
			Resource->RequestedInput = CaptureCompilationInputStamp();
		}
		const auto Validated = ValidatePreparedMaterialProgram(PayloadProperties, std::move(ProgramCandidate));
		if (!Validated) return std::unexpected(Validated.error());
		ProgramCandidate = *Validated;
		Resource->CompiledProgram = std::move(ProgramCandidate);
		Resource->AcceptedProperties = PayloadProperties;
		if (Resource == CompilationState.SelectedResource) Private::GetMaterialEditCompileQueue().Remove(FWeakObjectPtr(this));
		Resource->CookedPreparation.reset();
		Resource->Outcome = EMaterialCompileState::Ready;
		Resource->CacheInfo = {};
		if (!Resource->RequestId) Resource->RequestId = ++CompilationState.NextRequestId;
		Resource->CompiledDependencyRevision = Resource->RequestedInput.ShaderRevision;
		Resource->CompiledRevision = Resource->RequestedInput.ContentRevision;
		if (Resource == CompilationState.SelectedResource) CompilationState.SubmittedRevision = CompilationState.AuthoredRevision;
		if (Resource == CompilationState.SelectedResource) CompilationState.EditCompileDeadline.reset();

		Resource->Diagnostics.clear();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState);
		return {};
	}

	auto DMaterialInterface::PrepareCookedProgram(ECookTargetPlatform TargetPlatform,
		std::span<const FMaterialCookConfiguration> Requested, FBulkData& OutData, const FAssetBuildTaskContext& Control)
		-> FObjectCookResult
	{
		OutData = {};
		if (Control.IsCancelled()) return {EObjectCookError::Cancelled, {}};
		if (IsDynamicInstance()) return {};
		const auto Configurations = ResolveMaterialCookConfigurations(Requested);
		if (!Configurations) return {EObjectCookError::Preparation, "Material Cook configuration is unsupported."};
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (GetAssetRuntimeConfiguration().GetCookTargetPlatform() != TargetPlatform || !CookedProgramData.HasData())
				return {EObjectCookError::Preparation, "Material cooked program data is unavailable or targets another platform."};
			auto Read = CookedProgramData.AcquireRead();
			if (!Read) return {EObjectCookError::Preparation, "Material cooked program data could not be read."};
			std::vector<std::shared_ptr<const FMaterialProgram>> Variants;
			FMaterialStaticProperties Properties;
			for (const auto& Configuration : *Configurations)
			{
				if (Control.IsCancelled()) return {EObjectCookError::Cancelled, {}};
				std::shared_ptr<const FMaterialProgram> Program;
				const auto Decoded = DecodeMaterialCookedProgramFamily(Read->GetBytes(), TargetPlatform,
					ECookTargetProfile::Game, Configuration.Quality, Configuration.FeatureLevel, {}, Properties, Program);
				if (!Decoded) return {EObjectCookError::Preparation, Decoded.error()};
				Variants.push_back(std::move(Program));
			}
			std::vector<const FMaterialProgram*> Programs;
			for (const auto& Variant : Variants) Programs.push_back(Variant.get());
			FByteBuffer Bytes;
			const auto Encoded = EncodeMaterialCookedProgramFamily(Programs, Properties,
				TargetPlatform, ECookTargetProfile::Game, Bytes);
			if (!Encoded) return {EObjectCookError::Preparation, Encoded.error()};
			auto Bulk = FBulkData::TryCreateDetached(Bytes);
			if (!Bulk) return {EObjectCookError::Preparation, FormatBulkDataError(Bulk.error())};
			if (Control.IsCancelled()) return {EObjectCookError::Cancelled, {}};
			OutData = std::move(*Bulk);
			return {};
		}
#if DURIN_WITH_EDITOR
		FCookPackagePreparationOptions Options{.TargetPlatform = TargetPlatform, .TargetProfile = ECookTargetProfile::Game};
		Options.MaterialConfigurations.assign(Configurations->begin(), Configurations->end());
		Options.IsCancelled = [&Control] { return Control.IsCancelled(); };
		FCookedPlatformData Preparation(Options);
		Preparation.TryReserveWorkingSet = [&](uint64 Bytes) {
			if (Bytes > Control.MaximumWorkingSetBytes) {
				Preparation.Result = {EObjectCookError::Limit, "Material Cook preparation exceeds its working-set limit."};
				return false;
			}
			Preparation.WorkingSetBytes = Bytes;
			return true;
		};
		BeginMaterialCookPreparation(*this, Preparation);
		Preparation.Wait();
		if (Preparation.CheckCancellation()) return {EObjectCookError::Cancelled, {}};
		if (!Preparation.Result) return Preparation.Result;
		if (Preparation.ValidatePreparedInputs && !Preparation.ValidatePreparedInputs())
			return {EObjectCookError::Preparation, "Material Cook prepared inputs changed."};
		OutData = std::move(Preparation.Primary);
		return {};
#else
		return {EObjectCookError::Preparation, "Authored material Cook preparation is unavailable in this runtime."};
#endif
	}

	auto DMaterialInterface::SerializeCooked(FArchive& Ar) -> void
	{
		auto* Prepared = Ar.IsSaving() && !IsDynamicInstance()
			? FCookedPlatformData::Find(*this, Ar.GetTarget()) : nullptr;
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

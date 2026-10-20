#include "Materials/MaterialShaderMap.h"
#include "MaterialPreparedProgram.h"
#include "Shader/ShaderCompilingManager.h"
#include "Materials/MaterialCompileLifecycle.h"
#if DURIN_WITH_EDITOR
#include "MaterialTranslation.h"
#endif
namespace Durin
{
	FMaterialShaderMap::FMaterialShaderMap() = default;
	FMaterialShaderMap::~FMaterialShaderMap() = default;
	FMaterialShaderMap::FMaterialShaderMap(const FMaterialShaderMap& Other)
		: ActiveParameters(Other.ActiveParameters), Layout(Other.Layout), Requirements(Other.Requirements),
		  Dependencies(Other.Dependencies), CompiledShaders(Other.CompiledShaders), ShaderMapId(Other.ShaderMapId),
          CompilerIdentity(Other.CompilerIdentity), Target(Other.Target), PassContractVersion(Other.PassContractVersion),
          RequiredStages(Other.RequiredStages), CollectionLayouts(Other.CollectionLayouts), DebugData(Other.DebugData),
          CompilationTimings(Other.CompilationTimings) {}
	auto FMaterialCompilerResult::GetShaderMap() const -> const FMaterialShaderMap&
	{ static const FMaterialShaderMap Empty; return ShaderMap ? *ShaderMap : Empty; }
	auto FMaterialCompilerResult::GetDebugData() const -> const FMaterialProgramDebugData&
	{ static const FMaterialProgramDebugData Empty; const auto& Debug = GetShaderMap().DebugData; return Debug ? *Debug : Empty; }
	auto FMaterialShaderMap::GetShaderMapId() const -> FMaterialProgramIdentity
	{
#if DURIN_WITH_EDITOR
		std::lock_guard Lock(Mutex);
#endif
		return ShaderMapId;
	}
	auto FMaterialShaderMap::SetCompilationOutput(const FMaterialCompilerResult& Result) -> void
	{
#if DURIN_WITH_EDITOR
		std::lock_guard Lock(Mutex);
#endif
		CompilationTimings = Result.Timings;
		CollectionLayouts = Result.ActiveCollections;
		for (auto& Schema : CollectionLayouts) {
			Schema.AssetPath.clear(); Schema.AssetPath.shrink_to_fit();
			Schema.DefaultPayload.clear(); Schema.DefaultPayload.shrink_to_fit();
		}
	}

	namespace
	{
		std::mutex CacheMutex;
		std::unordered_map<FMaterialProgramIdentity, std::shared_ptr<const FMaterialShaderMap>> Programs;
		std::deque<FMaterialProgramIdentity> Order;
		uint64 Bytes = 0;
	}
	namespace
	{
		auto DebugBytes(const std::shared_ptr<const FMaterialProgramDebugData>& Debug) -> uint64
		{
			if (!Debug) return 0;
			uint64 Bytes = sizeof(*Debug) + Debug->GeneratedSource.capacity() + Debug->IR.Nodes.capacity() * sizeof(MIR::FNode);
			for (const auto& Node : Debug->IR.Nodes) Bytes += Node.Inputs.capacity() * sizeof(uint32);
			return Bytes;
		}
		auto CollectionBytes(std::span<const FMaterialParameterCollectionLayout> Collections) -> uint64
		{
			uint64 Bytes = 0;
			for (const auto& Collection : Collections) Bytes += sizeof(Collection) + Collection.AssetPath.capacity()
				+ Collection.DefaultPayload.capacity() + Collection.UniformLayout.Fields.capacity() * sizeof(FMaterialRenderField);
			return Bytes;
		}
	}
	namespace
	{
		std::mutex CompletionHookMutex;
		std::function<void(EMaterialQualityLevel, bool)> CompletionHook;
	}
	auto MaterialBuildPrivate::SetMaterialBuildCompletionHookForTests(std::function<void(EMaterialQualityLevel, bool)> Hook) -> void
	{ std::lock_guard Lock(CompletionHookMutex); CompletionHook = std::move(Hook); }
	auto FMaterialShaderMap::GetRetainedBytes() const -> uint64
	{
		uint64 Size = sizeof(*this) + CompilerIdentity.capacity() + Target.capacity() + DebugBytes(DebugData)
			+ CollectionBytes(CollectionLayouts) + ActiveParameters.capacity() * sizeof(FMaterialCompilerParameterDeclaration)
			+ Layout.Fields.capacity() * sizeof(FMaterialRenderField) + CompiledShaders.capacity() * sizeof(FShaderCode);
		for (const auto& Dependency : Dependencies) Size += sizeof(Dependency) + Dependency.VirtualPath.capacity();
		for (const auto& Shader : CompiledShaders) {
			Size += Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity() + Shader.Code.GetRetainedCapacityBytes();
			for (const auto& Binding : Shader.Reflection.ResourceBindings) Size += sizeof(Binding) + Binding.Name.capacity();
			Size += Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange) + Shader.PlatformData.GetRetainedCapacityBytes();
		}
		return Size;
	}
	auto FMaterialShaderMap::GetRetainedBytes(const FMaterialCompilerResult& Result) -> uint64
	{
		// Charge shared backing storage in full for each independently retained result.
		return sizeof(Result) + (Result.ShaderMap ? Result.ShaderMap->GetRetainedBytes() : 0)
			+ Result.StaticBools.capacity() * sizeof(FMaterialCompilerEnvironment::FStaticBoolValue)
			+ CollectionBytes(Result.ActiveCollections);
	}
	auto FMaterialShaderMap::FindId(FMaterialProgramIdentity Identity) -> std::shared_ptr<const FMaterialShaderMap>
	{ std::lock_guard Lock(CacheMutex); const auto Found = Programs.find(Identity); return Found == Programs.end() ? nullptr : Found->second; }

	auto FMaterialShaderMap::BindCollections(std::shared_ptr<const FMaterialCompilerResult> Program,
		std::span<const FMaterialParameterCollectionLayout> Collections, const FMaterialCompilerEnvironment* Environment) -> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!Program) return {};
		bool Changed = Environment && (Program->Quality != Environment->Quality || Program->FeatureLevel != Environment->FeatureLevel || Program->StaticBools != Environment->StaticBools);
		for (const auto& Collection : Program->ActiveCollections)
		{
			const auto Found = std::ranges::find(Collections, Collection.CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
			if (Found == Collections.end() || !Collection.HasCompatibleSchema(*Found)) return {};
			Changed |= Collection.DefaultPayload != Found->DefaultPayload || (Environment && Collection.AssetPath != Found->AssetPath);
		}
		if (!Changed) return Program;
		auto Result = std::make_shared<FMaterialCompilerResult>(*Program);
		if (Environment) { Result->Quality = Environment->Quality; Result->FeatureLevel = Environment->FeatureLevel; Result->StaticBools = Environment->StaticBools; }
		for (auto& Collection : Result->ActiveCollections) Collection = *std::ranges::find(Collections, Collection.CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
		return Result;
	}
	auto FMaterialShaderMap::Register(std::shared_ptr<const FMaterialShaderMap> ShaderMap, bool Replace)
		-> std::shared_ptr<const FMaterialShaderMap>
	{
		if (!ShaderMap || ShaderMap->CompiledShaders.empty()) return ShaderMap;
		const auto Size = ShaderMap->GetRetainedBytes(); if (Size > 8ull * 1024 * 1024) return ShaderMap;
		std::lock_guard Lock(CacheMutex);
		const auto Found = Programs.find(ShaderMap->ShaderMapId);
		if (Found != Programs.end()) {
			if (!Replace) return Found->second;
			Bytes -= Found->second->GetRetainedBytes(); Programs.erase(Found); std::erase(Order, ShaderMap->ShaderMapId);
		}
		while (!Order.empty() && (Programs.size() >= 128 || Bytes + Size > 256ull * 1024 * 1024)) {
			const auto Old = Order.front(); Order.pop_front(); Bytes -= Programs.at(Old)->GetRetainedBytes(); Programs.erase(Old);
		}
		Bytes += Size; Programs.emplace(ShaderMap->ShaderMapId, ShaderMap); Order.push_back(ShaderMap->ShaderMapId); return ShaderMap;
	}

	auto FMaterialShaderMap::ClearCache() -> void { std::lock_guard Lock(CacheMutex); Programs.clear(); Order.clear(); Bytes = 0; }
	auto FMaterialShaderMap::GetCacheStats() -> std::pair<uint32, uint64> { std::lock_guard Lock(CacheMutex); return {uint32(Programs.size()), Bytes}; }
#if DURIN_WITH_EDITOR
	auto FMaterialShaderMap::Compile(std::shared_ptr<FMaterialShaderMap> ShaderMap) -> void
	{
		FMaterialError ValidationError;
		auto Admitted = MaterialBuildPrivate::SubmitTranslation(ShaderMap->Input, ShaderMap->Force, *ShaderMap->Owner,
			[ShaderMap](FMaterialTranslatedProgram Product) {
				try { ProcessTranslation(ShaderMap, std::move(Product)); }
				catch (...) { CompleteCompilation(ShaderMap, {.Diagnostics = {{
					.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::AdmissionRejected}}}); }
			}, ShaderMap->BuildCancellation, true, ShaderMap->EncodedInput,
			ShaderMap->Inline ? DerivedData::EBuildCacheExecution::Inline : DerivedData::EBuildCacheExecution::Task, &ValidationError);
		if (!Admitted) CompleteCompilation(ShaderMap, {.Diagnostics = {{
			.Category = ValidationError.HasError() ? EMaterialProgramDiagnosticCategory::Normalization : EMaterialProgramDiagnosticCategory::Compile,
			.Error = ValidationError.HasError() ? std::move(ValidationError)
				: FMaterialError::FromExternal(EMaterialCompileError::AdmissionRejected, Admitted.error().Description)}}});
	}
	auto FMaterialShaderMap::ProcessTranslation(std::shared_ptr<FMaterialShaderMap> ShaderMap, FMaterialTranslatedProgram Translation) -> void
	{
		if (FShaderCompilingManager::IsBuildCanceled(ShaderMap)) return CompleteCompilation(ShaderMap, FMaterialCompilerResult{.Status = EMaterialProgramStatus::Canceled});
		std::shared_ptr<const FMaterialCompilerResult> Retained;
		for (const auto& Location : Translation.Sources) { ShaderMap->Sources.push_back(Location.Source); ShaderMap->SourceOrdinals.push_back(Location.Ordinal); }
		if (!Translation) return CompleteCompilation(ShaderMap, FMaterialShaderMap::FinalizeContent(std::move(Translation), {}, ShaderMap));
		{ std::lock_guard Lock(ShaderMap->Mutex); ShaderMap->ShaderMapId = Translation.Identity; }
		Retained = FShaderCompilingManager::Get().FindOrAddCompilingShaderMap(ShaderMap);
		if (Retained) return CompleteCompilation(ShaderMap, {}, std::move(Retained));
		if (ShaderMap->SingleFlight) return;
		ShaderMap->Translation = std::make_unique<FMaterialTranslatedProgram>(std::move(Translation));
		ShaderMap->CompilationBegin = std::chrono::steady_clock::now();
		FShaderCompilingManager::SubmitCompileJobs(ShaderMap);
	}

	auto FMaterialShaderMap::CompleteCompilation(std::shared_ptr<FMaterialShaderMap> ShaderMap, FMaterialCompilerResult Result,
		std::shared_ptr<const FMaterialCompilerResult> Retained) -> void
	{
		DerivedData::FRequestBarrier DeliveryBarrier(*ShaderMap->Owner);
		std::function<void(MaterialBuildPrivate::FMaterialBuildResult)> Callback;
		std::vector<std::shared_ptr<FMaterialShaderMap>> Subscribers;
		// Cache hits and shared subscribers bind once, before byte-limit checks.
		if (Retained) Retained = BindCollections(std::move(Retained), ShaderMap->Input->Collections, &ShaderMap->Input->Environment);
		if ((Result && FMaterialShaderMap::GetRetainedBytes(Result) > MaterialCompileMaxResultBytes)
			|| (Retained && *Retained && FMaterialShaderMap::GetRetainedBytes(*Retained) > MaterialCompileMaxResultBytes)) {
			Retained.reset();
			Result = {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds,
				.Error = EMaterialCompileError::CompiledMaterialResultExceedsRetainedResultByteLimit}}};
		}
		const auto Quality = Retained ? Retained->Quality : Result.Quality;
		const bool RetainedHit = bool(Retained) && !ShaderMap->SingleFlight;
		auto Program = Retained ? std::move(Retained) : std::make_shared<const FMaterialCompilerResult>(std::move(Result));
		auto Build = FShaderCompilingManager::Get().FinalizeCompilation(
			ShaderMap, std::move(Program), RetainedHit, Subscribers, Callback);
		if (!Build.Program || !Callback) return;
		std::function<void(EMaterialQualityLevel, bool)> Hook;
		{ std::lock_guard Lock(CompletionHookMutex); Hook = CompletionHook; }
		for (const auto& Subscriber : Subscribers) {
			auto Program = Build.Program;
			if (Program && !*Program) {
				auto Qualified = std::make_shared<FMaterialCompilerResult>(*Program);
				for (auto& Diagnostic : Qualified->Diagnostics) {
					const auto Origin = std::ranges::find(ShaderMap->Sources, Diagnostic.NodeId, &MIR::FSource::NodeId);
					if (Origin == ShaderMap->Sources.end()) continue;
					const auto Location = std::ranges::find(Subscriber->Sources, Origin->ExpressionIndex, &MIR::FSource::ExpressionIndex);
					if (Location == Subscriber->Sources.end()) continue;
					const auto& Source = *Location;
					Diagnostic.NodeId = Source.NodeId; Diagnostic.PortId = Source.PortId;
					Diagnostic.FunctionAssetPath = Source.FunctionAssetPath; Diagnostic.CallPath = Source.CallPath;
				}
				Program = std::move(Qualified);
			}
			// A subscriber callback cannot prevent another subscriber from retiring.
			try { FShaderCompilingManager::LaunchTask(Subscriber, [Subscriber, Program = std::move(Program)] { CompleteCompilation(Subscriber, {}, Program); }); } catch (...) {}
		}
		if (Hook) Hook(Quality, true);
		if (ShaderMap->Cancellation.IsCancelled()) { Build.Program = std::make_shared<const FMaterialCompilerResult>(FMaterialCompilerResult{.Status = EMaterialProgramStatus::Canceled}); Build.RetainedHit = false; }
		Build.Sources = std::move(ShaderMap->Sources);
		Build.SourceOrdinals = std::move(ShaderMap->SourceOrdinals);
		Callback(std::move(Build));
		if (Hook) Hook(Quality, false);
	}

#endif

	auto FMaterialShaderMap::FinalizeContent(FMaterialTranslatedProgram Translation,
		FShaderBuildResult Output, std::shared_ptr<FMaterialShaderMap> Artifact) -> FMaterialCompilerResult
	{
		if (!Artifact) Artifact = std::make_shared<FMaterialShaderMap>();
		Artifact->ActiveParameters = std::move(Translation.ActiveParameters);
		Artifact->Layout = std::move(Translation.Layout);
		Artifact->Requirements = Translation.Requirements;
		Artifact->Dependencies = std::move(Translation.Dependencies);
		{
#if DURIN_WITH_EDITOR
			std::lock_guard Lock(Artifact->Mutex);
#endif
			Artifact->ShaderMapId = Translation.Identity;
		}
		Artifact->CompilerIdentity = std::move(Translation.CompilerIdentity);
		Artifact->Target = std::move(Translation.Target);
		Artifact->PassContractVersion = Translation.PassContractVersion;
		Artifact->RequiredStages = Translation.RequiredStages;
		Artifact->DebugData = std::make_shared<const FMaterialProgramDebugData>(FMaterialProgramDebugData{
			.IR = std::move(Translation.IR), .GeneratedSource = std::move(Translation.GeneratedSource)});
		FMaterialCompilerResult Result{
			.Quality = Translation.Quality, .FeatureLevel = Translation.FeatureLevel,
			.StaticBools = std::move(Translation.StaticBools), .ActiveCollections = std::move(Translation.ActiveCollections),
			.ShaderMap = Artifact,
			.Timings = Translation.Timings, .Diagnostics = std::move(Translation.Diagnostics)};
		if (Translation.Status == EMaterialProgramStatus::Canceled || Output.Status == EShaderBuildStatus::Canceled)
			return {.Status = EMaterialProgramStatus::Canceled};
		if (!Translation) return Result;
		if (Output.Status == EShaderBuildStatus::OutputTooLarge)
			return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds,
				.Error = EMaterialCompileError::CompiledMaterialResultExceedsRetainedResultByteLimit}}};
		if (!Output)
		{
			FMaterialError Error{EMaterialCompileError::ShaderCompilerFailed};
			Error.ShaderCause = std::make_shared<const FShaderError>(std::move(Output.Error));
			Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = std::move(Error)});
			return Result;
		}
		const auto Target = Artifact->Target == "metal-library-1" ? MetalLibraryShaderTarget
			: Artifact->Target == "metal-msl-2.0" ? MetalShaderTarget : VulkanShaderTarget;
		const auto Reflection = std::ranges::all_of(Output.CompiledShaders,
			[&](const auto& Shader) { return Shader.Target == Target; })
			? ValidateMaterialCompiledStages(Output.CompiledShaders,
				Artifact->Layout, Translation.ResourceLimits, Result.ActiveCollections, Artifact->RequiredStages)
			: FMaterialLayoutValidationResult{.Error = EMaterialLayoutError::InvalidReflection};
		if (!Reflection)
		{
			Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Reflection, .Error = FMaterialError(Reflection)});
			return Result;
		}
		Artifact->CompiledShaders = std::move(Output.CompiledShaders);
		Artifact->SetCompilationOutput(Result);
		Result.Status = EMaterialProgramStatus::Ok;
		return Result;
	}

}

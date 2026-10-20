#include "Materials/MaterialShaderMap.h"
#include "MaterialPreparedProgram.h"
#include "MaterialProgramCompilingManager.h"
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
		  Dependencies(Other.Dependencies), CompiledShaders(Other.CompiledShaders), ShaderMapId(Other.ShaderMapId) {}
	auto FMaterialCompilerResult::GetShaderMap() const -> const FMaterialShaderMap&
	{ static const FMaterialShaderMap Empty; return ShaderMap ? *ShaderMap : Empty; }

	namespace
	{
		std::mutex CacheMutex;
		struct FProgramEntry { std::shared_ptr<const FMaterialCompilerResult> Program; uint64 Bytes = 0; };
		std::unordered_map<FMaterialProgramIdentity, FProgramEntry> Programs;
		std::deque<FMaterialProgramIdentity> Order;
		uint64 Bytes = 0;
	}
	auto FMaterialShaderMap::GetRetainedBytes(const FMaterialCompilerResult& Result) -> uint64
	{
		const auto& Artifact = Result.GetShaderMap();
		const auto& DebugData = Result.GetDebugData();
		// Charge the full transitive capacity even when another program also shares it.
		uint64 Size = sizeof(Result) + (Result.ShaderMap ? sizeof(FMaterialShaderMap) : 0)
			+ (Result.DebugData ? sizeof(FMaterialProgramDebugData) : 0)
			+ Result.StaticBools.capacity() * sizeof(FMaterialCompilerEnvironment::FStaticBoolValue)
			+ DebugData.GeneratedSource.capacity() + Result.CompilerIdentity.capacity() + Result.Target.capacity()
			+ DebugData.IR.Nodes.capacity() * sizeof(MIR::FNode) + Artifact.ActiveParameters.capacity() * sizeof(FMaterialCompilerParameterDeclaration)
			+ Artifact.Layout.Fields.capacity() * sizeof(FMaterialRenderField) + Artifact.CompiledShaders.capacity() * sizeof(FShaderCode);
		for (const auto& Node : DebugData.IR.Nodes) Size += Node.Inputs.capacity() * sizeof(uint32);
		for (const auto& Dependency : Artifact.Dependencies) Size += sizeof(Dependency) + Dependency.VirtualPath.capacity();
		for (const auto& Collection : Result.ActiveCollections) Size += sizeof(Collection) + Collection.AssetPath.capacity() + Collection.DefaultPayload.capacity() + Collection.UniformLayout.Fields.capacity() * sizeof(FMaterialRenderField);
		for (const auto& Shader : Artifact.CompiledShaders)
		{
			Size += Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity() + Shader.Code.GetRetainedCapacityBytes();
			for (const auto& Binding : Shader.Reflection.ResourceBindings) Size += sizeof(Binding) + Binding.Name.capacity();
			Size += Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange) + Shader.PlatformData.GetRetainedCapacityBytes();
		}
		return Size;
	}
	auto FMaterialShaderMap::FindId(FMaterialProgramIdentity Identity) -> std::shared_ptr<const FMaterialCompilerResult>
	{ std::lock_guard Lock(CacheMutex); const auto Found = Programs.find(Identity); return Found == Programs.end() ? nullptr : Found->second.Program; }
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
	auto FMaterialShaderMap::Configure(std::shared_ptr<const FMaterialCompilerResult> Program, const MIR::FCompilerInput& Input) -> std::shared_ptr<const FMaterialCompilerResult>
	{ return FMaterialShaderMap::BindCollections(std::move(Program), Input.Collections, &Input.Environment); }

	auto FMaterialShaderMap::Register(std::shared_ptr<const FMaterialCompilerResult> Program, bool Replace) -> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!Program || !*Program) return Program;
		const auto Size = FMaterialShaderMap::GetRetainedBytes(*Program); if (Size > 8ull * 1024 * 1024) return Program;
		std::lock_guard Lock(CacheMutex);
		const auto Found = Programs.find(Program->Identity);
		if (Found != Programs.end())
		{
			if (!Replace) return Found->second.Program;
			Bytes -= Found->second.Bytes; Programs.erase(Found); std::erase(Order, Program->Identity);
		}
		while (!Order.empty() && (Programs.size() >= 128 || Bytes + Size > 256ull * 1024 * 1024))
		{ auto Old = Order.front(); Order.pop_front(); Bytes -= Programs.at(Old).Bytes; Programs.erase(Old); }
		Bytes += Size; Programs.emplace(Program->Identity, FProgramEntry{Program, Size}); Order.push_back(Program->Identity); return Program;
	}
	auto FMaterialShaderMap::ClearCache() -> void { std::lock_guard Lock(CacheMutex); Programs.clear(); Order.clear(); Bytes = 0; }
	auto FMaterialShaderMap::GetCacheStats() -> std::pair<uint32, uint64> { std::lock_guard Lock(CacheMutex); return {uint32(Programs.size()), Bytes}; }
#if DURIN_WITH_EDITOR
	auto FMaterialShaderMap::Compile(std::shared_ptr<FMaterialShaderMap> ShaderMap) -> void
	{
		using FManager = MaterialBuildPrivate::FMaterialProgramCompilingManager;
		FMaterialError ValidationError;
		auto Admitted = MaterialBuildPrivate::SubmitTranslation(ShaderMap->Input, ShaderMap->Force, *ShaderMap->Owner,
			[ShaderMap](FMaterialTranslatedProgram Product) {
				try { FManager::ProcessTranslation(ShaderMap, std::move(Product)); }
				catch (...) { FManager::CompleteCompilation(ShaderMap, {.Diagnostics = {{
					.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::AdmissionRejected}}}); }
			}, ShaderMap->BuildCancellation, true, ShaderMap->EncodedInput,
			ShaderMap->Inline ? DerivedData::EBuildCacheExecution::Inline : DerivedData::EBuildCacheExecution::Task, &ValidationError);
		if (!Admitted) FManager::CompleteCompilation(ShaderMap, {.Diagnostics = {{
			.Category = ValidationError.HasError() ? EMaterialProgramDiagnosticCategory::Normalization : EMaterialProgramDiagnosticCategory::Compile,
			.Error = ValidationError.HasError() ? std::move(ValidationError)
				: FMaterialError::FromExternal(EMaterialCompileError::AdmissionRejected, Admitted.error().Description)}}});
	}
	auto FMaterialShaderMap::ProcessCompilationResults(size_t Index, FShaderBuildResult Output) -> void
	{
		{
			std::lock_guard Lock(Mutex);
			ReservedOutputBytes -= Jobs[Index].MaximumOutputBytes;
			if (Output) {
				auto StageArtifact = std::make_shared<FMaterialShaderMap>();
				StageArtifact->CompiledShaders = std::move(Output.CompiledShaders);
				const auto Bytes = FMaterialShaderMap::GetRetainedBytes(FMaterialCompilerResult{.ShaderMap = StageArtifact});
				if (Bytes > MaterialCompileMaxResultBytes - RetainedStageBytes) {
					ResultTooLarge = true;
					Output = FShaderBuildResult::Failure("Compiled material result exceeds retained result byte limit.");
				}
				else { RetainedStageBytes += Bytes; Output.CompiledShaders = std::move(StageArtifact->CompiledShaders); }
			}
			Outputs[Index] = std::move(Output); --ActiveJobs;
		}
	}
	auto FMaterialShaderMap::FinalizeCompilation(std::shared_ptr<FMaterialShaderMap> Self) -> FMaterialCompilerResult
	{
			if (ResultTooLarge) return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds,
				.Error = EMaterialCompileError::CompiledMaterialResultExceedsRetainedResultByteLimit}}};
			FShaderBuildResult Output{.Status = EShaderBuildStatus::Ok};
			for (auto& Slot : Outputs) {
				if (!Slot) continue;
				if (!*Slot) { Output = std::move(*Slot); break; }
				for (auto& Shader : Slot->CompiledShaders) Output.CompiledShaders.push_back(std::move(Shader));
			}
			return FinalizeContent(std::move(*Translation), std::move(Output), std::move(Self));
	}
#endif

	auto FMaterialShaderMap::FinalizeContent(FMaterialTranslatedProgram Translation,
		FShaderBuildResult Output, std::shared_ptr<FMaterialShaderMap> Artifact) -> FMaterialCompilerResult
	{
		if (!Artifact) Artifact = std::make_shared<FMaterialShaderMap>();
		Artifact->ShaderMapId = Translation.Identity;
		Artifact->ActiveParameters = std::move(Translation.ActiveParameters);
		Artifact->Layout = std::move(Translation.Layout);
		Artifact->Requirements = Translation.Requirements;
		Artifact->Dependencies = std::move(Translation.Dependencies);
		FMaterialCompilerResult Result{
			.Identity = Translation.Identity, .CompilerIdentity = std::move(Translation.CompilerIdentity), .Target = std::move(Translation.Target),
			.PassContractVersion = Translation.PassContractVersion, .RequiredStages = Translation.RequiredStages, .Quality = Translation.Quality, .FeatureLevel = Translation.FeatureLevel,
			.StaticBools = std::move(Translation.StaticBools), .ActiveCollections = std::move(Translation.ActiveCollections),
			.ShaderMap = Artifact,
			.DebugData = std::make_shared<const FMaterialProgramDebugData>(FMaterialProgramDebugData{
				.IR = std::move(Translation.IR), .GeneratedSource = std::move(Translation.GeneratedSource)}),
			.Timings = Translation.Timings, .Diagnostics = std::move(Translation.Diagnostics)};
		if (Translation.Status == EMaterialProgramStatus::Canceled || Output.Status == EShaderBuildStatus::Canceled)
			return {.Status = EMaterialProgramStatus::Canceled};
		if (!Translation) return Result;
		if (!Output)
		{
			Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Compile,
				.Error = FMaterialError::FromExternal(EMaterialCompileError::ShaderCompilerFailed, Output.Diagnostic)});
			return Result;
		}
		const auto Target = Result.Target == "metal-library-1" ? MetalLibraryShaderTarget
			: Result.Target == "metal-msl-2.0" ? MetalShaderTarget : VulkanShaderTarget;
		const auto Reflection = std::ranges::all_of(Output.CompiledShaders,
			[&](const auto& Shader) { return Shader.Target == Target; })
			? ValidateMaterialCompiledStages(Output.CompiledShaders,
				Artifact->Layout, Translation.ResourceLimits, Result.ActiveCollections, Result.RequiredStages)
			: FMaterialLayoutValidationResult{.Error = EMaterialLayoutError::InvalidReflection};
		if (!Reflection)
		{
			Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Reflection, .Error = FMaterialError(Reflection)});
			return Result;
		}
		Artifact->CompiledShaders = std::move(Output.CompiledShaders);
		Result.Status = EMaterialProgramStatus::Ok;
		return Result;
	}

}

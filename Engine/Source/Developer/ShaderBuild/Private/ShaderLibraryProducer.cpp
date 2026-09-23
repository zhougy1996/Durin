#include "ShaderLibraryProducer.h"

#include "Shader/Shader.h"
#include "ShaderBuilder.h"

namespace Durin
{
	namespace
	{
		auto MakeCompileOptions(
			std::span<const FShaderType* const> Types,
			FShaderCompileOptions& OutOptions) -> bool
		{
			OutOptions = {};
			if (Types.empty() || Types.front() == nullptr) return false;
			OutOptions.VirtualShaderPath = Types.front()->GetVirtualShaderPath();
			for (const FShaderType* Type : Types)
			{
				if (Type == nullptr || Type->GetVirtualShaderPath()
					!= OutOptions.VirtualShaderPath) return false;
				OutOptions.EntryPoints.push_back(Type->GetEntryPoint().data());
				OutOptions.Frequencies.push_back(Type->GetFrequency());
				Type->ModifyCompilationEnvironment({Type, Type->GetVirtualShaderPath(), Type->GetEntryPoint(), Type->GetFrequency()}, OutOptions);
			}
			return !OutOptions.VirtualShaderPath.empty();
		}
	}

	auto ProduceCookedShaderLibrary(
		FShaderBuilder& Builder,
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		std::shared_ptr<const FShaderSourceArtifacts> Artifacts,
		const std::function<bool()>& IsCancelled) -> FShaderOperationResult
	{
		OutBytes.clear();
		std::vector<FShaderRuntimeRequest> Inventory;
		if (auto Result = FreezeShaderRuntimeInventory(TargetPlatform, TargetProfile, Inventory); !Result) return Result;
		if (Inventory.empty())
			return std::unexpected(FShaderError{.Code = EShaderError::InventoryEmpty});
		std::vector<FShaderCookedLibraryRecord> Records;
		Records.reserve(Inventory.size());
		for (const FShaderRuntimeRequest& Request : Inventory)
		{
			if (IsCancelled && IsCancelled()) { return std::unexpected(FShaderError{.Code = EShaderError::Cancelled}); }
			std::vector<const FShaderType*> Types;
			if (auto Result = GetShaderRuntimeRequestBuildTypes(Request, Types); !Result)
				return Result;
			FShaderCompileOptions Options;
			if (!MakeCompileOptions(Types, Options))
				return std::unexpected(FShaderError{.Code = EShaderError::RequestBuildTypesMismatch, .ActualIdentity = Request.Name});
			Options.SourceArtifacts = Artifacts;
			FShaderCompilerOutput Output = Builder.GetOrCompile(
				Options.VirtualShaderPath, Options);
			if (!Output)
			{
				auto Error = Output.Error;
				Error.ActualIdentity = Request.Name;
				return std::unexpected(std::move(Error));
			}
			FXxHash128 RuntimeIdentity;
			if (auto Result = BuildShaderRuntimeRequestIdentity(Request, RuntimeIdentity); !Result) return Result;
			FXxHash128Builder Production;
			Production.Update("DurinCookedShaderProduction_v1");
			Production.UpdateValue(RuntimeIdentity);
			Production.Update(Builder.GetCompilerEnvironmentIdentity());
			for (const FCompiledShader& Shader : Output.CompiledShaders)
				Production.UpdateValue(Shader.Hash);
			Records.push_back({Request, Production.Finalize(), std::move(Output)});
		}
		return EncodeShaderCookedLibrary(TargetPlatform, TargetProfile, Records, OutBytes);
	}
}

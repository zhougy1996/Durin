#include "ShaderLibraryProducer.h"

#include "Shader/Shader.h"
#include "ShaderCompileService.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

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
			}
			return !OutOptions.VirtualShaderPath.empty();
		}
	}

	auto ProduceCookedShaderLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		std::vector<FShaderRuntimeRequest> Inventory;
		if (!FreezeShaderRuntimeInventory(
			TargetPlatform, TargetProfile, Inventory, OutError)) return false;
		if (Inventory.empty())
			return Fail(OutError, "Cooked Shader inventory is empty.");
		std::vector<FShaderCookedLibraryRecord> Records;
		Records.reserve(Inventory.size());
		for (const FShaderRuntimeRequest& Request : Inventory)
		{
			std::vector<const FShaderType*> Types;
			if (!GetShaderRuntimeRequestBuildTypes(Request, Types, OutError))
				return false;
			FShaderCompileOptions Options;
			if (!MakeCompileOptions(Types, Options))
				return Fail(OutError, std::format(
					"Cooked Shader request '{}' has incompatible build types.",
					Request.Name));
			FShaderCompilerOutput Output = GetOrCompileShader(
				Options.VirtualShaderPath, Options);
			if (!Output)
				return Fail(OutError, std::format(
					"Cooked Shader request '{}' failed: {}",
					Request.Name, Output.ErrorMessage));
			FXxHash128 RuntimeIdentity;
			if (!BuildShaderRuntimeRequestIdentity(
				Request, RuntimeIdentity, OutError)) return false;
			FXxHash128Builder Production;
			Production.Update("DurinCookedShaderProduction_v1");
			Production.UpdateValue(RuntimeIdentity);
			Production.Update(GetShaderCompilerEnvironmentIdentityFromService());
			for (const FCompiledShader& Shader : Output.CompiledShaders)
				Production.UpdateValue(Shader.Hash);
			Records.push_back({Request, Production.Finalize(), std::move(Output)});
		}
		return EncodeShaderCookedLibrary(
			TargetPlatform, TargetProfile, Records, OutBytes, OutError);
	}
}

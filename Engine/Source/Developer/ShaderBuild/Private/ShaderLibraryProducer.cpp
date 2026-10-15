#include "ShaderLibraryProducer.h"
#include "Hash/CanonicalHash.h"
#include "ShaderBuildVersion.h"
#include "Shader/ShaderCompiledOutput.h"

#include "Shader/Shader.h"
#include "ShaderBuilder.h"
#include "TargetPlatform.h"

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
		const auto Platform = ToCookTargetPlatform(TargetPlatform);
		const auto Profile = TargetProfile == EShaderTargetProfile::Game ? ECookTargetProfile::Game
			: TargetProfile == EShaderTargetProfile::EditorValidation ? ECookTargetProfile::EditorValidation : ECookTargetProfile::Invalid;
		if (!Platform) return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
		const auto Resolved = ResolveTargetPlatform(*Platform);
		if (!Resolved || Profile == ECookTargetProfile::Invalid) return std::unexpected(FShaderError{.Code = EShaderError::InvalidCompileRequest});
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
			Options.Target = Resolved->GetShaderTarget();
			Options.SourceArtifacts = Artifacts;
			FShaderBuildResult Output = Builder.GetOrCompile(
				Options.VirtualShaderPath, Options);
			if (!Output)
			{
				auto Error = Output.Status == EShaderBuildStatus::Canceled ? FShaderError{.Code = EShaderError::Cancelled}
					: FShaderError::FromBuildDiagnostic(Output.Diagnostic);
				Error.ActualIdentity = Request.Name;
				return std::unexpected(std::move(Error));
			}
			FXxHash128 RuntimeIdentity;
			if (auto Result = BuildShaderRuntimeRequestIdentity(Request, RuntimeIdentity); !Result) return Result;
			FXxHash128Builder Production;
			Production.Update("DurinCookedShaderProduction_v1");
			Production.UpdateValue(RuntimeIdentity);
			Production.Update(Builder.GetCompilerEnvironmentIdentity());
			UpdateCanonicalHash(Production, Builder.GetBuildVersion());
			UpdateCanonicalHash(Production, ShaderBuildPrivate::LibraryProductionRevision);
			const auto Descriptor = GetShaderBuildDescriptor();
			UpdateCanonicalHash(Production, Descriptor.Version);
			UpdateCanonicalHash(Production, Descriptor.ConstantsSchema);
			UpdateCanonicalHash(Production, Descriptor.OutputSchema);
			UpdateCanonicalHash(Production, ShaderCompiledOutput::PayloadSchemaVersion);
			UpdateCanonicalHash(Production, ShaderCookedLibrarySchemaVersion);
			for (const FShaderCode& Shader : Output.CompiledShaders)
				Production.UpdateValue(Shader.CodeHash);
			Records.push_back({Request, Production.Finalize(), FShaderCompilerOutput{.CompiledShaders = std::move(Output.CompiledShaders), .Error = {}}});
		}
		return EncodeShaderCookedLibrary(TargetPlatform, TargetProfile, Records, OutBytes);
	}
}

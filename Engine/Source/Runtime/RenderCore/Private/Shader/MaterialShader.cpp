#include "Shader/MaterialShader.h"
#include "Shader/ShaderData.h"

namespace Durin
{
	namespace
	{
		class FMaterialShaderTypeRegistry
		{
		public:
			auto Register(const FMaterialShaderType* Type) -> void
			{
				check(Type);
				const auto Duplicate = std::ranges::find_if(
					Types, [Type](const FMaterialShaderType* Existing) {
						return Existing->GetFName() == Type->GetFName();
					});
				checkf(Duplicate == Types.end(),
					"Duplicate material shader implementation: {}", Type->GetName());
				Types.push_back(Type);
			}

			auto GetTypes() const
				-> const std::vector<const FMaterialShaderType*>&
			{
				return Types;
			}

		private:
			std::vector<const FMaterialShaderType*> Types;
		};

		auto GetMaterialShaderTypeRegistry() -> FMaterialShaderTypeRegistry&
		{
			static FMaterialShaderTypeRegistry Registry;
			return Registry;
		}

		class FMeshMaterialShaderTypeRegistry
		{
		public:
			auto Register(const FMeshMaterialShaderType* Type) -> void
			{
				check(Type);
				checkf(std::ranges::none_of(Types,
					[Type](const FMeshMaterialShaderType* Existing) {
						return Existing->GetFName() == Type->GetFName();
					}), "Duplicate mesh Material shader implementation: {}",
					Type->GetName());
				Types.push_back(Type);
			}
			auto GetTypes() const
				-> const std::vector<const FMeshMaterialShaderType*>&
			{
				return Types;
			}
		private:
			std::vector<const FMeshMaterialShaderType*> Types;
		};

		auto GetMeshMaterialShaderTypeRegistry()
			-> FMeshMaterialShaderTypeRegistry&
		{
			static FMeshMaterialShaderTypeRegistry Registry;
			return Registry;
		}

		auto IsMaterialShaderType(const FShaderType* Type) -> bool
		{
			const auto& Types = FMaterialShaderType::GetTypeList();
			return std::ranges::find(Types, Type) != Types.end();
		}

		auto IsMeshMaterialShaderType(const FShaderType* Type) -> bool
		{
			const auto& Types = FMeshMaterialShaderType::GetTypeList();
			return std::ranges::find(Types, Type) != Types.end();
		}
	}

	class FMaterialShaderMapPayload
	{
	public:
		std::shared_ptr<FShaderMapBase> ShaderMap;
		FMaterialShaderMapIdentity Identity;
		FRenderResourceGeneration Generation;
		FXxHash128 CompatibilityHash;
		std::string CompatibilityText;
	};

	FMaterialShaderType::FMaterialShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		FShaderFactoryFunction InFactory,
		FShouldCompilePermutationFunction InShouldCompilePermutation,
		FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
		const FShaderParametersMetadata* InParametersMetadata)
		: FShaderType(
			InName, InVirtualShaderPath, InFrequency, InEntryPoint, {}, InFactory,
			InShouldCompilePermutation, InModifyCompilationEnvironment,
			InParametersMetadata)
	{
		GetMaterialShaderTypeRegistry().Register(this);
	}

	auto FMaterialShaderType::GetTypeList()
		-> const std::vector<const FMaterialShaderType*>&
	{
		return GetMaterialShaderTypeRegistry().GetTypes();
	}

	FMeshMaterialShaderType::FMeshMaterialShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		FShaderFactoryFunction InFactory,
		FShouldCompilePermutationFunction InShouldCompilePermutation,
		FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
		const FShaderParametersMetadata* InParametersMetadata)
		: FMaterialShaderType(
			InName, InVirtualShaderPath, InFrequency, InEntryPoint, InFactory,
			InShouldCompilePermutation, InModifyCompilationEnvironment,
			InParametersMetadata)
	{
		GetMeshMaterialShaderTypeRegistry().Register(this);
	}

	auto FMeshMaterialShaderType::GetTypeList()
		-> const std::vector<const FMeshMaterialShaderType*>&
	{
		return GetMeshMaterialShaderTypeRegistry().GetTypes();
	}

	auto FMaterialShaderMap::TryCreate(
		FMaterialShaderMapBuildInput Input,
		FMaterialShaderMap& OutMap,
		std::string& OutError) -> bool
	{
		OutMap = {};
		OutError.clear();
		if (Input.ShaderTypes.empty())
		{
			OutError = "Material shader map requires at least one shader type.";
			return false;
		}
		if (!std::isfinite(Input.Identity.OpacityMaskThreshold))
		{
			OutError = "Material shader identity has a non-finite opacity mask threshold.";
			return false;
		}
		if (Input.bContainsGeneratedMaterialStages
			&& Input.CompiledProgramIdentity != Input.Identity.ProgramIdentity)
		{
			OutError = std::format(
				"Compiled material program identity '{}' does not match requested identity '{}'.",
				Input.CompiledProgramIdentity.ToString(),
				Input.Identity.ProgramIdentity.ToString());
			return false;
		}
		if (Input.bContainsGeneratedMaterialStages
			&& (Input.Target.empty() || Input.CompiledTarget != Input.Target))
		{
			OutError = std::format(
				"Compiled material target '{}' does not match requested target '{}'.",
				Input.CompiledTarget, Input.Target);
			return false;
		}

		std::vector<std::pair<FXxHash128, std::string>> ExactTypes;
		size_t MaterialTypeCount = 0;
		for (const FShaderType* Type : Input.ShaderTypes)
		{
			if (Type == nullptr)
			{
				OutError = "Material shader map contains a null shader type.";
				return false;
			}
			if (IsMaterialShaderType(Type))
				++MaterialTypeCount;
			FMaterialShaderPermutationIdentity Permutation{
				.Material = Input.Identity,
				.ShaderType = std::string(Type->GetName()),
				.EntryPoint = std::string(Type->GetEntryPoint()),
				.Target = Input.Target,
				.PermutationId = Input.LocalPermutationId,
				.Frequency = Type->GetFrequency()};
			if (IsMeshMaterialShaderType(Type))
			{
				if (Input.VertexFactoryType == nullptr)
				{
					OutError = std::format(
						"Mesh Material shader type '{}' requires a Vertex Factory type.",
						Type->GetName());
					return false;
				}
				FMeshMaterialShaderPermutationIdentity MeshIdentity{
					.Material = std::move(Permutation),
					.VertexFactoryType = std::string(
						Input.VertexFactoryType->GetName()),
					.MeshPassKey = Input.MeshPassKey,
					.MeshPermutationId = Input.MeshPermutationId};
				ExactTypes.emplace_back(
					GetMaterialShaderIdentityHash(MeshIdentity),
					GetMaterialShaderIdentityText(MeshIdentity));
			}
			else
			{
				ExactTypes.emplace_back(
					GetMaterialShaderIdentityHash(Permutation),
					GetMaterialShaderIdentityText(Permutation));
			}
		}
		if (MaterialTypeCount == 0)
		{
			OutError = "Material shader map contains no registered Material shader type.";
			return false;
		}
		std::ranges::sort(ExactTypes, {}, &std::pair<FXxHash128, std::string>::second);
		FXxHash128Builder SetHash;
		std::string CompatibilityText;
		for (const auto& [Hash, Text] : ExactTypes)
		{
			SetHash.UpdateValue(Hash.HashLow);
			SetHash.UpdateValue(Hash.HashHigh);
			if (!CompatibilityText.empty()) CompatibilityText.append("|");
			CompatibilityText.append(Text);
		}

		auto Candidate = std::make_shared<FMaterialShaderMapPayload>();
		Candidate->ShaderMap = std::make_shared<FShaderMapBase>();
		if (!Candidate->ShaderMap->Initialize(
				Input.ShaderTypes, Input.CompilerOutput, Input.CompileOptions,
				OutError, true))
		{
			return false;
		}
		for (const FShaderType* Type : Input.ShaderTypes)
		{
			FShader* Shader = Candidate->ShaderMap->GetShader(Type);
			if (Shader == nullptr)
			{
				OutError = std::format(
					"Material shader map is missing type '{}'.", Type->GetName());
				return false;
			}
			if (Input.bCreateRHIShaders
				&& Shader->GetOrCreateRHIShader(false) == nullptr)
			{
				OutError = std::format(
					"RHI shader creation returned null for Material type '{}'.",
					Type->GetName());
				return false;
			}
		}
		Candidate->Identity = Input.Identity;
		Candidate->Generation = Input.Generation;
		Candidate->CompatibilityHash = SetHash.Finalize();
		Candidate->CompatibilityText = std::move(CompatibilityText);
		OutMap = FMaterialShaderMap(std::move(Candidate));
		return true;
	}

	auto FMaterialShaderMap::TryCompile(
		FMaterialShaderMapCompileInput Input,
		FMaterialShaderMap& OutMap,
		std::string& OutError) -> bool
	{
		OutMap = {};
		OutError.clear();
		if (Input.ShaderTypes.empty())
		{
			OutError = "Material shader map requires at least one shader type.";
			return false;
		}

		std::vector<const FShaderType*> FixedTypes;
		std::vector<const FCompiledShader*> SelectedGenerated(
			Input.ShaderTypes.size(), nullptr);
		for (size_t Index = 0; Index < Input.ShaderTypes.size(); ++Index)
		{
			const FShaderType* Type = Input.ShaderTypes[Index];
			if (Type == nullptr)
			{
				OutError = "Material shader map contains a null shader type.";
				return false;
			}
			const auto Generated = std::ranges::find_if(
				Input.GeneratedStages, [Type](const FCompiledShader& Shader) {
					return Shader.SourceEntryPoint == Type->GetEntryPoint()
						&& Shader.Frequency == Type->GetFrequency();
				});
			if (Generated != Input.GeneratedStages.end())
				SelectedGenerated[Index] = &*Generated;
			else if (!Input.GeneratedStages.empty()
				&& IsMaterialShaderType(Type)
				&& !IsMeshMaterialShaderType(Type))
			{
				OutError = std::format(
					"Accepted material program has no {} stage for type '{}'.",
					Type->GetEntryPoint(), Type->GetName());
				return false;
			}
			else
				FixedTypes.push_back(Type);
		}

		// Compile each source independently before linking validated stage interfaces.
		std::vector<std::vector<const FShaderType*>> Groups;
		for (const FShaderType* Type : FixedTypes)
		{
			auto Group = std::ranges::find_if(Groups, [Type](const auto& Existing) {
				return GetShaderDataDomain() != EShaderDataDomain::Cooked
					&& Existing.front()->GetVirtualShaderPath() == Type->GetVirtualShaderPath();
			});
			if (Group == Groups.end()) Groups.push_back({Type});
			else Group->push_back(Type);
		}
		std::vector<std::unique_ptr<FShaderMapBase>> FixedMaps;
		for (const auto& Group : Groups)
		{
			auto Map = std::make_unique<FShaderMapBase>();
			auto Options = Input.CompileOptions;
			Options.VirtualShaderPath = Group.front()->GetVirtualShaderPath();
			Options.EntryPoints.clear(); Options.Frequencies.clear();
			for (const auto* Type : Group)
				Type->ModifyCompilationEnvironment({Type, Type->GetVirtualShaderPath(), Type->GetEntryPoint(), Type->GetFrequency()}, Options);
			const auto& RuntimeRequest = Group.front()->GetFrequency() == EShaderFrequency::Fragment
				? Input.FixedFragmentRuntimeRequest : Input.FixedShaderRuntimeRequest;
			if (!RuntimeRequest.empty() && GetShaderDataDomain() == EShaderDataDomain::Cooked)
			{
				FShaderCompilerOutput Output;
				if (!LoadCookedShaderRuntimeRequest(RuntimeRequest, Group, Output, OutError)
					|| !Map->Initialize(Group, Output, Options, OutError)) return false;
			}
			else if (!Map->InitializeFromShaderTypes(Group, Options, OutError)) return false;
			FixedMaps.push_back(std::move(Map));
		}

		FShaderCompilerOutput Combined;
		Combined.bSucceeded = true;
		Combined.CompiledShaders.reserve(Input.ShaderTypes.size());
		for (size_t Index = 0; Index < Input.ShaderTypes.size(); ++Index)
		{
			if (SelectedGenerated[Index] != nullptr)
			{
				Combined.CompiledShaders.push_back(*SelectedGenerated[Index]);
				continue;
			}
			const FShaderType* Type = Input.ShaderTypes[Index];
			const auto FoundMap = std::ranges::find_if(FixedMaps, [Type](const auto& Map) { return Map->FindShaderIndex(Type) != nullptr; });
			if (FoundMap == FixedMaps.end() || !(*FoundMap)->GetCode())
			{
				OutError = std::format(
					"Fixed Material shader type '{}' produced no resource code.",
					Type->GetName());
				return false;
			}
			Combined.CompiledShaders.push_back(
				(*FoundMap)->GetCode()->GetCompiledShader(*(*FoundMap)->FindShaderIndex(Type)));
		}

		return TryCreate({
			.Identity = Input.Identity,
			.Generation = Input.Generation,
			.Target = std::move(Input.Target),
			.LocalPermutationId = Input.LocalPermutationId,
			.VertexFactoryType = Input.VertexFactoryType,
			.MeshPassKey = Input.MeshPassKey,
			.MeshPermutationId = Input.MeshPermutationId,
			.ShaderTypes = Input.ShaderTypes,
			.CompilerOutput = std::move(Combined),
			.CompileOptions = std::move(Input.CompileOptions),
			.bContainsGeneratedMaterialStages = !Input.GeneratedStages.empty(),
			.CompiledProgramIdentity = Input.CompiledProgramIdentity,
			.CompiledTarget = std::move(Input.CompiledTarget),
			.bCreateRHIShaders = Input.bCreateRHIShaders},
			OutMap, OutError);
	}

	auto FMaterialShaderMap::GetIdentity() const
		-> const FMaterialShaderMapIdentity&
	{
		check(Payload);
		return Payload->Identity;
	}

	auto FMaterialShaderMap::GetGeneration() const
		-> const FRenderResourceGeneration&
	{
		check(Payload);
		return Payload->Generation;
	}

	auto FMaterialShaderMap::GetCompatibilityHash() const -> FXxHash128
	{
		return Payload ? Payload->CompatibilityHash : FXxHash128{};
	}

	auto FMaterialShaderMap::GetCompatibilityText() const -> std::string_view
	{
		return Payload ? std::string_view(Payload->CompatibilityText)
			: std::string_view{};
	}

	auto FMaterialShaderMap::GetPipelineLayout() const
		-> const FPipelineLayoutDesc&
	{
		check(Payload && Payload->ShaderMap);
		return Payload->ShaderMap->GetMergedPipelineLayout();
	}

	auto GetShaderFromMaterialPayloadImpl(
		const std::shared_ptr<FMaterialShaderMapPayload>& Payload,
		const FMaterialShaderType* Type) -> FShader*
	{
		return Payload && Payload->ShaderMap
			? Payload->ShaderMap->GetShader(Type) : nullptr;
	}

	auto GetMaterialPayloadIdentityImpl(
		const std::shared_ptr<FMaterialShaderMapPayload>& Payload)
		-> const FMaterialShaderMapIdentity&
	{
		check(Payload);
		return Payload->Identity;
	}

	auto GetMaterialPayloadGenerationImpl(
		const std::shared_ptr<FMaterialShaderMapPayload>& Payload)
		-> const FRenderResourceGeneration&
	{
		check(Payload);
		return Payload->Generation;
	}
}

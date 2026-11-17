#include "Materials/MaterialPreparedProgram.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Shader/ShaderMapPreparation.h"
#include "Shader/MaterialShader.h"
#include "RenderResourcePreparation.h"
#include "Threading/RunnableThread.h"
#include "Shader/ShaderData.h"

namespace Durin
{
	auto FMaterialShader::InitializeParameterBindings() -> FShaderOperationResult
	{
		// Registered owner metadata is only the static projection. The complete
		// dynamic Material/VF schema is admitted before accepted bindings publish.
		auto Map = FShaderParameterMap::FromReflection(Reflection);
		if (!Map) { Bindings = {}; return std::unexpected(Map.error()); }
		return Bindings.Bind(Type ? Type->GetParametersMetadata() : nullptr, *Map);
	}
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
			return std::ranges::contains(Types, Type);
		}

		auto IsMeshMaterialShaderType(const FShaderType* Type) -> bool
		{
			const auto& Types = FMeshMaterialShaderType::GetTypeList();
			return std::ranges::contains(Types, Type);
		}
	}

	class FMeshMaterialShaderMapPayload
	{
	public:
		std::shared_ptr<FShaderMapBase> ShaderMap;
		FMeshMaterialShaderMapIdentity Identity;
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
		const FShaderParametersMetadata* InParametersMetadata,
		std::optional<FMaterialShaderEntryDescriptor> InDescription,
		bool (*InMaterialPolicy)(const FMaterialShaderPermutationParameters&),
		void (*InMaterialEnvironment)(const FMaterialShaderPermutationParameters&, FShaderCompileOptions&))
		: FShaderType(
			InName, InVirtualShaderPath, InFrequency, InEntryPoint, {}, InFactory,
			InShouldCompilePermutation, InModifyCompilationEnvironment,
			InParametersMetadata), Description(std::move(InDescription)),
		MaterialPolicy(InMaterialPolicy), MaterialEnvironment(InMaterialEnvironment)
	{
		if (Description) {
			checkf((InFrequency != EShaderFrequency::Vertex || Description->VertexFactory)
				&& (Description->Domain != EMaterialDomain::PostProcess || (!Description->VertexFactory && InFrequency == EShaderFrequency::Fragment)),
				"Generated Material Shader descriptions require an applicable VF and domain/stage combination.");
			Description->ShaderType = GetName(); Description->Name = GetEntryPoint(); Description->Frequency = GetFrequency();
		}
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
		const FShaderParametersMetadata* InParametersMetadata,
		std::optional<FMaterialShaderEntryDescriptor> InDescription,
		bool (*InMaterialPolicy)(const FMaterialShaderPermutationParameters&),
		void (*InMaterialEnvironment)(const FMaterialShaderPermutationParameters&, FShaderCompileOptions&))
		: FMaterialShaderType(
			InName, InVirtualShaderPath, InFrequency, InEntryPoint, InFactory,
			InShouldCompilePermutation, InModifyCompilationEnvironment,
			InParametersMetadata, std::move(InDescription), InMaterialPolicy, InMaterialEnvironment)
	{
		GetMeshMaterialShaderTypeRegistry().Register(this);
	}

	auto FMeshMaterialShaderType::GetTypeList()
		-> const std::vector<const FMeshMaterialShaderType*>&
	{
		return GetMeshMaterialShaderTypeRegistry().GetTypes();
	}

	auto GetMaterialShaderEntries() -> std::vector<FMaterialShaderEntryDescriptor>
	{
		std::vector<FMaterialShaderEntryDescriptor> Result;
		for (const auto* Type : FMaterialShaderType::GetTypeList())
			if (const auto* D = Type->GetMaterialEntryDescriptor()) Result.push_back(*D);
		std::ranges::sort(Result, [](const auto& A, const auto& B) {
			return std::tie(A.Entry, A.GeometryUsage, A.ShaderType) < std::tie(B.Entry, B.GeometryUsage, B.ShaderType);
		});
		return Result;
	}
	auto FindMaterialShaderEntry(std::string_view Name) -> const FMaterialShaderEntryDescriptor*
	{
		for (const auto* Type : FMaterialShaderType::GetTypeList())
			if (Type->GetName() == Name) return Type->GetMaterialEntryDescriptor();
		return nullptr;
	}
	auto GetMaterialCompiledEntryPoints() -> std::vector<std::string_view>
	{
		std::vector<std::string_view> Result;
		for (const auto& D : GetMaterialShaderEntries())
			if (!std::ranges::contains(Result, D.Name)) Result.push_back(D.Name);
		return Result;
	}
	auto FMaterialShaderType::ShouldCompileMaterialPermutation(const FMaterialShaderPermutationParameters& P) const -> bool
	{
		if (!Description || P.ShaderType != this || P.Frequency != GetFrequency() || P.EntryPoint != GetEntryPoint()
			|| P.PermutationId != Description->PermutationId || Description->Domain != P.Domain || (!P.bIncludeEditorPasses && Description->bEditorOnly)
			|| !(Description->BlendModes & (1u << uint8(P.Properties.BlendMode)))
			|| (P.Domain == EMaterialDomain::Surface && (!(Description->GeometryUsage & P.Properties.GeometryUsage)
				|| !(Description->PassUsage & P.Properties.PassUsage)))) return false;
		if (Description->VertexFactory && Description->VertexFactory() != P.VertexFactoryType) return false;
		return ShouldCompilePermutation(P) && (!MaterialPolicy || MaterialPolicy(P))
			&& (!P.VertexFactoryType || P.VertexFactoryType->ShouldCompilePermutation(P));
	}
	auto FMaterialShaderType::ModifyMaterialCompilationEnvironment(const FMaterialShaderPermutationParameters& P, FShaderCompileOptions& O) const -> void
	{
		ModifyCompilationEnvironment(P, O);
		if (P.VertexFactoryType) P.VertexFactoryType->ModifyCompilationEnvironment(P, O);
		if (MaterialEnvironment) MaterialEnvironment(P, O);
	}
	auto GetMaterialShaderCompilationOptions(std::string_view Name, const FMaterialStaticProperties& Properties,
		EMaterialDomain Domain, const FMaterialCompilerEnvironment& E) -> FShaderCompileOptions
	{
		const auto* D = FindMaterialShaderEntry(Name);
		check(D);
		const auto* Type = *std::ranges::find_if(FMaterialShaderType::GetTypeList(), [&](const auto* T) { return T->GetName() == Name; });
		FMaterialShaderPermutationParameters P;
		P.ShaderType = Type; P.VirtualShaderPath = Type->GetVirtualShaderPath(); P.EntryPoint = D->Name; P.Frequency = D->Frequency;
		P.Target = E.Target == "metal-library-1" ? MetalLibraryShaderTarget : E.Target.starts_with("metal") ? MetalShaderTarget : VulkanShaderTarget;
		P.PermutationId = D->PermutationId; P.VertexFactoryType = D->VertexFactory ? D->VertexFactory() : nullptr;
		P.Domain = Domain; P.Properties = CanonicalizeMaterialShaderProperties(Properties);
		uint8 AvailablePasses = 0;
		for (const auto& Entry : GetMaterialShaderEntries()) if (Entry.Frequency == EShaderFrequency::Fragment && (!Entry.bEditorOnly || E.bIncludeEditorPasses)) AvailablePasses |= Entry.PassUsage;
		P.Properties.PassUsage &= AvailablePasses;
		P.Quality = E.Quality; P.FeatureLevel = E.FeatureLevel; P.bIncludeEditorPasses = E.bIncludeEditorPasses;
		auto O = MakeMaterialShaderCompileOptions(P.Target, D->Frequency == EShaderFrequency::Vertex ? FMaterialStaticProperties{} : Properties);
		O.EntryPoints = {D->Name.data()}; O.Frequencies = {D->Frequency};
		Type->ModifyMaterialCompilationEnvironment(P, O);
		checkf(O.Target == P.Target && O.EntryPoints.size() == 1 && O.EntryPoints[0] == D->Name && O.Frequencies.size() == 1 && O.Frequencies[0] == D->Frequency,
			"Material compilation callbacks must preserve registered target and entry identity.");
		return O;
	}
	auto SelectMaterialShaderEntries(const FMaterialStaticProperties& Properties, bool Editor, EMaterialDomain Domain,
		const FMaterialCompilerEnvironment* Environment) -> FMaterialShaderEntrySet
	{
		FMaterialShaderEntrySet Result;
		uint8 Passes = 0;
		for (const auto& D : GetMaterialShaderEntries())
			if (D.Frequency == EShaderFrequency::Fragment && (!D.bEditorOnly || Editor)) Passes |= D.PassUsage;
		for (const auto& D : GetMaterialShaderEntries()) {
			const auto* Type = *std::ranges::find_if(FMaterialShaderType::GetTypeList(), [&](const auto* T) { return T->GetName() == D.ShaderType; });
			FMaterialShaderPermutationParameters P;
			P.ShaderType = Type; P.VirtualShaderPath = Type->GetVirtualShaderPath(); P.EntryPoint = D.Name; P.Frequency = D.Frequency;
			P.Target = Environment && Environment->Target == "metal-library-1" ? MetalLibraryShaderTarget : Environment && Environment->Target.starts_with("metal") ? MetalShaderTarget : VulkanShaderTarget;
			P.PermutationId = D.PermutationId; P.VertexFactoryType = D.VertexFactory ? D.VertexFactory() : nullptr;
			P.Domain = Domain; P.Properties = CanonicalizeMaterialShaderProperties(Properties); P.Properties.PassUsage &= Passes; P.bIncludeEditorPasses = Editor;
			if (Environment) { P.Quality = Environment->Quality; P.FeatureLevel = Environment->FeatureLevel; }
			if (Type->ShouldCompileMaterialPermutation(P)) Result.emplace_back(D.ShaderType);
		}
		return Result;
	}

	auto FMeshMaterialShaderMap::TryCreate(
		FMeshMaterialShaderMapBuildInput Input,
		FMeshMaterialShaderMap& OutMap) -> FShaderCompletion
	{
		FShaderOperationResult Result;
		OutMap = {};

		if (Input.ShaderTypes.empty())
		{
			return std::unexpected(std::string("Material shader map requires at least one shader type."));
		}
		if (!std::isfinite(Input.Identity.OpacityMaskThreshold))
		{
			return std::unexpected(std::string("Material shader identity has a non-finite opacity mask threshold."));
		}
		if (Input.bContainsGeneratedMaterialStages
			&& Input.CompiledProgramIdentity != Input.Identity.ProgramIdentity)
		{
			return std::unexpected(std::format(
				"Compiled material program identity '{}' does not match requested identity '{}'.",
				(Input.CompiledProgramIdentity.ToString()), (Input.Identity.ProgramIdentity.ToString())));
		}
		if (Input.bContainsGeneratedMaterialStages
			&& (Input.Target.empty() || Input.CompiledTarget != Input.Target))
		{
			return std::unexpected(std::format(
				"Compiled material target '{}' does not match requested target '{}'.",
				(Input.CompiledTarget), (Input.Target)));
		}

		std::vector<std::pair<FXxHash128, std::string>> ExactTypes;
		size_t MaterialTypeCount = 0;
		for (const FShaderType* Type : Input.ShaderTypes)
		{
			if (Type == nullptr)
			{
				return std::unexpected(std::string("Material shader map contains a null shader type."));
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
			if (Input.bContainsGeneratedMaterialStages)
			{
				if (const auto* D = FindMaterialShaderEntry(Type->GetName()); D) {
					if (D->PermutationId != Input.LocalPermutationId || (D->VertexFactory && D->VertexFactory() != Input.VertexFactoryType))
						return std::unexpected(std::string("Registered Material Shader permutation or Vertex Factory is incompatible."));
				}
				const auto SourceEntry = Type->GetEntryPoint();
				const auto Contract = std::ranges::find_if(Input.GeneratedEntries, [&](const auto& E) {
					return E.Shader.ShaderType == Type->GetName() && E.Shader.EntryPoint == SourceEntry
						&& E.Shader.Frequency == Type->GetFrequency() && E.Shader.PermutationId == Input.LocalPermutationId;
				});
				// Indirect factories retain their own fixed vertex program.
				if (Contract != Input.GeneratedEntries.end())
				{
					auto RequestedMaterial = Input.Identity;
					if (RequestedMaterial.BlendMode != EMaterialBlendMode::Masked)
						RequestedMaterial.OpacityMaskThreshold = FMaterialStaticProperties{}.OpacityMaskThreshold;
					if (Contract->Shader.Material != RequestedMaterial || Contract->Shader.Target != Input.Target
						|| Contract->Shader.PassContractVersion != CurrentMaterialPassContractVersion
						|| (Contract->Mesh && (!Input.VertexFactoryType || Contract->Mesh->VertexFactoryType != Input.VertexFactoryType->GetName())))
						return std::unexpected(std::format("Generated material execution contract is incompatible: requested {}, compiled {}, factory {}.", GetMaterialShaderIdentityText(Permutation), GetMaterialShaderIdentityText(Contract->Shader), Input.VertexFactoryType ? std::string(Input.VertexFactoryType->GetName()) : "none"));
					if (const auto* D = FindMaterialShaderEntry(Type->GetName()); D) {
                        const auto Slot = std::ranges::find(Input.ShaderTypes, Type) - Input.ShaderTypes.begin();
                        if (size_t(Slot) >= Input.CompilerOutput.CompiledShaders.size()) return std::unexpected(std::string("Material binding code slot is missing."));
                        if (!Input.CompiledParameters) return std::unexpected(std::string("Compiled parameter metadata is missing."));
                        const auto& Parameters = *Input.CompiledParameters;
                        const auto Schema = BuildMaterialParameterSchema(D->ShaderType, Parameters.Layout, Parameters.CollectionLayouts);
                        const auto Logical = ValidateMaterialShaderBindings(Input.CompilerOutput.CompiledShaders[Slot].Reflection,
                            D->ShaderType, D->Frequency, Parameters.Layout, Parameters.CollectionLayouts);
                        if (!Schema || Contract->ParameterSchemaIdentity != Schema->Identity || !Logical || *Logical != Contract->Bindings
                            || Contract->ParameterMapIdentity != GetMaterialParameterMapIdentity(*Contract, Input.CompilerOutput.CompiledShaders[Slot]))
                            return std::unexpected(std::string("Accepted material schema/allocation does not match its compiled product."));

						FMaterialStaticProperties P; P.BlendMode = static_cast<EMaterialBlendMode>(uint8(Input.Identity.BlendMode)); P.ShadingModel = static_cast<EMaterialShadingModel>(uint8(Input.Identity.ShadingModel));
						if (Contract->Shader.OutputLayout != GetMaterialShaderOutputLayout(*D, P)) return std::unexpected(std::string("Material Shader output contract is incompatible."));
					}
					Permutation.PassContractVersion = Contract->Shader.PassContractVersion;
					Permutation.OutputLayout = Contract->Shader.OutputLayout;
				}
				else if (Type->GetFrequency() == EShaderFrequency::Fragment || FindMaterialShaderEntry(Type->GetName()))
					return std::unexpected(std::string("Generated material entry contract is missing."));
			}
			if (IsMeshMaterialShaderType(Type))
			{
				if (Input.VertexFactoryType == nullptr)
				{
					return std::unexpected(std::format("Mesh Material shader type '{}' requires a Vertex Factory type.", (std::string(Type->GetName()))));
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
			return std::unexpected(std::string("Material shader map contains no registered Material shader type."));
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

		auto Candidate = std::make_shared<FMeshMaterialShaderMapPayload>();
		Candidate->ShaderMap = std::make_shared<FShaderMapBase>();
		if (!(Result = Candidate->ShaderMap->Initialize(Input.ShaderTypes, Input.CompilerOutput, Input.CompileOptions, true)))
		{
			return Result;
		}
		for (const FShaderType* Type : Input.ShaderTypes)
		{
			FShader* Shader = Candidate->ShaderMap->GetShader(Type);
			if (Shader == nullptr)
			{
				return std::unexpected(std::format("Material shader map is missing type '{}'.", (std::string(Type->GetName()))));
			}
            if (IsMaterialShaderType(Type)) {
                const auto Entry = std::ranges::find(Input.GeneratedEntries, std::string(Type->GetName()), [](const auto& E) { return E.Shader.ShaderType; });
                if (Entry != Input.GeneratedEntries.end()) static_cast<FMaterialShader*>(Shader)->SetAcceptedParameterBindings(Entry->Bindings);
                else if (const auto* D = FindMaterialShaderEntry(Type->GetName()); D && !Input.bContainsGeneratedMaterialStages) {
                    const auto Bindings = ValidateMaterialShaderBindings(Shader->GetReflection(), D->ShaderType, D->Frequency, {}, {});
                    if (!Bindings) return std::unexpected(Bindings.error());
                    static_cast<FMaterialShader*>(Shader)->SetAcceptedParameterBindings(*Bindings);
                }
            }

			if (Input.bCreateRHIShaders
				&& Shader->GetOrCreateRHIShader(false) == nullptr)
			{

				return std::unexpected(std::format("RHI shader creation returned null for Material type '{}'.", (std::string(Type->GetName()))));
			}
		}
		Candidate->Identity = Input.Identity;
		Candidate->Generation = Input.Generation;
		Candidate->CompatibilityHash = SetHash.Finalize();
		Candidate->CompatibilityText = std::move(CompatibilityText);
		OutMap = FMeshMaterialShaderMap(std::move(Candidate));
		return {};
	}

	static auto CompileMeshMaterialShaderMap(
		FMeshMaterialShaderMapCompileInput Input,
		FMeshMaterialShaderMap& OutMap,
		FShaderCompletion (*Initialize)(FShaderMapBase&, std::span<const FShaderType* const>, const FShaderCompileOptions&)) -> FShaderCompletion
	{
		FShaderOperationResult Result;
		OutMap = {};

		if (Input.ShaderTypes.empty())
		{
			return std::unexpected(std::string("Material shader map requires at least one shader type."));
		}

		std::vector<const FShaderType*> FixedTypes;
		std::vector<const FShaderCode*> SelectedGenerated(
			Input.ShaderTypes.size(), nullptr);
		for (size_t Index = 0; Index < Input.ShaderTypes.size(); ++Index)
		{
			const FShaderType* Type = Input.ShaderTypes[Index];
			if (Type == nullptr)
			{
				return std::unexpected(std::string("Material shader map contains a null shader type."));
			}
			const auto Contract = std::ranges::find_if(Input.GeneratedEntries, [&](const auto& E) {
				return E.Shader.ShaderType == Type->GetName() && E.Shader.EntryPoint == Type->GetEntryPoint()
					&& E.Shader.Frequency == Type->GetFrequency() && E.Shader.PermutationId == Input.LocalPermutationId;
			});
			const auto Generated = Contract == Input.GeneratedEntries.end() || Input.GeneratedEntries.size() != Input.GeneratedStages.size()
				? Input.GeneratedStages.end() : Input.GeneratedStages.begin() + (Contract - Input.GeneratedEntries.begin());
			if (Generated != Input.GeneratedStages.end() && (Generated->SourceEntryPoint != Type->GetEntryPoint() || Generated->Frequency != Type->GetFrequency()))
				return std::unexpected(std::string("Generated material code does not match its registered execution contract."));
			if (Generated != Input.GeneratedStages.end()) SelectedGenerated[Index] = &*Generated;
			else if (!Input.GeneratedStages.empty()
				&& IsMaterialShaderType(Type)
				&& (!IsMeshMaterialShaderType(Type) || FindMaterialShaderEntry(Type->GetName())))
			{
				return std::unexpected(std::format(
				"Accepted material program has no {} stage for type '{}'.",
				(std::string(Type->GetEntryPoint())), (std::string(Type->GetName()))));
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
				Type->ModifyCompilationEnvironment({Type, Type->GetVirtualShaderPath(), Type->GetEntryPoint(), Type->GetFrequency(), Options.Target, Input.LocalPermutationId, Input.VertexFactoryType}, Options);
			const auto& RuntimeRequest = Group.front()->GetFrequency() == EShaderFrequency::Fragment
				? Input.FixedFragmentRuntimeRequest : Input.FixedShaderRuntimeRequest;
			if (!RuntimeRequest.empty() && GetShaderDataDomain() == EShaderDataDomain::Cooked)
			{
				FShaderCompilerOutput Output;
				FShaderOperationResult CookedError;
				if (!(CookedError = LoadCookedShaderRuntimeRequest(RuntimeRequest, Group, Output)))
					return CookedError;
				if (!(Result = Map->Initialize(Group, Output, Options))) return Result;
			}
			else if (auto Completion = Initialize(*Map, Group, Options); !Completion) return Completion;
			FixedMaps.push_back(std::move(Map));
		}

		FShaderCompilerOutput Combined;
		Combined.Status = EShaderBuildStatus::Ok;
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
				return std::unexpected(std::format("Fixed Material shader type '{}' produced no resource code.", (std::string(Type->GetName()))));
			}
			Combined.CompiledShaders.push_back(
				(*FoundMap)->GetCode()->GetCompiledShader(*(*FoundMap)->FindShaderIndex(Type)));
		}

		return FMeshMaterialShaderMap::TryCreate({
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
			.GeneratedEntries = Input.GeneratedEntries,
            .CompiledParameters = Input.CompiledParameters,
			.bCreateRHIShaders = Input.bCreateRHIShaders}, OutMap);
	}

	auto FMeshMaterialShaderMap::TryCompile(FMeshMaterialShaderMapCompileInput Input, FMeshMaterialShaderMap& OutMap) -> FShaderCompletion
	{
		return CompileMeshMaterialShaderMap(std::move(Input), OutMap, InitializeShaderMapFromTypes);
	}

	auto FMeshMaterialShaderMap::TryCompileSync(FMeshMaterialShaderMapCompileInput Input, FMeshMaterialShaderMap& OutMap) -> FShaderCompletion
	{
		if (IsInRenderingThread())
			return FShaderCompletion::Failure("Synchronous shader compilation is forbidden on the rendering thread.");
		return CompileMeshMaterialShaderMap(std::move(Input), OutMap, InitializeShaderMapFromTypesSync);
	}

	auto FMeshMaterialShaderMap::GetIdentity() const
		-> const FMeshMaterialShaderMapIdentity&
	{
		check(Payload);
		return Payload->Identity;
	}

	auto FMeshMaterialShaderMap::GetGeneration() const
		-> const FRenderResourceGeneration&
	{
		check(Payload);
		return Payload->Generation;
	}

	auto FMeshMaterialShaderMap::GetCompatibilityHash() const -> FXxHash128
	{
		return Payload ? Payload->CompatibilityHash : FXxHash128{};
	}

	auto FMeshMaterialShaderMap::GetCompatibilityText() const -> std::string_view
	{
		return Payload ? std::string_view(Payload->CompatibilityText)
			: std::string_view{};
	}

	auto FMeshMaterialShaderMap::GetPipelineLayout() const
		-> const FPipelineLayoutDesc&
	{
		check(Payload && Payload->ShaderMap);
		return Payload->ShaderMap->GetMergedPipelineLayout();
	}

	auto GetShaderFromMaterialPayloadImpl(
		const std::shared_ptr<FMeshMaterialShaderMapPayload>& Payload,
		const FMaterialShaderType* Type) -> FShader*
	{
		return Payload && Payload->ShaderMap
			? Payload->ShaderMap->GetShader(Type) : nullptr;
	}

	auto GetMaterialPayloadIdentityImpl(
		const std::shared_ptr<FMeshMaterialShaderMapPayload>& Payload)
		-> const FMeshMaterialShaderMapIdentity&
	{
		check(Payload);
		return Payload->Identity;
	}

	auto GetMaterialPayloadGenerationImpl(
		const std::shared_ptr<FMeshMaterialShaderMapPayload>& Payload)
		-> const FRenderResourceGeneration&
	{
		check(Payload);
		return Payload->Generation;
	}
}

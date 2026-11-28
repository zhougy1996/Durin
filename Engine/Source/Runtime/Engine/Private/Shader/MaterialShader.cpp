#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialPreparedProgram.h"
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

	auto FMeshMaterialShaderMap::Assemble(const FMeshMaterialShaderMapCompileInput& Input,
		const FShaderCompilerOutput& CompilerOutput, std::span<const FMaterialShaderEntryRequest* const> Entries,
		FMeshMaterialShaderMap& OutMap) -> FShaderOperationResult
	{
		FShaderOperationResult Result;

		std::vector<std::pair<FXxHash128, std::string>> ExactTypes;
		size_t MaterialTypeCount = 0;
		for (size_t Index = 0; Index < Input.ShaderTypes.size(); ++Index)
		{
			const auto* Type = Input.ShaderTypes[Index];
			if (IsMaterialShaderType(Type))
				++MaterialTypeCount;
			FMaterialShaderPermutationIdentity Permutation{
				.Material = Input.Identity,
				.ShaderType = std::string(Type->GetName()),
				.EntryPoint = std::string(Type->GetEntryPoint()),
				.Target = Input.Target,
				.PermutationId = Input.LocalPermutationId,
				.Frequency = Type->GetFrequency()};
			if (const auto* Entry = Entries[Index])
			{
				Permutation.PassContractVersion = Entry->Shader.PassContractVersion;
				Permutation.OutputLayout = Entry->Shader.OutputLayout;
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
		if (!(Result = Candidate->ShaderMap->Initialize(Input.ShaderTypes, CompilerOutput, Input.CompileOptions, true)))
		{
			return Result;
		}
		for (size_t Index = 0; Index < Input.ShaderTypes.size(); ++Index)
		{
			const auto* Type = Input.ShaderTypes[Index];
			FShader* Shader = Candidate->ShaderMap->GetShader(Type);
			if (Shader == nullptr)
			{
				return std::unexpected(std::format("Material shader map is missing type '{}'.", (std::string(Type->GetName()))));
			}
			if (IsMaterialShaderType(Type))
			{
				auto* MaterialShader = static_cast<FMaterialShader*>(Shader);
				if (const auto* Entry = Entries[Index])
					MaterialShader->SetAcceptedParameterBindings(Entry->Bindings);
				else if (const auto* D = FindMaterialShaderEntry(Type->GetName()))
				{
					const auto Bindings = ValidateMaterialShaderBindings(Shader->GetReflection(), D->ShaderType, D->Frequency, {}, {});
					if (!Bindings) return std::unexpected(Bindings.error());
					MaterialShader->SetAcceptedParameterBindings(*Bindings);
				}
			}

			if (Input.bCreateRHIShaders
				&& Shader->GetOrCreateRHIShader(false) == nullptr)
			{

				return std::unexpected(std::format("RHI shader creation returned null for Material type '{}'.", (std::string(Type->GetName()))));
			}
		}
		Candidate->Identity = Input.Identity;
		Candidate->CompatibilityHash = SetHash.Finalize();
		Candidate->CompatibilityText = std::move(CompatibilityText);
		OutMap = FMeshMaterialShaderMap(std::move(Candidate));
		return {};
	}

	template<class TResult>
	auto FMeshMaterialShaderMap::Compile(
		FMeshMaterialShaderMapCompileInput Input,
		FMeshMaterialShaderMap& OutMap,
		TResult (*Initialize)(FShaderMapBase&, std::span<const FShaderType* const>, const FShaderCompileOptions&)) -> TResult
	{
		FShaderOperationResult Result;
		OutMap = {};

		if (Input.ShaderTypes.empty())
		{
			return std::unexpected(std::string("Material shader map requires at least one shader type."));
		}

		if (!std::isfinite(Input.Identity.OpacityMaskThreshold))
			return std::unexpected(std::string("Invalid material opacity mask threshold."));
		const auto* Accepted = Input.AcceptedMap.get();
		if (Accepted && (Accepted->ShaderMapId != Input.Identity.ProgramIdentity || Accepted->Layout.Identity != Input.Identity.RenderLayout
			|| Accepted->PassContractVersion != CurrentMaterialPassContractVersion || Accepted->Target != Input.Target))
			return std::unexpected(std::string("Accepted material shader map does not match the requested program, layout, pass contract or target."));

		auto Material = Input.Identity;
		if (Material.BlendMode != EMaterialBlendMode::Masked)
			Material.OpacityMaskThreshold = FMaterialStaticProperties{}.OpacityMaskThreshold;

		std::vector<const FShaderType*> FixedTypes;
		std::vector<const FShaderCode*> SelectedGenerated(Input.ShaderTypes.size());
		std::vector<const FMaterialShaderEntryRequest*> Entries(Input.ShaderTypes.size());
		for (size_t Index = 0; Index < Input.ShaderTypes.size(); ++Index)
		{
			const auto* Type = Input.ShaderTypes[Index];
			if (!Type) return std::unexpected(std::string("Material shader map contains a null shader type."));
			if (!Accepted || (Input.bUseFixedVertexStage && Type->GetFrequency() == EShaderFrequency::Vertex))
			{
				FixedTypes.push_back(Type);
				continue;
			}
			const auto Slot = Accepted->FindShaderIndex(*Type, Input.VertexFactoryType, Input.LocalPermutationId);
			if (!Slot)
				return std::unexpected(std::format("Accepted material program has no compatible stage for type '{}' permutation {}.",
					Type->GetName(), Input.LocalPermutationId));
			const auto& Entry = Accepted->Entries[*Slot];
			if (Entry.Shader.Material != Material)
				return std::unexpected(std::string("Accepted material entry does not match the requested material properties."));
			Entries[Index] = &Entry;
			SelectedGenerated[Index] = &Accepted->CompiledShaders[*Slot];
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

		return Assemble(Input, Combined, Entries, OutMap);
	}

	auto FMeshMaterialShaderMap::TryCompile(FMeshMaterialShaderMapCompileInput Input, FMeshMaterialShaderMap& OutMap) -> FShaderMapQueryResult
	{
		return Compile(std::move(Input), OutMap, InitializeShaderMapFromTypes);
	}

	auto FMeshMaterialShaderMap::TryCompileSync(FMeshMaterialShaderMapCompileInput Input, FMeshMaterialShaderMap& OutMap) -> FShaderCompletion
	{
		if (IsInRenderingThread())
			return FShaderCompletion::Failure("Synchronous shader compilation is forbidden on the rendering thread.");
		return Compile(std::move(Input), OutMap, InitializeShaderMapFromTypesSync);
	}

	auto FMeshMaterialShaderMap::GetIdentity() const
		-> const FMeshMaterialShaderMapIdentity&
	{
		check(Payload);
		return Payload->Identity;
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
}

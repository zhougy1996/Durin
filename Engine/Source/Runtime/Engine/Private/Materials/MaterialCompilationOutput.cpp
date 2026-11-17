#include "VertexFactory.h"
#include "MaterialPreparedProgram.h"
#include "Materials/MaterialAttributeDefinitionMap.h"

namespace Durin
{
	auto GetMaterialShaderOutputLayout(const FMaterialShaderEntryDescriptor& D, const FMaterialStaticProperties& P) -> FMaterialRenderLayoutIdentity
	{
		const uint32 Tag = D.Frequency == EShaderFrequency::Vertex ? 0u : uint32(D.Entry) + 1
			+ (D.Entry == EMaterialShaderEntry::GBuffer && P.ShadingModel == EMaterialShadingModel::ClearCoat ? 0x100u : 0u);
		return {CurrentMaterialPassContractVersion, {0x4d415450, 0x4f555450, 1, Tag}};
	}
	auto GetMaterialEntryProperties(EMaterialShaderEntry Entry, const FMaterialStaticProperties& Properties)
		-> std::vector<EMaterialProperty>
	{
		using P = EMaterialProperty;
		if (Entry == EMaterialShaderEntry::PostProcess) return {P::MP_EmissiveColor};
		if (Entry == EMaterialShaderEntry::LocalVertex || Entry == EMaterialShaderEntry::SplineVertex) return {P::MP_WorldPositionOffset};
		if (Entry == EMaterialShaderEntry::Shadow || Entry == EMaterialShaderEntry::HitProxy)
			return Properties.BlendMode == EMaterialBlendMode::Masked ? std::vector<P>{P::MP_OpacityMask} : std::vector<P>{};
		if (Entry == EMaterialShaderEntry::GBuffer && Properties.ShadingModel == EMaterialShadingModel::Unlit) return {};
		auto Result = FMaterialAttributeDefinitionMap::GetActiveProperties(EMaterialDomain::Surface, Properties.ShadingModel, Properties.BlendMode);
		std::erase(Result, P::MP_WorldPositionOffset);
		return Result;
	}

	auto BuildMaterialCompilationOutput(const MIR::FModule& IR, const FMaterialRenderLayout& Layout,
		std::span<const FMaterialCompilerParameterDeclaration> Parameters, std::span<const FMaterialParameterCollectionLayout> Collections,
		const FMaterialStaticProperties& Properties, const FMaterialCompilerEnvironment& Environment, FMaterialProgramIdentity Identity)
		-> FMaterialCompilationOutput
	{
		FMaterialCompilationOutput Result;
		Result.Domain = IR.Domain; Result.bIncludeEditorPasses = Environment.bIncludeEditorPasses; Result.Quality = Environment.Quality; Result.FeatureLevel = Environment.FeatureLevel; Result.StaticProperties = CanonicalizeMaterialShaderProperties(Properties);
		Result.Layout = Layout; Result.ActiveParameters.assign(Parameters.begin(), Parameters.end());
		Result.Requirements = BuildMaterialProgramRequirements(IR);
		Result.CollectionLayouts.assign(Collections.begin(), Collections.end());
		for (auto& Collection : Result.CollectionLayouts) { Collection.AssetPath.clear(); Collection.DefaultPayload.clear(); }
		const auto Selected = SelectMaterialShaderEntries(Properties, Environment.bIncludeEditorPasses, IR.Domain, &Environment);
		for (const auto& Entry : Selected)
		{
			const auto& Descriptor = *FindMaterialShaderEntry(Entry);
			const bool Vertex = Descriptor.Frequency == EShaderFrequency::Vertex;
			FMaterialShaderEntryRequest Request;
			Request.Shader = {.Material = {.RenderLayout = Layout.Identity, .ProgramIdentity = Identity, .BlendMode = Properties.BlendMode, .ShadingModel = Properties.ShadingModel, .OpacityMaskThreshold = CanonicalizeMaterialShaderProperties(Properties).OpacityMaskThreshold}, .ShaderType = std::string(Descriptor.ShaderType), .EntryPoint = std::string(Descriptor.Name), .Target = Environment.Target, .PermutationId = Descriptor.PermutationId, .Frequency = Descriptor.Frequency, .PassContractVersion = Environment.PassContractVersion, .OutputLayout = GetMaterialShaderOutputLayout(Descriptor, Properties)};
			if (Descriptor.VertexFactory) Request.Mesh = FMeshMaterialShaderPermutationIdentity{.Material = Request.Shader,
				.VertexFactoryType = std::string(Descriptor.VertexFactory()->GetName())};
			auto& Requirements = Request.Requirements;
			if (Descriptor.Entry == EMaterialShaderEntry::PostProcess) Requirements.SceneTextures.push_back({EMaterialSceneTextureId::PostProcessInput0, EMaterialSceneTextureFilter::Linear, true});
			std::vector<bool> Visited(IR.Nodes.size());
			std::function<void(uint32)> Visit = [&](uint32 Index) {
				if (Visited[Index]) return;
				Visited[Index] = true;
				if (!Vertex && IR.Nodes[Index].Opcode == EMaterialProgramOpcode::VertexInterpolator) return;
				for (const auto Input : IR.Nodes[Index].Inputs) Visit(Input);
			};
			for (const auto Property : GetMaterialEntryProperties(Descriptor.Entry, Properties))
			{
				const auto& Root = IR.GetAttribute(Property);
				Requirements.AttributeIDs.push_back(Root.AttributeID);
				if (Root.bExpression) Visit(Root.ExpressionIndex);
				if (Property == EMaterialProperty::MP_WorldPositionOffset)
					Requirements.bUsesWPO = Root.bExpression || Root.Literal != FMaterialProgramLiteral{};
				if (Property == EMaterialProperty::MP_OpacityMask) Requirements.bUsesMask = true;
			}
			// A shared geometry entry transports interpolators demanded by its selected
			// pixel passes. Pixel closures stop at the explicit interpolation boundary.
			if (Vertex)
			{
				std::vector<bool> Pixel(IR.Nodes.size());
				std::function<void(uint32)> VisitPixel = [&](uint32 Index) {
					if (Pixel[Index]) return; Pixel[Index] = true;
					if (IR.Nodes[Index].Opcode == EMaterialProgramOpcode::VertexInterpolator) return;
					for (const auto Input : IR.Nodes[Index].Inputs) VisitPixel(Input);
				};
				for (const auto PixelEntry : Selected)
					if (FindMaterialShaderEntry(PixelEntry)->Frequency == EShaderFrequency::Fragment)
						for (const auto Property : GetMaterialEntryProperties(FindMaterialShaderEntry(PixelEntry)->Entry, Properties))
							if (const auto& Root = IR.GetAttribute(Property); Root.bExpression) VisitPixel(Root.ExpressionIndex);
				for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
					if (Pixel[Index] && IR.Nodes[Index].Opcode == EMaterialProgramOpcode::VertexInterpolator) Visit(Index);
			}
			MIR::FModule ContextIR; ContextIR.Domain = IR.Domain; ContextIR.Nodes.clear();
			for (uint32 Index = 0; Index < IR.Nodes.size(); ++Index)
			{
				if (!Visited[Index]) continue;
				const auto& Node = IR.Nodes[Index]; ContextIR.Nodes.push_back(Node);
				if (Node.Opcode == EMaterialProgramOpcode::SceneTexture || Node.Opcode == EMaterialProgramOpcode::SceneDepth)
				{
					const auto Read = Node.GetSceneTextureRead();
					Requirements.SceneTextures.push_back({Read.SceneTextureId, Read.Filter, true});
				}
				if (Node.Opcode == EMaterialProgramOpcode::VertexInterpolator) Requirements.Interpolators.push_back(Node.GetSemantics());
				if (Node.Opcode == EMaterialProgramOpcode::CollectionParameter)
					Requirements.CollectionIDs.push_back(Node.GetCollectionParameter().CollectionId);
				if (Node.Opcode != EMaterialProgramOpcode::Parameter && Node.Opcode != EMaterialProgramOpcode::TextureParameter) continue;
				Requirements.ParameterIDs.push_back(Node.GetParameterId());
				for (const auto& Field : Layout.Fields)
					if (Field.ParameterId == Node.GetParameterId() && Field.Storage == EMaterialRenderFieldStorage::Resource)
						Requirements.ResourceIndices.push_back(uint16(Field.CompactIndex));
			}
			Requirements.Context = BuildMaterialProgramRequirements(ContextIR);
			const auto Unique = [](auto& Values) { std::ranges::sort(Values); Values.erase(std::ranges::unique(Values).begin(), Values.end()); };
			std::ranges::sort(Requirements.SceneTextures, [](const auto& A, const auto& B) { return std::tie(A.InputId, A.Filter) < std::tie(B.InputId, B.Filter); });
			Requirements.SceneTextures.erase(std::ranges::unique(Requirements.SceneTextures).begin(), Requirements.SceneTextures.end());
			Unique(Requirements.AttributeIDs); Unique(Requirements.ParameterIDs); Unique(Requirements.ResourceIndices); Unique(Requirements.CollectionIDs);
			if (auto Schema = BuildMaterialParameterSchema(Request.Shader.ShaderType, Layout, Collections))
                Request.ParameterSchemaIdentity = Schema->Identity;
            Result.Entries.push_back(std::move(Request));
		}
		std::ranges::sort(Result.Entries, [](const auto& A, const auto& B) { return MaterialShaderIdentityLess(A.Shader, B.Shader); });
		return Result;
	}
    auto AcceptMaterialShaderBindings(FMaterialCompilationOutput& Output, std::span<const FShaderCode> Stages) -> FMaterialLayoutValidationResult
    {
        if (Stages.size() != Output.Entries.size()) return {.Error = EMaterialLayoutError::InvalidReflection};
        std::vector<std::vector<FMaterialShaderBinding>> Accepted;
        for (size_t I = 0; I < Stages.size(); ++I)
        {
            const auto* D = FindMaterialShaderEntry(Output.Entries[I].Shader.ShaderType);
            if (!D) return {.Error = EMaterialLayoutError::InvalidReflection};
            auto Bindings = ValidateMaterialShaderBindings(Stages[I].Reflection, D->ShaderType, D->Frequency,
                Output.Layout, Output.CollectionLayouts);
            if (!Bindings) return {.Error = EMaterialLayoutError::InvalidReflection};
            Accepted.push_back(std::move(*Bindings));
        }
        for (size_t I = 0; I < Accepted.size(); ++I) {
            Output.Entries[I].Bindings = std::move(Accepted[I]);
            Output.Entries[I].ParameterMapIdentity = GetMaterialParameterMapIdentity(Output.Entries[I], Stages[I]);
        }
        return {};
    }

	auto ValidateMaterialCompilationOutput(const FMaterialCompilationOutput& Output,
		FMaterialProgramIdentity Identity, std::string_view Target, std::span<const FShaderCode> Stages)
		-> FMaterialLayoutValidationResult
	{
		const FMaterialLayoutValidationResult Rejected{.Error = EMaterialLayoutError::InvalidReflection};
		if (Output.BindingSchemaVersion != MaterialBindingSchemaVersion || !IsSupportedMaterialQualityLevel(Output.Quality) || !IsSupportedMaterialFeatureLevel(Output.FeatureLevel)
			|| !ValidateMaterialConfiguration(Output.Domain, Output.StaticProperties)
			|| Output.StaticProperties != CanonicalizeMaterialShaderProperties(Output.StaticProperties)) return Rejected;
		FMaterialCompilerEnvironment Environment; Environment.Target = Target; Environment.Quality = Output.Quality; Environment.FeatureLevel = Output.FeatureLevel;
		Environment.bIncludeEditorPasses = Output.bIncludeEditorPasses;
		MIR::FModule ExpectedIR; ExpectedIR.Domain = Output.Domain;
		const auto Expected = BuildMaterialCompilationOutput(ExpectedIR, Output.Layout,
			Output.ActiveParameters, Output.CollectionLayouts, Output.StaticProperties, Environment, Identity);
		if (Output.Entries.size() > MaterialProgramMaxShaderEntries || Output.Entries.size() != Expected.Entries.size() || Stages.size() != Output.Entries.size()) return Rejected;
		const auto Canonical = [](const auto& Values) {
			return std::ranges::is_sorted(Values) && std::adjacent_find(Values.begin(), Values.end()) == Values.end();
		};
		for (size_t Index = 0; Index < Output.Entries.size(); ++Index)
		{
			const auto& Entry = Output.Entries[Index]; const auto& Required = Entry.Requirements;
			if (Entry.Shader != Expected.Entries[Index].Shader || Entry.Mesh != Expected.Entries[Index].Mesh
				|| Required.AttributeIDs != Expected.Entries[Index].Requirements.AttributeIDs
				|| !Canonical(Required.ParameterIDs) || !Canonical(Required.ResourceIndices) || !Canonical(Required.CollectionIDs)
				|| (Output.Domain == EMaterialDomain::Surface && !Required.SceneTextures.empty()) || Required.Interpolators.size() > MaterialProgramMaxVertexInterpolators) return Rejected;
			if ((Required.bUsesWPO && Entry.Shader.Frequency != EShaderFrequency::Vertex)
				|| Required.bUsesMask != Expected.Entries[Index].Requirements.bUsesMask
				|| !std::ranges::all_of(Required.Interpolators, IsValidMaterialValueSemantics)) return Rejected;
			if (Output.Domain == EMaterialDomain::PostProcess && (!Required.Context.bMaterialView || !Required.Context.bViewport
				|| Required.Context.bMaterialPrimitive || Required.Context.bObjectTransforms || Required.Context.bBoundsCenter
				|| Required.Context.bTangentFrame || Required.Context.bVertexNormal || !Required.Interpolators.empty())) return Rejected;
			for (size_t S = 0; S < Required.SceneTextures.size(); ++S)
			{
				const auto& Read = Required.SceneTextures[S];
				if (!Read.bRequired || Read.InputId > EMaterialSceneTextureId::SceneDepth || Read.Filter > EMaterialSceneTextureFilter::Linear
					|| (Read.InputId == EMaterialSceneTextureId::SceneDepth && Read.Filter != EMaterialSceneTextureFilter::Point)
					|| (S && std::tie(Required.SceneTextures[S-1].InputId, Required.SceneTextures[S-1].Filter) >= std::tie(Read.InputId, Read.Filter))) return Rejected;
			}
			for (const auto Id : Required.ParameterIDs)
				if (std::ranges::find(Output.ActiveParameters, Id, &FMaterialCompilerParameterDeclaration::Id) == Output.ActiveParameters.end()) return Rejected;
			for (const auto Resource : Required.ResourceIndices)
				if (!std::ranges::any_of(Output.Layout.Fields, [&](const auto& F) {
					return F.Storage == EMaterialRenderFieldStorage::Resource && F.CompactIndex == Resource && std::ranges::contains(Required.ParameterIDs, F.ParameterId);
				})) return Rejected;
			for (const auto Id : Required.CollectionIDs)
				if (std::ranges::find(Output.CollectionLayouts, Id, &FMaterialParameterCollectionLayout::CollectionId) == Output.CollectionLayouts.end()) return Rejected;
			// Stage products occupy the same canonical full-key slots as their contracts.
			const auto* Shader = &Stages[Index];
			if (Shader->SourceEntryPoint != Entry.Shader.EntryPoint || Shader->Frequency != Entry.Shader.Frequency) return Rejected;
            const auto* D = FindMaterialShaderEntry(Entry.Shader.ShaderType);
            const auto Bindings = ValidateMaterialShaderBindings(Shader->Reflection, D->ShaderType, D->Frequency,
                Output.Layout, Output.CollectionLayouts);
            if (!Bindings || *Bindings != Entry.Bindings
                || Entry.ParameterSchemaIdentity != Expected.Entries[Index].ParameterSchemaIdentity
                || Entry.ParameterSchemaIdentity == FXxHash128{}
                || Entry.ParameterMapIdentity != GetMaterialParameterMapIdentity(Entry, *Shader)) return Rejected;
            for (const auto& Binding : Entry.Bindings)
            {
                if (Binding.OwnerName == "Material" && (Binding.Type == ERHIBindingType::Texture || Binding.Type == ERHIBindingType::Sampler))
                { if (!std::ranges::contains(Required.ResourceIndices, Binding.ResourceIndex)) return Rejected; }
                if (Binding.OwnerName == "Material" && Binding.Name.starts_with("MaterialCollection") && !std::ranges::contains(Required.CollectionIDs,
                    Output.CollectionLayouts[Binding.ResourceIndex].CollectionId)) return Rejected;
                const auto HasScene = [&](EMaterialSceneTextureId Id, std::optional<EMaterialSceneTextureFilter> Filter = {}) {
                    return std::ranges::any_of(Required.SceneTextures, [&](const auto& Read) { return Read.InputId == Id && (!Filter || Read.Filter == *Filter); });
                };
                if ((Binding.Name == "PostProcessInput0" && !HasScene(EMaterialSceneTextureId::PostProcessInput0))
                    || (Binding.Name == "MaterialSceneDepth" && !HasScene(EMaterialSceneTextureId::SceneDepth))
                    || (Binding.Name == "MaterialScenePointSampler" && !std::ranges::any_of(Required.SceneTextures, [](const auto& Read) { return Read.Filter == EMaterialSceneTextureFilter::Point; }))
                    || (Binding.Name == "MaterialSceneLinearSampler" && !std::ranges::any_of(Required.SceneTextures, [](const auto& Read) { return Read.Filter == EMaterialSceneTextureFilter::Linear; }))) return Rejected;
            }
		}
		return {};
	}

}

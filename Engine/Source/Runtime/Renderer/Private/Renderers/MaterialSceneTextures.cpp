#include "Renderers/MaterialSceneTextures.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"

namespace Durin
{
	auto FMaterialSceneTexturePassParameters::GetRDGParametersMetadata() -> const FRDGParametersMetadata*
	{
		using P = FMaterialSceneTexturePassParameters;
		static const std::array Members{
			MakeRDGTextureReadMetadata<P, decltype(P::PostProcessInput0)>("PostProcessInput0", offsetof(P, PostProcessInput0)),
			MakeRDGTextureReadMetadata<P, decltype(P::SceneDepth)>("SceneDepth", offsetof(P, SceneDepth)),
			MakeRDGColorAttachmentBindingMetadata<P, decltype(P::Output)>("Output", offsetof(P, Output))};
		static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FMaterialSceneTexturePassParameters", Members);
		return &Metadata;
	}
	auto MaterialSceneTextureUV(FVector2f UV, FIntPoint Extent, FIntPoint Min, FIntPoint Size) -> FVector2f
	{
		const auto Pixel = FVector2f(Min) + glm::clamp(UV, FVector2f(0), FVector2f(1)) * FVector2f(Size);
		return glm::clamp(Pixel, FVector2f(Min) + .5f, FVector2f(Min + Size) - .5f) / FVector2f(Extent);
	}
    auto ResolveMaterialSceneTextureBindings(FRHICommandListImmediate& Commands,
        std::span<const FMaterialShaderBinding> Bindings, FRHITexture* Input0, FRHITexture* Depth,
        const FMaterialSceneTextureUniform& Uniform) -> std::expected<FMaterialSceneTextureBindings, std::string>
    {
        FShaderParameterMap Map;
        for (const auto& B : Bindings) {
            if (B.OwnerName != "SceneTextures") continue;
            auto Added = Map.AddParameterAllocation(B.Name, {B.SetIndex, B.BindingIndex, B.ArraySize, B.Type});
            if (!Added) return std::unexpected(Added.error());
        }
        FShaderParameterBindings Resolved;
        auto Bound = Resolved.Bind(FMaterialSceneTextureParameterOwner::GetParametersMetadata(), Map);
        if (!Bound) return std::unexpected(Bound.error());
        FMaterialSceneTextureBindings Result;
        FMaterialSceneTextureParameterOwner::FParameters Values;
        if (Map.ContainsParameterAllocation("MaterialSceneTextures")) {
            Result.Uniform = Commands.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
            Values.MaterialSceneTextures = Result.Uniform;
        }
        for (uint32 I = 0; I < 2; ++I) {
            const auto Name = I == 0 ? "PostProcessInput0" : "MaterialSceneDepth";
            if (!Map.ContainsParameterAllocation(Name)) continue;
            auto* Texture = I == 0 ? Input0 : Depth;
            if (!Texture) return std::unexpected("A required scene texture provider is unavailable");
            auto View = MakeDefaultTextureViewDesc(*Texture, ERHITextureViewUsage::Sampled);
            View.Range = {I == 0 ? ERHITextureAspect::Color : ERHITextureAspect::Depth, 0, 1, 0, 1};
            Result.Textures[I] = GDynamicRHI->RHICreateTextureView(Texture, View);
            if (!Result.Textures[I]) return std::unexpected("Scene texture view allocation failed");
        }
        Values.PostProcessInput0 = Input0; Values.MaterialSceneDepth = Depth;
        for (uint32 I = 0; I < 2; ++I) {
            const auto Name = I == 0 ? "MaterialScenePointSampler" : "MaterialSceneLinearSampler";
            if (!Map.ContainsParameterAllocation(Name)) continue;
            auto Desc = FRHISamplerDesc::PointClamp();
            if (I == 1) { Desc.MinFilter = ESamplerFilter::Linear; Desc.MagFilter = ESamplerFilter::Linear; }
            Result.Samplers[I] = GDynamicRHI->RHICreateSampler(Desc);
            if (!Result.Samplers[I]) return std::unexpected("Scene sampler allocation failed");
        }
        Values.MaterialScenePointSampler = Result.Samplers[0]; Values.MaterialSceneLinearSampler = Result.Samplers[1];
        Result.Parameters = ResolveShaderParameterResources(*FMaterialSceneTextureParameterOwner::GetParametersMetadata(), Resolved.GetResources(), &Values);
        // The owner lowers the two texture members to its retained, aspect-specific views.
        for (size_t I = 0; I < Resolved.GetResources().size(); ++I) {
            const auto Offset = Resolved.GetResources()[I].Offset;
            if (Offset == offsetof(FMaterialSceneTextureParameterOwner::FParameters, PostProcessInput0)) Result.Parameters[I].Resource = Result.Textures[0];
            if (Offset == offsetof(FMaterialSceneTextureParameterOwner::FParameters, MaterialSceneDepth)) Result.Parameters[I].Resource = Result.Textures[1];
        }
        return Result;
    }

	auto PlanMaterialSceneTextures(FRDGBuilder& Graph, const FMaterialShaderEntryRequest& Entry,
		const FMaterialSceneTextureInputs& Inputs, std::optional<FRDGTextureHandle> Output) -> std::expected<FMaterialSceneTexturePlan, std::string>
	{
		if (Entry.Shader.ShaderType != "FPostProcessMaterialShader" || Entry.Shader.EntryPoint != "PostProcessMaterialMain"
			|| Entry.Shader.Frequency != EShaderFrequency::Fragment || Entry.Shader.PassContractVersion != CurrentMaterialPassContractVersion || Entry.Mesh || Entry.Requirements.SceneTextures.empty())
			return std::unexpected("Scene texture planning requires a PostProcess fragment entry.");
		std::array<bool, 2> Required{};
		FMaterialSceneTexturePlan Plan;
		std::optional<std::pair<EMaterialSceneTextureId, EMaterialSceneTextureFilter>> Previous;
		for (const auto& Read : Entry.Requirements.SceneTextures)
		{
			const auto Key = std::pair{Read.InputId, Read.Filter};
			if (!Read.bRequired || Read.InputId > EMaterialSceneTextureId::SceneDepth || Read.Filter > EMaterialSceneTextureFilter::Linear
				|| (Read.InputId == EMaterialSceneTextureId::SceneDepth && Read.Filter != EMaterialSceneTextureFilter::Point)
				|| (Previous && *Previous >= Key)) return std::unexpected("Scene texture requirements are invalid or noncanonical.");
			Previous = Key; Required[size_t(Read.InputId)] = true;
			Plan.bPointSampler |= Read.Filter == EMaterialSceneTextureFilter::Point;
			Plan.bLinearSampler |= Read.Filter == EMaterialSceneTextureFilter::Linear;
		}
		// Alpha preservation always reads Input0, even for a constant effect.
		if (!Required[0] || !Plan.bLinearSampler || !Inputs.PostProcessInput0) return std::unexpected("PostProcessInput0 is unavailable.");
		const std::array Sources{&Inputs.PostProcessInput0, &Inputs.SceneDepth};
		for (size_t Index = 0; Index < Required.size(); ++Index)
		{
			if (!Required[Index]) continue;
			if (!*Sources[Index]) return std::unexpected("A required material scene input is unavailable.");
			const auto& Source = **Sources[Index];
			const auto Desc = Graph.GetTextureDesc(Source.Texture);
			if (!Desc || Desc->Dimension != ETextureDimension::Texture2D || Desc->NumSamples != 1 || Desc->ArraySize != 1
				|| !(Desc->Flags & ETextureCreateFlags::ShaderResource) || Source.ViewMin.x < 0 || Source.ViewMin.y < 0
				|| Source.ViewSize.x <= 0 || Source.ViewSize.y <= 0 || Source.ViewMin.x > Desc->Extent.x - Source.ViewSize.x
				|| Source.ViewMin.y > Desc->Extent.y - Source.ViewSize.y
				|| (Index == 0 ? Desc->Format != EPixelFormat::RGBA16_FLOAT : !(GetTextureAspects(Desc->Format) & ERHITextureAspect::Depth)))
				return std::unexpected("Material scene input has an incompatible texture or viewport.");
			auto& Viewport = Index == 0 ? Plan.Uniform.Input0Viewport : Plan.Uniform.DepthViewport;
			auto& Size = Index == 0 ? Plan.Uniform.Input0TextureSize : Plan.Uniform.DepthTextureSize;
			Viewport = FVector4f(Source.ViewMin.x, Source.ViewMin.y, Source.ViewSize.x, Source.ViewSize.y);
			Size = FVector4f(Desc->Extent.x, Desc->Extent.y, 1.f / Desc->Extent.x, 1.f / Desc->Extent.y);
			++Plan.TextureCount;
		}
		if (Required[1] && (!std::isfinite(Inputs.ClearDepth) || (Inputs.ClearDepth != 0.f && Inputs.ClearDepth != 1.f)
			|| !std::ranges::all_of(Inputs.ProjectionRows, [](const auto& V) { return std::isfinite(V.x) && std::isfinite(V.y) && std::isfinite(V.z) && std::isfinite(V.w); })))
			return std::unexpected("Material scene depth projection is invalid.");
		if (Required[1])
		{
			const FMatrix4f Projection(Inputs.ProjectionRows[0], Inputs.ProjectionRows[1], Inputs.ProjectionRows[2], Inputs.ProjectionRows[3]);
			const auto Determinant = glm::determinant(Projection);
			if (!std::isfinite(Determinant) || std::abs(Determinant) <= 1.e-30f)
				return std::unexpected("Material scene depth projection is singular.");
		}
		Plan.Uniform.ProjectionRows = Inputs.ProjectionRows;
		Plan.Uniform.DepthParameters.x = Inputs.ClearDepth;
		const auto& Input0 = *Inputs.PostProcessInput0;
		if (Output)
		{
			const auto Desc = Graph.GetTextureDesc(*Output);
			const auto InputDesc = Graph.GetTextureDesc(Input0.Texture);
			if (!Desc || !InputDesc || *Output == Input0.Texture || (Inputs.SceneDepth && *Output == Inputs.SceneDepth->Texture)
				|| Desc->Extent != InputDesc->Extent || Desc->Format != EPixelFormat::RGBA16_FLOAT
				|| Desc->Dimension != ETextureDimension::Texture2D || Desc->ArraySize != 1 || Desc->NumMips != 1
				|| Desc->NumSamples != 1 || !(Desc->Flags & ETextureCreateFlags::RenderTargetable)
				|| !(Desc->Flags & ETextureCreateFlags::ShaderResource)) return std::unexpected("Invalid material effect output.");
			Plan.Output = {*Output, Input0.ViewMin, Input0.ViewSize};
		}
		else
		{
			Plan.Output = {Graph.CreateTexture({.Texture = FRHITextureCreateDesc::Create2D("MaterialPostProcessOutput",
				Input0.ViewSize.x, Input0.ViewSize.y, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)}, "Material.PostProcessOutput"),
				{0, 0}, Input0.ViewSize};
		}
		Plan.Parameters = Graph.AllocParameters<FMaterialSceneTexturePassParameters>();
		Plan.Parameters->PostProcessInput0 = {Input0.Texture, {ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Required[1]) Plan.Parameters->SceneDepth = {Inputs.SceneDepth->Texture, {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		Plan.Parameters->Output = {Plan.Output.Texture, {ERHITextureAspect::Color, 0, 1, 0, 1}, ERHIRenderTargetLoadAction::DontCare};
		return Plan;
	}
}

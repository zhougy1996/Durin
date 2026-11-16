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
		const FShaderReflectionData& Reflection, const FMaterialSceneTexturePassParameters& Parameters,
		const FRDGParameterResolver& Resolver, const FMaterialSceneTextureUniform& Uniform)
		-> std::expected<FMaterialSceneTextureBindings, std::string>
	{
		FMaterialSceneTextureBindings Result;
		std::array<bool, 5> Seen{};
		const std::array Names{"MaterialSceneTextures", "PostProcessInput0", "MaterialSceneDepth", "MaterialScenePointSampler", "MaterialSceneLinearSampler"};
		for (const auto& Binding : Reflection.ResourceBindings)
		{
			if (Binding.SetIndex != 0 || Binding.BindingIndex < 40 || Binding.BindingIndex > 44) continue;
			const auto Slot = Binding.BindingIndex - 40;
			const auto Type = Slot == 0 ? ERHIBindingType::UniformBuffer : Slot < 3 ? ERHIBindingType::Texture : ERHIBindingType::Sampler;
			if (Seen[Slot] || Binding.Name != Names[Slot] || Binding.Type != Type || Binding.ArraySize != 1)
				return std::unexpected("Material scene binding reflection is incompatible.");
			Seen[Slot] = true;
			FRHIShaderParameterResource Parameter{.SetIndex = 0, .BindingIndex = Binding.BindingIndex, .Type = Type};
			if (Slot == 0)
			{
				Result.Uniform = Commands.CreateUniformBufferRange(&Uniform, sizeof(Uniform));
				Parameter.Resource = Result.Uniform.Buffer; Parameter.Offset = Result.Uniform.Offset; Parameter.Size = Result.Uniform.Size;
			}
			else if (Slot < 3)
			{
				auto* Texture = Slot == 1 ? Resolver.GetTexture(Parameters.PostProcessInput0) : Resolver.GetTexture(Parameters.SceneDepth);
				if (!Texture) return std::unexpected("A required material scene texture is unavailable during binding.");
				auto View = MakeDefaultTextureViewDesc(*Texture, ERHITextureViewUsage::Sampled);
				View.Range = {Slot == 1 ? ERHITextureAspect::Color : ERHITextureAspect::Depth, 0, 1, 0, 1};
				Result.Textures[Slot-1] = GDynamicRHI->RHICreateTextureView(Texture, View);
				Parameter.Resource = Result.Textures[Slot-1];
			}
			else
			{
				auto Desc = FRHISamplerDesc::PointClamp();
				if (Slot == 4) { Desc.MinFilter = ESamplerFilter::Linear; Desc.MagFilter = ESamplerFilter::Linear; }
				Result.Samplers[Slot-3] = GDynamicRHI->RHICreateSampler(Desc);
				Parameter.Resource = Result.Samplers[Slot-3];
			}
			if (!Parameter.Resource) return std::unexpected("Material scene binding allocation failed.");
			Result.Parameters.push_back(Parameter);
		}
		return Result;
	}

	auto PlanMaterialSceneTextures(FRDGBuilder& Graph, const FMaterialShaderEntryRequest& Entry,
		const FMaterialSceneTextureInputs& Inputs) -> std::expected<FMaterialSceneTexturePlan, std::string>
	{
		if (Entry.Shader.ShaderType != "PostProcessMaterial" || Entry.Shader.EntryPoint != "PostProcessMaterialMain"
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
		Plan.Output = {Graph.CreateTexture({.Texture = FRHITextureCreateDesc::Create2D("MaterialPostProcessOutput",
			Input0.ViewSize.x, Input0.ViewSize.y, EPixelFormat::RGBA16_FLOAT)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)}, "Material.PostProcessOutput"),
			{0, 0}, Input0.ViewSize};
		Plan.Parameters = Graph.AllocParameters<FMaterialSceneTexturePassParameters>();
		Plan.Parameters->PostProcessInput0 = {Input0.Texture, {ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Required[1]) Plan.Parameters->SceneDepth = {Inputs.SceneDepth->Texture, {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		Plan.Parameters->Output = {Plan.Output.Texture, {ERHITextureAspect::Color, 0, 1, 0, 1}, ERHIRenderTargetLoadAction::DontCare};
		return Plan;
	}
}

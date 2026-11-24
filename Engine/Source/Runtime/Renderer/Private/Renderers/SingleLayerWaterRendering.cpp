#include "Renderers/SingleLayerWaterRendering.h"

#include "RDG/RDG.h"
#include "Math/Operations.h"
#include "RHICommandList.h"
#include "Renderers/DeferredDirectionalLightingRendering.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/SceneRenderGraphTypes.h"
#include "Renderers/SceneRendererProfiling.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		using namespace RendererPrivate;
		constexpr FRHITextureSubresourceRange ColorRange{ERHITextureAspect::Color, 0, 1, 0, 1};
		constexpr FRHITextureSubresourceRange DepthRange{ERHITextureAspect::Depth, 0, 1, 0, 1};

		void SetWaterViewRect(FRHICommandListImmediate& Commands, const FSceneView& View)
		{
			Commands.SetViewport(float(View.ViewportX), float(View.ViewportY), 0,
				float(View.ViewportX + View.ViewportWidth), float(View.ViewportY + View.ViewportHeight), 1);
			Commands.SetScissor(float(View.ViewportX), float(View.ViewportY), float(View.ViewportWidth), float(View.ViewportHeight));
		}

#define WATER_READ(Field) MakeRDGTextureAccessMetadata<FParameters, decltype(FParameters::Field)>(#Field, offsetof(FParameters, Field))
#define WATER_COLOR(Field) MakeRDGColorAttachmentBindingMetadata<FParameters, decltype(FParameters::Field)>(#Field, offsetof(FParameters, Field))
#define WATER_DEPTH(Field) MakeRDGDepthStencilAttachmentBindingMetadata<FParameters, decltype(FParameters::Field)>(#Field, offsetof(FParameters, Field))
#define WATER_VALUE(Field, Type) MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::Field), Type>(#Field, offsetof(FParameters, Field))

		struct FSnapshotParameters
		{
			TRDGValueRead<FSceneColorPassResult> Base;
			std::optional<TRDGValueRead<FVolumetricCloudPassResult>> Cloud;
			FRDGTextureAccess Color;
			std::optional<FRDGTextureAccess> CloudColor;
			FRDGTextureAccess Depth;
			FRDGColorAttachmentBinding ColorOutput, DepthOutput;
			TRDGValueWrite<FSceneColorPassResult> Completion;

			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using FParameters = FSnapshotParameters;
				static const std::array Members{WATER_VALUE(Base, FSceneColorPassResult),
					WATER_VALUE(Cloud, FVolumetricCloudPassResult), WATER_READ(Color), WATER_READ(CloudColor),
					WATER_READ(Depth), WATER_COLOR(ColorOutput), WATER_COLOR(DepthOutput), WATER_VALUE(Completion, FSceneColorPassResult)};
				static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>("FWaterSnapshotParameters", Members);
				return &Metadata;
			}
		};

		struct FDepthParameters
		{
			TRDGValueRead<FSceneColorPassResult> Snapshot;
			FRDGDepthStencilAttachmentBinding Depth;
			FRDGColorAttachmentBinding Coverage;
			TRDGValueWrite<FSceneColorPassResult> Completion;

			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using FParameters = FDepthParameters;
				static const std::array Members{WATER_VALUE(Snapshot, FSceneColorPassResult),
					WATER_DEPTH(Depth), WATER_COLOR(Coverage), WATER_VALUE(Completion, FSceneColorPassResult)};
				static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>("FWaterDepthParameters", Members);
				return &Metadata;
			}
		};

		struct FShadingParameters
		{
			TRDGValueRead<FSceneColorPassResult> Nearest;
			FRDGTextureAccess BackgroundColor, BackgroundDepth, Coverage;
			std::optional<FRDGTextureAccess> Irradiance, BrdfLut, DirectionalShadow;
			FRDGDepthStencilAttachmentBinding SceneDepth;
			FRDGColorAttachmentBinding Color, NormalRoughness, SpecularF0;
			std::optional<FRDGColorAttachmentBinding> CloudColor;
			TRDGValueWrite<FSceneColorPassResult> Completion;

			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using FParameters = FShadingParameters;
				static const std::array Members{WATER_VALUE(Nearest, FSceneColorPassResult), WATER_READ(BackgroundColor),
					WATER_READ(BackgroundDepth), WATER_READ(Coverage), WATER_READ(Irradiance), WATER_READ(BrdfLut),
					WATER_READ(DirectionalShadow), WATER_DEPTH(SceneDepth), WATER_COLOR(Color),
					WATER_COLOR(NormalRoughness), WATER_COLOR(SpecularF0), WATER_COLOR(CloudColor), WATER_VALUE(Completion, FSceneColorPassResult)};
				static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>("FWaterShadingParameters", Members);
				return &Metadata;
			}
		};

		struct FReflectionParameters
		{
			TRDGValueRead<FSceneColorPassResult> Shading;
			FRDGTextureAccess Coverage, Depth, NormalRoughness, SpecularF0, Environment, BrdfLut;
			FRDGColorAttachmentBinding Color;
			std::optional<FRDGColorAttachmentBinding> CloudColor;
			TRDGValueWrite<FSceneColorPassResult> Completion;

			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using FParameters = FReflectionParameters;
				static const std::array Members{WATER_VALUE(Shading, FSceneColorPassResult),
					WATER_READ(Coverage), WATER_READ(Depth), WATER_READ(NormalRoughness), WATER_READ(SpecularF0),
					WATER_READ(Environment), WATER_READ(BrdfLut), WATER_COLOR(Color), WATER_COLOR(CloudColor),
					WATER_VALUE(Completion, FSceneColorPassResult)};
				static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>("FWaterReflectionParameters", Members);
				return &Metadata;
			}
		};

#undef WATER_READ
#undef WATER_COLOR
#undef WATER_DEPTH
#undef WATER_VALUE

		auto DrawWaterSurfaces(FRHICommandListImmediate& Commands, const FSceneView& View, const FParallelMeshDrawCommandPass& Draws,
			FRHITexture* BackgroundColor, FRHITexture* BackgroundDepth, FRHITexture* Coverage, EWaterVolumeReadback Readback) -> bool
		{
			struct alignas(16) FWaterViewUniform
			{
				std::array<FVector4f, 4> Projection;
				FVector4f DepthParameters;
			};
			const auto Projection = Math::TransposeToFloat(View.ProjectionMatrix);
			const FWaterViewUniform ViewData{{Projection[0], Projection[1], Projection[2], Projection[3]},
				FVector4f(View.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f,
					float(Readback), View.Settings.Mode.bEnableWaterRefractionMask ? 1.0f : 0.0f, 0.0f)};
			const auto ViewBuffer = Commands.CreateUniformBufferRange(&ViewData, sizeof(ViewData));
			if (!ViewBuffer.Buffer) return false;
			FMeshDrawCommandStateCache Cache;
			std::map<const FMeshPassBindings*, FMeshDrawShaderBindings> PassBindings;
			for (const auto& Draw : Draws.GetMeshDrawCommands())
			{
				auto [Found, Inserted] = PassBindings.try_emplace(Draw.Pass, Draw.Pass->Fragment);
				if (Inserted)
				{
					const auto& Layout = *Draw.Command->Pipeline->GetInitializer().FragmentLayout;
					for (const auto& Entry : Layout.GetEntries())
					{
						if (Entry.OwnerName != "WaterScene") continue;
						if (Entry.Name == "WaterView")
						{
							if (Entry.ArraySize != 1 || Entry.RuntimeType != ERHIBindingType::UniformBufferDynamic) return false;
							Found->second.Parameters.push_back({ViewBuffer.Buffer, Entry.SetIndex, Entry.BindingIndex, 0,
								Entry.RuntimeType, ViewBuffer.Offset, ViewBuffer.Size});
							continue;
						}
						FRHITexture* Texture = nullptr;
						if (Entry.Name == "WaterBackgroundColor") Texture = BackgroundColor;
						else if (Entry.Name == "WaterBackgroundDepth") Texture = BackgroundDepth;
						else if (Entry.Name == "WaterCoverage") Texture = Coverage;
						if (!Texture || Entry.ArraySize != 1 || Entry.RuntimeType != ERHIBindingType::Texture) return false;
						Found->second.Parameters.push_back({Texture, Entry.SetIndex, Entry.BindingIndex, 0, Entry.RuntimeType});
					}
				}
				if (!Draw.SubmitDraw(Commands, &Cache, {}, &Found->second)) return false;
			}
			return true;
		}
	}

	auto AddSingleLayerWaterPasses(FRDGBuilder& Graph, const FSceneView& View,
		const FBaseSceneGraphOutput& Base, const FCloudCompositeGraphOutput& Cloud,
		const FResolvedSceneResources& Resolved, FPostProcessRenderer& Fullscreen,
		const FSceneEnvironmentInputs& Environment, std::optional<FRDGTextureHandle> DirectionalShadow,
		uint32 Width, uint32 Height, const FRendererQualificationPolicy& Qualification) -> std::optional<TRDGValueHandle<FSceneColorPassResult>>
	{
		using namespace RendererPrivate;
		const auto& Meshes = Resolved.Receiver.StaticMeshes;
		const auto& Draws = Meshes.GetMeshDrawCommandPass({EMeshPipelinePass::SingleLayerWater});
		if (!Resolved.WaterSnapshot || Draws.GetMeshDrawCommands().empty()) return std::nullopt;
		const auto& DepthDraws = Meshes.GetMeshDrawCommandPass({EMeshPipelinePass::SingleLayerWaterDepth});
		const auto Readback = Qualification.WaterVolumeReadback;

		auto Texture = [&](const char* Name, EPixelFormat Format) {
			return Graph.CreateTexture({.Texture = FRHITextureCreateDesc::Create2D(Name, Width, Height, Format)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
				.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f))}, Name);
		};
		const auto BackgroundColor = Texture("Water.BackgroundColor", EPixelFormat::RGBA16_FLOAT);
		const auto BackgroundDepth = Texture("Water.BackgroundDeviceZ", EPixelFormat::R32_FLOAT);
		const auto Coverage = Texture("Water.Coverage", EPixelFormat::R8_UNORM);
		const auto NormalRoughness = Texture("Water.NormalRoughness", EPixelFormat::RGBA16_FLOAT);
		const auto SpecularF0 = Texture("Water.SpecularF0", EPixelFormat::RGBA16_FLOAT);
		const auto Read = [](FRDGTextureHandle Handle, FRHITextureSubresourceRange Range = ColorRange) {
			return FRDGTextureAccess{Handle, Range, ERDGUse::Read, ERHIAccess::GraphicsShaderRead};
		};
		const auto SnapshotDone = Graph.CreateValue<FSceneColorPassResult>("Water.SnapshotResult", "scene-color-result");
		const auto DepthDone = Graph.CreateValue<FSceneColorPassResult>("Water.DepthResult", "scene-color-result");
		const auto ShadingDone = Graph.CreateValue<FSceneColorPassResult>("Water.ShadingResult", "scene-color-result");
		auto Snapshot = Graph.AllocParameters<FSnapshotParameters>();
		Snapshot->Base = {Base.Completion};
		if (Cloud.Completion && Cloud.Composite)
		{
			Snapshot->Cloud = TRDGValueRead<FVolumetricCloudPassResult>{*Cloud.Completion};
			Snapshot->CloudColor = Read(*Cloud.Composite);
		}
		Snapshot->Color = Read(Base.Color);
		Snapshot->Depth = Read(Base.Depth, DepthRange);
		Snapshot->ColorOutput = {BackgroundColor, ColorRange, ERHIRenderTargetLoadAction::DontCare};
		Snapshot->DepthOutput = {BackgroundDepth, ColorRange, ERHIRenderTargetLoadAction::DontCare};
		Snapshot->Completion = {SnapshotDone};
		(void)Graph.AddRasterPass("Water.Snapshot", std::move(Snapshot),
			[&View, &Fullscreen, &Draw = *Resolved.WaterSnapshot](FRHICommandListImmediate& Commands,
				const FSnapshotParameters& P, const FRDGParameterResolver& R) {
				auto& Result = R.WriteValue(P.Completion);
				Result = R.ReadValue(P.Base);
				if (!Result.IsSuccess()) return;
				const auto* CloudResult = R.ReadValue(P.Cloud);
				Result.bUsesVolumetricCloudComposite = CloudResult && CloudResult->bCompositeOutputValid;
				auto* SceneColor = Result.bUsesVolumetricCloudComposite ? R.GetTexture(P.CloudColor) : R.GetTexture(P.Color);
				Fullscreen.DrawWaterSnapshot_RenderThread(Commands, View, Draw, SceneColor, R.GetTexture(P.Depth));
			});

		auto Depth = Graph.AllocParameters<FDepthParameters>();
		Depth->Snapshot = {SnapshotDone};
		Depth->Depth = {Base.Depth, DepthRange};
		Depth->Coverage = {Coverage, ColorRange, ERHIRenderTargetLoadAction::Clear};
		Depth->Completion = {DepthDone};
		(void)Graph.AddRasterPass("Water.NearestDepth", std::move(Depth),
			[&View, &DepthDraws](FRHICommandListImmediate& Commands, const FDepthParameters& P, const FRDGParameterResolver& R) {
				auto& Result = R.WriteValue(P.Completion);
				Result = R.ReadValue(P.Snapshot);
				if (!Result.IsSuccess()) return;
				SetWaterViewRect(Commands, View);
				FMeshDrawCommandStateCache Cache;
				for (const auto& Draw : DepthDraws.GetMeshDrawCommands())
					if (!Draw.SubmitDraw(Commands, &Cache)) { Result.Result = ERenderViewResult::RendererResourcesUnavailable; break; }
			});

		auto Shading = Graph.AllocParameters<FShadingParameters>();
		Shading->Nearest = {DepthDone};
		Shading->BackgroundColor = Read(BackgroundColor);
		Shading->BackgroundDepth = Read(BackgroundDepth);
		Shading->Coverage = Read(Coverage);
		// The accepted surface bindings already reference these textures.
		const auto ReadLighting = [&Read](std::optional<FRDGTextureHandle> Handle, const FRHITexture* Texture)
			-> std::optional<FRDGTextureAccess> {
			if (!Handle || !Texture) return std::nullopt;
			return Read(*Handle, {GetTextureAspects(Texture->GetFormat()), 0, Texture->GetNumMips(), 0, Texture->GetArraySize()});
		};
		Shading->Irradiance = ReadLighting(Environment.Irradiance, Environment.SelectedIrradiance);
		Shading->BrdfLut = ReadLighting(Environment.BrdfLut, Environment.SelectedBrdfLut);
		Shading->DirectionalShadow = ReadLighting(DirectionalShadow, Meshes.DirectionalShadowTexture);
		Shading->SceneDepth = {Base.Depth, DepthRange};
		Shading->Color = {Base.Color, ColorRange};
		if (Cloud.Completion && Cloud.Composite) Shading->CloudColor = FRDGColorAttachmentBinding{*Cloud.Composite, ColorRange};
		Shading->NormalRoughness = {NormalRoughness, ColorRange, ERHIRenderTargetLoadAction::Clear};
		Shading->SpecularF0 = {SpecularF0, ColorRange, ERHIRenderTargetLoadAction::Clear};
		Shading->Completion = {ShadingDone};
		(void)Graph.AddPass("Water.Shading", ERDGPassType::Graphics, std::move(Shading),
			[&View, &Draws, Readback](FRHICommandListImmediate& Commands, const FShadingParameters& P, const FRDGParameterResolver& R) {
				auto& Result = R.WriteValue(P.Completion);
				Result = R.ReadValue(P.Nearest);
				if (!Result.IsSuccess()) return;
				const auto Color = MakeRDGNativeAttachmentBinding(Result.bUsesVolumetricCloudComposite
					? R.GetColorAttachment(P.CloudColor) : R.GetColorAttachment(P.Color));
				const auto Normal = MakeRDGNativeAttachmentBinding(R.GetColorAttachment(P.NormalRoughness));
				const auto Specular = MakeRDGNativeAttachmentBinding(R.GetColorAttachment(P.SpecularF0));
				const auto Depth = MakeRDGNativeAttachmentBinding(R.GetDepthStencilAttachment(P.SceneDepth));
				FRHIRenderPassInfo Pass;
				Pass.RenderTargetLayout.NumColorRenderTargets = 3;
				Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget = Color.Layout;
				Pass.RenderTargetLayout.ColorAttachments[1].RenderTarget = Normal.Layout;
				Pass.RenderTargetLayout.ColorAttachments[2].RenderTarget = Specular.Layout;
				Pass.RenderTargetLayout.bHasDepthStencil = true;
				Pass.RenderTargetLayout.DepthStencilAttachment = Depth.Layout;
				Color.BindColor(Pass, 0);
				Normal.BindColor(Pass, 1);
				Specular.BindColor(Pass, 2);
				Depth.BindDepthStencil(Pass);
				Commands.BeginRenderPass(Pass, "Water.Shading");
				SetWaterViewRect(Commands, View);
				if (!DrawWaterSurfaces(Commands, View, Draws, R.GetTexture(P.BackgroundColor),
					R.GetTexture(P.BackgroundDepth), R.GetTexture(P.Coverage), Readback))
					Result.Result = ERenderViewResult::RendererResourcesUnavailable;
				Commands.EndRenderPass();
			});
		if (!Resolved.WaterReflection || !Environment.Sampler || Qualification.bDisableWaterReflections
			|| Readback != EWaterVolumeReadback::None)
			return ShadingDone;

		const auto Prefiltered = ReadLighting(Environment.Prefiltered, Environment.SelectedPrefiltered);
		const auto Brdf = ReadLighting(Environment.BrdfLut, Environment.SelectedBrdfLut);
		if (!Prefiltered || !Brdf) return ShadingDone;
		const auto ReflectionDone = Graph.CreateValue<FSceneColorPassResult>("Water.ReflectionResult", "scene-color-result");
		auto Reflection = Graph.AllocParameters<FReflectionParameters>();
		Reflection->Shading = {ShadingDone};
		Reflection->Coverage = Read(Coverage);
		Reflection->Depth = Read(Base.Depth, DepthRange);
		Reflection->NormalRoughness = Read(NormalRoughness);
		Reflection->SpecularF0 = Read(SpecularF0);
		Reflection->Environment = *Prefiltered;
		Reflection->BrdfLut = *Brdf;
		Reflection->Color = {Base.Color, ColorRange};
		if (Cloud.Completion && Cloud.Composite) Reflection->CloudColor = FRDGColorAttachmentBinding{*Cloud.Composite, ColorRange};
		Reflection->Completion = {ReflectionDone};
		(void)Graph.AddPass("Water.SkyReflection", ERDGPassType::Graphics, std::move(Reflection),
			[&View, &Fullscreen, &Draw = *Resolved.WaterReflection, Lighting = Resolved.Lighting.UniformBuffer,
				Sampler = Environment.Sampler](FRHICommandListImmediate& Commands, const FReflectionParameters& P, const FRDGParameterResolver& R) {
				auto& Result = R.WriteValue(P.Completion);
				Result = R.ReadValue(P.Shading);
				if (!Result.IsSuccess()) return;
				const auto Color = MakeRDGNativeAttachmentBinding(Result.bUsesVolumetricCloudComposite
					? R.GetColorAttachment(P.CloudColor) : R.GetColorAttachment(P.Color));
				FRHIRenderPassInfo Pass;
				Pass.RenderTargetLayout.NumColorRenderTargets = 1;
				Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget = Color.Layout;
				Color.BindColor(Pass, 0);
				Commands.BeginRenderPass(Pass, "Water.SkyReflection");
				if (!Fullscreen.DrawWaterReflection_RenderThread(Commands, View, Draw, {
					.Coverage = R.GetTexture(P.Coverage), .Depth = R.GetTexture(P.Depth),
					.NormalRoughness = R.GetTexture(P.NormalRoughness), .SpecularF0 = R.GetTexture(P.SpecularF0),
					.Environment = R.GetTexture(P.Environment), .BrdfLut = R.GetTexture(P.BrdfLut),
					.Sampler = Sampler, .Lighting = Lighting}))
					Result.Result = ERenderViewResult::RendererResourcesUnavailable;
				Commands.EndRenderPass();
			});
		return ReflectionDone;
	}
}

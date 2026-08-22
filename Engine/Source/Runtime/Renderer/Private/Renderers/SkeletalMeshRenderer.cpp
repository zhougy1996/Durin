#include "Renderers/SkeletalMeshRenderer.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"

namespace Durin
{
	using namespace RendererPrivate;

	struct FSkeletalMeshRenderer::FState
	{
		struct FBaseResources
		{
			std::unordered_map<size_t, TRenderResourceCreationSlot<FSamplerRHIRef>> MaterialSamplerCache;
		};
		struct FShaderMapPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkeletalMeshVertexShader> VertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			TShaderRef<FStaticMeshOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FStaticMeshShadowFragmentShader> ShadowFragmentShader;
		};
		struct FPipelinePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkeletalMeshVertexShader> VertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			TShaderRef<FStaticMeshOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FStaticMeshShadowFragmentShader> ShadowFragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		TRenderResourceCreationSlot<FBaseResources> BaseResources{
			ERenderResourceGenerationDependency::Device
		};
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderMapPayload>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderMapPayload>
			ShadowShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FEffectiveStaticMeshPipelineKey, FPipelinePayload> Pipelines{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
		TRendererResourceSlotCache<FEffectiveStaticMeshPipelineKey, FPipelinePayload> ShadowPipelines{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
	};
	auto PrepareSkeletalMeshView_RenderThread(
		const FRHICommandListImmediate& CommandList,
		std::span<const FPrimitiveSceneInfo* const> SceneInfos,
		const FSceneView& View,
		ERasterMode RasterMode,
		FPreparedSkeletalPaletteTable& PaletteTable,
		ERenderPreparationMode Mode
	) -> FPreparedSkeletalMeshView
	{
		check(IsInRenderingThread());
		checkf(!CommandList.IsInsideRenderPass(), "SkeletalMesh preparation must occur before the scene render pass.");
		FPreparedSkeletalMeshView Result;
		Result.Primitives.reserve(SceneInfos.size());
		for (const FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			++Result.VisibleCandidates;
			++Result.RequestedPaletteUploads;
			if (SceneInfo == nullptr)
			{
				++Result.RejectedPrimitives;
				continue;
			}
			check(SceneInfo->GetKind() == EPrimitiveSceneProxyKind::SkeletalMesh);
			const FSkeletalMeshSceneProxy& Proxy =
				SceneInfo->GetSkeletalMeshProxy();
			const FSkeletalMeshRenderData* RenderData = Proxy.GetRenderData();
			std::shared_ptr<const FSkeletalPosePalette> Pose;
			const FPrimitiveSceneId PrimitiveId = SceneInfo->GetId();
			if (const auto It = PaletteTable.PrimitiveToEntry.find(PrimitiveId);
				It != PaletteTable.PrimitiveToEntry.end())
			{
				Pose = PaletteTable.Entries[It->second].Pose;
			}
			else
			{
				Pose = Proxy.GetPose();
				const uint32 EntryIndex = static_cast<uint32>(PaletteTable.Entries.size());
				PaletteTable.PrimitiveToEntry.emplace(PrimitiveId, EntryIndex);
				PaletteTable.Entries.push_back({.Pose = Pose});
			}
			const FMatrix& LocalToWorld = SceneInfo->GetTransform();
			const bool bPoseComplete = Pose != nullptr && !Pose->Matrices.empty()
									   && RenderData != nullptr
									   && Pose->Matrices.size() == RenderData->PaletteBoneIndices.size()
									   && std::ranges::all_of(Pose->Matrices, [](const FMatrix4f& Matrix) { return Math::IsFinite(Matrix); });
			const uint64 PaletteBytes = Pose != nullptr ? Pose->Matrices.size() * sizeof(FMatrix4f) : 0;
			const FRHICapabilities* Capabilities = GDynamicRHI != nullptr ? GDynamicRHI->RHIGetCapabilities() : nullptr;
			if (RenderData == nullptr || !RenderData->IsReadyForRendering()
				|| !bPoseComplete || !Math::IsFinite(LocalToWorld)
				|| Capabilities == nullptr
				|| Capabilities->MinStorageBufferOffsetAlignment == 0
				|| PaletteBytes == 0
				|| PaletteBytes > Capabilities->MaxStorageBufferRange
				|| PaletteBytes > MaximumSkeletalPosePaletteBytes)
			{
				++Result.RejectedPrimitives;
				++Result.RejectedPalettes;
				continue;
			}
			const double Determinant = Math::LinearDeterminant(LocalToWorld);
			if (!std::isfinite(Determinant))
			{
				++Result.RejectedPrimitives;
				continue;
			}

			const uint32 PrimitiveIndex = static_cast<uint32>(Result.Primitives.size());
			Result.Primitives.push_back({.PrimitiveId = SceneInfo->GetId(), .RenderData = RenderData, .VertexFactory = &RenderData->VertexFactory, .Pose = Pose, .LocalToWorld = LocalToWorld});
			const size_t FirstSectionCount = Result.GetNumSections();
			const size_t FirstTriangleCount = Result.SelectedTriangles;
			const auto& Indices = RenderData->IndexBuffer.GetIndices();
			for (uint32 SectionIndex = 0;
				 SectionIndex < RenderData->Sections.size(); ++SectionIndex)
			{
				const FSkeletalMeshRenderSection& Section =
					RenderData->Sections[SectionIndex];
				if (Section.IndexCount == 0
					|| static_cast<uint64>(Section.FirstIndex) + Section.IndexCount
						   > Indices.size()) continue;
				FPreparedSkeletalMeshDraw Item;
				Item.Material = Proxy.ResolveMaterialRenderData_RenderThread(
					Section.MaterialSlotIndex
				);
				if (!ResolveMaterialBinding(
						Item.Material,
						Item.MaterialBinding,
						"SkeletalMeshMaterialBinding")) continue;
				Item.PrimitiveIndex = PrimitiveIndex;
				Item.SectionIndex = SectionIndex;
				Item.Section = &Section;
				Item.ShaderMapIdentity = Item.Material.PipelineIdentity.ShaderMap;
				Item.PipelineKey.Material = Item.Material.PipelineIdentity;
				Item.PipelineKey.VertexDomain = EVertexDeformationDomain::Skeletal;
				Item.PipelineKey.Rasterizer.PolygonMode =
					RasterMode == ERasterMode::Wireframe ? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
				Item.PipelineKey.Rasterizer.CullMode =
					Item.Material.PipelineIdentity.bTwoSided ? ERHICullMode::None : ERHICullMode::Back;
				Item.PipelineKey.Rasterizer.FrontFace = Determinant < 0.0 ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
				Item.PipelineKey.Depth.bEnableTest = true;
				Item.PipelineKey.Depth.CompareOp =
					View.DepthConvention == ESceneDepthConvention::ReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::Less;
				const EMaterialBlendMode BlendMode =
					Item.Material.PipelineIdentity.ShaderMap.BlendMode;
				Item.Pass = BlendMode == EMaterialBlendMode::Masked ? EStaticMeshBasePass::Masked : BlendMode == EMaterialBlendMode::Translucent ? EStaticMeshBasePass::Translucent :
																																				   EStaticMeshBasePass::Opaque;
				if (Mode == ERenderPreparationMode::ShadowDepth
					&& Item.Pass == EStaticMeshBasePass::Translucent)
					continue;
				const auto DepthPolicy = Item.Material.PipelineIdentity.DepthWritePolicy;
				Item.PipelineKey.Depth.bEnableWrite =
					DepthPolicy == EMaterialDepthWritePolicy::Enabled
					|| (DepthPolicy == EMaterialDepthWritePolicy::Automatic
						&& Item.Pass != EStaticMeshBasePass::Translucent);
				if (Item.Pass == EStaticMeshBasePass::Translucent)
					Item.PipelineKey.ColorBlend = FRHIColorBlendState::StraightAlpha();
				const FVector4 Center = LocalToWorld
										* FVector4(Section.LocalBounds.GetCenter(), 1.0);
				if (!Math::IsFinite(Center)) continue;
				Item.SortCenter = FVector3(Center);
				const FVector3 Offset = Item.SortCenter - View.ViewLocation;
				Item.TranslucentDistanceSquared = Math::Dot(Offset, Offset);
				if (!std::isfinite(Item.TranslucentDistanceSquared)) continue;
				Item.bCastsShadow = Item.Pass != EStaticMeshBasePass::Translucent;
				Item.SortKey = MakeSkeletalMeshDrawSortKey(
					Result.Primitives[PrimitiveIndex], Item
				);
				auto* Bucket = Item.Pass == EStaticMeshBasePass::Opaque ? &Result.Opaque : Item.Pass == EStaticMeshBasePass::Masked ? &Result.Masked :
																																	  &Result.Translucent;
				Bucket->push_back(std::move(Item));
				Result.SelectedTriangles += Section.IndexCount / 3;
				if (BlendMode == EMaterialBlendMode::Masked)
				{
					++Result.MaskedSections;
					Result.MaskedTriangles += Section.IndexCount / 3;
				}
				else if (BlendMode == EMaterialBlendMode::Translucent)
				{
					++Result.TranslucentSections;
					Result.TranslucentTriangles += Section.IndexCount / 3;
				}
				else
				{
					++Result.OpaqueSections;
					Result.OpaqueTriangles += Section.IndexCount / 3;
				}
			}
			const size_t PreparedSections = Result.GetNumSections() - FirstSectionCount;
			if (PreparedSections == 0)
			{
				Result.Primitives.pop_back();
				Result.SelectedTriangles = FirstTriangleCount;
				++Result.RejectedPrimitives;
				continue;
			}
			Result.SelectedSections += PreparedSections;
		}
		const auto SortingStart = std::chrono::steady_clock::now();
		auto StateSort = [](const FPreparedSkeletalMeshDraw& A,
							const FPreparedSkeletalMeshDraw& B) {
			return A.SortKey < B.SortKey;
		};
		std::ranges::sort(Result.Opaque, StateSort);
		std::ranges::sort(Result.Masked, StateSort);
		std::ranges::sort(Result.Translucent, [](const FPreparedSkeletalMeshDraw& A, const FPreparedSkeletalMeshDraw& B) {
			if (A.TranslucentDistanceSquared != B.TranslucentDistanceSquared)
				return A.TranslucentDistanceSquared > B.TranslucentDistanceSquared;
			return A.SortKey < B.SortKey;
		});
		Result.SortingNanoseconds = static_cast<uint64>(std::chrono::duration_cast<
															std::chrono::nanoseconds>(
															std::chrono::steady_clock::now() - SortingStart
		)
															.count());
		auto CountStateFacts = [&Result](const auto& Bucket) -> size_t {
			if (Bucket.empty()) return 0;
			size_t Groups = 1;
			for (size_t Index = 1; Index < Bucket.size(); ++Index)
			{
				const auto& Previous = Bucket[Index - 1].SortKey;
				const auto& Current = Bucket[Index].SortKey;
				const bool bPipeline = Previous.Pipeline != Current.Pipeline;
				const bool bMaterial =
					Previous.MaterialUniform != Current.MaterialUniform;
				const bool bVertexFactory =
					Previous.VertexFactory != Current.VertexFactory;
				const bool bGeometry = Previous.Geometry != Current.Geometry
									   || Previous.PrimitiveId != Current.PrimitiveId;
				Result.PipelineTransitions += bPipeline ? 1u : 0u;
				Result.MaterialTransitions += bMaterial ? 1u : 0u;
				Result.VertexFactoryTransitions += bVertexFactory ? 1u : 0u;
				Result.GeometryTransitions += bGeometry ? 1u : 0u;
				Groups += bPipeline || bMaterial || bVertexFactory ? 1u : 0u;
			}
			return Groups;
		};
		Result.OpaqueStateGroups = CountStateFacts(Result.Opaque);
		Result.MaskedStateGroups = CountStateFacts(Result.Masked);
		CountStateFacts(Result.Translucent);
		check(Result.VisibleCandidates == Result.Primitives.size() + Result.RejectedPrimitives);
		check(Result.SelectedSections == Result.GetNumSections());
		check(Result.SelectedTriangles == Result.OpaqueTriangles + Result.MaskedTriangles + Result.TranslucentTriangles);
		return Result;
	}
	FSkeletalMeshRenderer::FSkeletalMeshRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FDefaultTextureResources& InDefaultTextures,
		FEnvironmentLightingResources& InEnvironmentLighting
	)
		: Coordinator(InCoordinator)
		, DefaultTextures(InDefaultTextures)
		, EnvironmentLighting(InEnvironmentLighting)
		, State(std::make_unique<FState>())
	{
	}

	FSkeletalMeshRenderer::~FSkeletalMeshRenderer() = default;

	auto FSkeletalMeshRenderer::EnsureBaseResources_RenderThread() -> bool
	{
		using FResult = TRenderResourceCreateResult<FState::FBaseResources>;
		return State->BaseResources.Resolve(
				   Coordinator.GetGeneration_RenderThread(),
				   []() -> FResult { return FResult::Success(FState::FBaseResources{}); },
				   ReportRendererResourceCreateDiagnostic
			   )
			   != nullptr;
	}

	auto FSkeletalMeshRenderer::EnsureMaterialSamplers_RenderThread(
		const FPreparedSkeletalMeshDraw& Item
	) -> bool
	{
		FState::FBaseResources* Base = State->BaseResources.GetPayload();
		if (Base == nullptr)
			return false;
		for (const FMaterialSamplerState& SamplerState :
			 Item.MaterialBinding.Samplers)
		{
			auto Entry = Base->MaterialSamplerCache.try_emplace(
													   GetMaterialSamplerKey(SamplerState),
													   ERenderResourceGenerationDependency::Device
			)
							 .first;
			if (Entry->second.Resolve(
					Coordinator.GetGeneration_RenderThread(),
					[SamplerState] {
						return CreateMaterialSampler(
							SamplerState, "SkeletalMeshMaterialSampler"
						);
					},
					ReportRendererResourceCreateDiagnostic
				)
				== nullptr)
				return false;
		}
		return true;
	}

	auto FSkeletalMeshRenderer::EnsureSectionResources_RenderThread(
		const FPreparedSkeletalMeshPrimitive& Primitive,
		const FPreparedSkeletalMeshDraw& Item,
		bool bShadowDepth,
		bool bHybridRetained
	) -> bool
	{
		FState::FBaseResources* Base = State->BaseResources.GetPayload();
		if (Base == nullptr || Primitive.VertexFactory == nullptr) return false;
		const FMaterialRenderData& Material = Item.Material;
		using FShaderResult = TRenderResourceCreateResult<FState::FShaderMapPayload>;
		auto& ShaderMapCache = bShadowDepth ? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderEntry = ShaderMapCache.FindOrAdd(
			Material.PipelineIdentity.ShaderMap
		);
		FState::FShaderMapPayload* ShaderPayload = ShaderEntry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &Material, bShadowDepth]() -> FShaderResult {
				const FMaterialShaderMapIdentity& Identity =
					Material.PipelineIdentity.ShaderMap;
				FShaderCompileOptions Options;
				Options.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				Options.Macros.emplace_back("DURIN_SKELETAL_MESH", "1");
				Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", std::to_string(static_cast<uint8>(Identity.BlendMode)));
				Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", std::to_string(static_cast<uint8>(Identity.ShadingModel)));
				Options.Macros.emplace_back(
					"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
					std::to_string(std::bit_cast<uint32>(
						Identity.OpacityMaskThreshold
					))
				);
				if (bShadowDepth
					&& Identity.BlendMode != EMaterialBlendMode::Masked)
				{
					Options.Macros.emplace_back(
						"DURIN_OPAQUE_SHADOW_DEPTH", "1"
					);
				}
				FShaderType& VertexType = FSkeletalMeshVertexShader::StaticType();
				FShaderType& FragmentType = FStaticMeshFragmentShader::StaticType();
				FShaderType& ShadowFragmentType =
					FStaticMeshShadowFragmentShader::StaticType();
				FShaderType& OpaqueShadowFragmentType =
					FStaticMeshOpaqueShadowFragmentShader::StaticType();
				std::vector<const FShaderType*> Types{&VertexType};
				if (!bShadowDepth)
					Types.push_back(&FragmentType);
				else if (Identity.BlendMode == EMaterialBlendMode::Masked)
					Types.push_back(&ShadowFragmentType);
				else
					Types.push_back(&OpaqueShadowFragmentType);
				auto ShaderMap = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						std::move(Error),
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual
					));
				auto* Vertex = static_cast<FSkeletalMeshVertexShader*>(
					ShaderMap->GetShader(&VertexType)
				);
				auto* Fragment = !bShadowDepth ? static_cast<FStaticMeshFragmentShader*>(
													 ShaderMap->GetShader(&FragmentType)
												 ) :
												 nullptr;
				auto* ShadowFragment = bShadowDepth
											   && Identity.BlendMode == EMaterialBlendMode::Masked ?
										   static_cast<FStaticMeshShadowFragmentShader*>(
											   ShaderMap->GetShader(&ShadowFragmentType)
										   ) :
										   nullptr;
				auto* OpaqueShadowFragment =
					bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked ? static_cast<FStaticMeshOpaqueShadowFragmentShader*>(
																						   ShaderMap->GetShader(&OpaqueShadowFragmentType)
																					   ) :
																					   nullptr;
				if (Vertex == nullptr || (!bShadowDepth && Fragment == nullptr))
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						"Compiled map did not contain both typed shaders.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual
					));
				if (bShadowDepth && Identity.BlendMode == EMaterialBlendMode::Masked
					&& ShadowFragment == nullptr)
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						"Compiled masked shader map did not contain the shadow fragment shader.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual
					));
				if (bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked
					&& OpaqueShadowFragment == nullptr)
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						"Compiled shader map did not contain the opaque shadow fragment shader.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual
					));
				FState::FShaderMapPayload Candidate;
				Candidate.ShaderMap = std::move(ShaderMap);
				Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
				if (Fragment != nullptr)
					Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
				if (ShadowFragment != nullptr)
					Candidate.ShadowFragmentShader = {
						ShadowFragment, Candidate.ShaderMap.get()
					};
				if (OpaqueShadowFragment != nullptr)
					Candidate.OpaqueShadowFragmentShader = {
						OpaqueShadowFragment, Candidate.ShaderMap.get()
					};
				if (Candidate.VertexShader.GetRHIShader(false) == nullptr
					|| (!bShadowDepth
						&& Candidate.FragmentShader.GetRHIShader(false) == nullptr)
					|| (bShadowDepth
						&& Identity.BlendMode == EMaterialBlendMode::Masked
						&& Candidate.ShadowFragmentShader.GetRHIShader(false) == nullptr)
					|| (bShadowDepth
						&& Identity.BlendMode != EMaterialBlendMode::Masked
						&& Candidate.OpaqueShadowFragmentShader.GetRHIShader(false) == nullptr))
					return FShaderResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"SkeletalMeshShaderMap", GetIdentityText(Identity),
						"RHI shader creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
					));
				return FShaderResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic
		);
		if (ShaderPayload == nullptr) return false;

		using FPipelineResult = TRenderResourceCreateResult<FState::FPipelinePayload>;
		FEffectiveStaticMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey) : Item.PipelineKey;
		EffectivePipelineKey.bHybridRetained =
			!bShadowDepth && bHybridRetained;
		auto& PipelineCache = bShadowDepth ? State->ShadowPipelines : State->Pipelines;
		auto& PipelineEntry = PipelineCache.FindOrAdd(EffectivePipelineKey);
		FRenderResourceGeneration Generation =
			Coordinator.GetGeneration_RenderThread();
		Generation.Shader = ShaderEntry.Slot.GetPayloadGeneration().Shader;
		FState::FPipelinePayload* Pipeline = PipelineEntry.Slot.Resolve(
			Generation,
			[&EffectivePipelineKey, &PipelineEntry, ShaderPayload,
			 bShadowDepth,
			 VertexFactory = Primitive.VertexFactory]() -> FPipelineResult {
				FState::FPipelinePayload Candidate;
				Candidate.ShaderMap = ShaderPayload->ShaderMap;
				Candidate.VertexShader = ShaderPayload->VertexShader;
				Candidate.FragmentShader = ShaderPayload->FragmentShader;
				Candidate.ShadowFragmentShader =
					ShaderPayload->ShadowFragmentShader;
				Candidate.OpaqueShadowFragmentShader =
					ShaderPayload->OpaqueShadowFragmentShader;
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = bShadowDepth ? RenderTargetLayouts::MakeDirectionalShadowDepth() : (EffectivePipelineKey.bHybridRetained ? RenderTargetLayouts::MakeHybridRetainedForward() : RenderTargetLayouts::MakeSceneTargets());
				Initializer.BoundShaders.VertexShader =
					Candidate.VertexShader.GetRHIShader();
				Initializer.BoundShaders.FragmentShader = bShadowDepth ? (EffectivePipelineKey.Material.ShaderMap.BlendMode
																				  == EMaterialBlendMode::Masked ?
																			  Candidate.ShadowFragmentShader.GetRHIShader() :
																			  Candidate.OpaqueShadowFragmentShader.GetRHIShader()) :
																		 Candidate.FragmentShader.GetRHIShader();
				Initializer.VertexDeclaration = VertexFactory->GetDeclaration();
				Initializer.RasterizerState = EffectivePipelineKey.Rasterizer;
				Initializer.DepthStencilState = EffectivePipelineKey.Depth;
				if (!bShadowDepth)
				{
					Initializer.ColorBlendStates[0] = EffectivePipelineKey.ColorBlend;
					if (EffectivePipelineKey.Material.ShaderMap.BlendMode
						== EMaterialBlendMode::Translucent)
					{
						Initializer.ColorBlendStates[1].ColorWriteMask =
							ERHIColorWriteMask::None;
					}
				}
				Initializer.PipelineLayout =
					Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
					FName(std::format("SkeletalMeshPipeline_{}", PipelineEntry.Index)),
					Initializer
				);
				if (Candidate.PipelineState == nullptr)
					return FPipelineResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"SkeletalMeshPipeline", GetIdentityText(EffectivePipelineKey),
						"Graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
					));
				return FPipelineResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic
		);
		if (Pipeline == nullptr) return false;

		return EnsureMaterialSamplers_RenderThread(Item);
	}

	auto FSkeletalMeshRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSkeletalPaletteTable& PaletteTable,
		FPreparedSkeletalMeshView& PreparedView,
		bool bPrepareLitOpaqueForward
	) -> bool
	{
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::Prepared);
		EnsureBaseResources_RenderThread();
		for (FPreparedSkeletalMeshPrimitive& Primitive : PreparedView.Primitives)
		{
			switch (ResolveSkeletalPalette_RenderThread(
				CommandList, PaletteTable, Primitive
			))
			{
			case ESkeletalPaletteResolveResult::Uploaded:
				++PreparedView.UploadedPalettes;
				PreparedView.UploadedPaletteMatrices += Primitive.Pose->Matrices.size();
				PreparedView.UploadedPaletteBytes += Primitive.PaletteRange.Size;
				break;
			case ESkeletalPaletteResolveResult::Reused:
				++PreparedView.ReusedPalettes;
				break;
			case ESkeletalPaletteResolveResult::Rejected:
				++PreparedView.RejectedPalettes;
				break;
			}
		}
		ForEachBasePassBucket(PreparedView, [&](auto& Bucket, EStaticMeshBasePass Pass) {
			for (FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++PreparedView.ResourcePreparationAttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const bool bNeedsForwardPipeline =
					Pass == EStaticMeshBasePass::Translucent
					|| bPrepareLitOpaqueForward
					|| Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
						   != EMaterialShadingModel::Lit;
				Draw.bResourcesReady = Primitive != nullptr
									   && Primitive->PaletteRange.Buffer != nullptr
									   && (bNeedsForwardPipeline ? EnsureSectionResources_RenderThread(*Primitive, Draw) : EnsureMaterialSamplers_RenderThread(Draw));
				if (Draw.bResourcesReady)
					++PreparedView.ResourcePreparationSuccessfulDraws;
			}
		});
		check(PreparedView.RequestedPaletteUploads == PreparedView.UploadedPalettes + PreparedView.ReusedPalettes + PreparedView.RejectedPalettes);
		check(PreparedView.UploadedPaletteBytes == PreparedView.UploadedPaletteMatrices * sizeof(FMatrix4f));
		return FinalizeResourcePreparation(
			PreparedView, EPreparedSkeletalMeshPhase::ResourcesPrepared
		);
	}

	auto FSkeletalMeshRenderer::PrepareHybridRetainedResources_RenderThread(
		FPreparedSkeletalMeshView& PreparedView
	) -> bool
	{
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		bool bReady = true;
		ForEachBasePassBucket(PreparedView, [this, &PreparedView, &bReady](const auto& Bucket, EStaticMeshBasePass Pass) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				if (Pass != EStaticMeshBasePass::Translucent
					&& Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
						   == EMaterialShadingModel::Lit)
					continue;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				bReady = Primitive != nullptr
						 && EnsureSectionResources_RenderThread(
							 *Primitive, Draw, false, true
						 )
						 && bReady;
			}
		});
		return bReady;
	}

	auto FSkeletalMeshRenderer::PrepareShadowResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSkeletalPaletteTable& PaletteTable,
		FPreparedSkeletalMeshView& PreparedView
	) -> bool
	{
		check(!CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::Prepared);
		check(PreparedView.Translucent.empty());
		EnsureBaseResources_RenderThread();
		for (FPreparedSkeletalMeshPrimitive& Primitive : PreparedView.Primitives)
		{
			switch (ResolveSkeletalPalette_RenderThread(
				CommandList, PaletteTable, Primitive
			))
			{
			case ESkeletalPaletteResolveResult::Uploaded:
				++PreparedView.UploadedPalettes;
				PreparedView.UploadedPaletteMatrices += Primitive.Pose->Matrices.size();
				PreparedView.UploadedPaletteBytes += Primitive.PaletteRange.Size;
				break;
			case ESkeletalPaletteResolveResult::Reused:
				++PreparedView.ReusedPalettes;
				break;
			case ESkeletalPaletteResolveResult::Rejected:
				++PreparedView.RejectedPalettes;
				break;
			}
		}
		ForEachShadowBucket(PreparedView, [this, &PreparedView](auto& Bucket) {
			for (FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++PreparedView.ResourcePreparationAttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				Draw.bResourcesReady = Primitive != nullptr
									   && Primitive->PaletteRange.Buffer != nullptr
									   && EnsureSectionResources_RenderThread(*Primitive, Draw, true);
				PreparedView.ResourcePreparationSuccessfulDraws +=
					Draw.bResourcesReady ? 1u : 0u;
			}
		});
		return FinalizeResourcePreparation(
			PreparedView, EPreparedSkeletalMeshPhase::ResourcesPrepared
		);
	}

	auto FSkeletalMeshRenderer::ExecuteShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& ShadowView,
		const FRHIUniformBufferRange& FallbackLighting,
		FPreparedSkeletalMeshView& PreparedView
	) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		ForEachShadowBucket(PreparedView, [this, &CommandList, &ShadowView, &FallbackLighting, &PreparedView](const auto& Bucket) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++PreparedView.AttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				if (Primitive != nullptr && Draw.bResourcesReady
					&& DrawSection_RenderThread(
						CommandList, ShadowView, FallbackLighting,
						ERenderMode::Unlit, *Primitive, Draw, true
					))
					++PreparedView.SuccessfulDraws;
				else
					++PreparedView.RejectedDraws;
			}
		});
		FinalizeExecution(
			PreparedView, EPreparedSkeletalMeshPhase::Executed, false
		);
	}

	auto FSkeletalMeshRenderer::Execute_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, FPreparedSkeletalMeshView& PreparedView
	) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
		{
			PreparedView.Phase = EPreparedSkeletalMeshPhase::Executed;
			return;
		}
		ForEachBasePassBucket(PreparedView, [&](const auto& Bucket, EStaticMeshBasePass Pass) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++PreparedView.AttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const bool bComplete = Primitive != nullptr
									   && Primitive->PrimitiveId != InvalidPrimitiveSceneId
									   && Primitive->RenderData != nullptr
									   && Primitive->VertexFactory != nullptr
									   && Primitive->Pose != nullptr
									   && Primitive->PaletteRange.Buffer != nullptr
									   && Draw.Section != nullptr && Draw.Pass == Pass
									   && Draw.SortKey.Pipeline[0] == static_cast<uint32>(Pass)
									   && Draw.ShaderMapIdentity
											  == Draw.Material.PipelineIdentity.ShaderMap;
				if (!bComplete || !Draw.bResourcesReady)
				{
					++PreparedView.RejectedDraws;
					continue;
				}
				if (DrawSection_RenderThread(CommandList, View, Lighting, RenderMode, *Primitive, Draw))
					++PreparedView.SuccessfulDraws;
				else
					++PreparedView.RejectedDraws;
			}
		});
		FinalizeExecution(PreparedView, EPreparedSkeletalMeshPhase::Executed);
	}

	auto FSkeletalMeshRenderer::ExecutePreparedDraw_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EStaticMeshBasePass Pass, const FPreparedSkeletalMeshDraw& Draw, FPreparedSkeletalMeshView& PreparedView, bool bHybridRetained
	) -> void
	{
		++PreparedView.AttemptedDraws;
		const FPreparedSkeletalMeshPrimitive* Primitive =
			PreparedView.GetPrimitive(Draw);
		const bool bComplete = Primitive != nullptr
							   && Primitive->PrimitiveId != InvalidPrimitiveSceneId
							   && Primitive->RenderData != nullptr
							   && Primitive->VertexFactory != nullptr && Primitive->Pose != nullptr
							   && Primitive->PaletteRange.Buffer != nullptr
							   && Draw.Section != nullptr && Draw.Pass == Pass
							   && Draw.SortKey.Pipeline[0] == static_cast<uint32>(Pass)
							   && Draw.ShaderMapIdentity == Draw.Material.PipelineIdentity.ShaderMap;
		if (!bComplete || !Draw.bResourcesReady)
		{
			++PreparedView.RejectedDraws;
			return;
		}
		if (DrawSection_RenderThread(CommandList, View, Lighting, RenderMode, *Primitive, Draw, false, bHybridRetained))
			++PreparedView.SuccessfulDraws;
		else
			++PreparedView.RejectedDraws;
	}

	auto FSkeletalMeshRenderer::ExecutePass_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EStaticMeshBasePass Pass, FPreparedSkeletalMeshView& PreparedView
	) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
			return;
		const auto& Bucket = GetBasePassBucket(PreparedView, Pass);
		for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			ExecutePreparedDraw_RenderThread(CommandList, View, Lighting, RenderMode, Pass, Draw, PreparedView);
	}

	auto FSkeletalMeshRenderer::FinalizeExecution_RenderThread(
		FPreparedSkeletalMeshView& PreparedView
	) -> void
	{
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		FinalizeExecution(PreparedView, EPreparedSkeletalMeshPhase::Executed);
	}

	auto FSkeletalMeshRenderer::ExecuteGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FGBufferRenderer& GBuffer,
		FPreparedSkeletalMeshView& PreparedView
	) -> void
	{
		check(CommandList.IsInsideRenderPass());
		check(PreparedView.Phase == EPreparedSkeletalMeshPhase::ResourcesPrepared);
		PreparedView.GBufferSkippedDraws += PreparedView.Translucent.size();
		ForEachShadowBucket(PreparedView, [this, &CommandList, &View, &GBuffer, &PreparedView](const auto& Bucket) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					++PreparedView.GBufferSkippedDraws;
					continue;
				}
				++PreparedView.GBufferAttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				if (Primitive != nullptr && Draw.bResourcesReady
					&& DrawGBufferSection_RenderThread(
						CommandList, View, GBuffer, *Primitive, Draw
					))
				{
					++PreparedView.GBufferSuccessfulDraws;
				}
				else
				{
					++PreparedView.GBufferRejectedDraws;
				}
			}
		});
		check(PreparedView.GBufferAttemptedDraws == PreparedView.GBufferSuccessfulDraws + PreparedView.GBufferRejectedDraws);
	}

	auto FSkeletalMeshRenderer::DrawGBufferSection_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FGBufferRenderer& GBuffer,
		const FPreparedSkeletalMeshPrimitive& Primitive,
		const FPreparedSkeletalMeshDraw& Item
	) -> bool
	{
		if (Primitive.RenderData == nullptr || Primitive.VertexFactory == nullptr
			|| Primitive.Pose == nullptr || Item.Section == nullptr)
		{
			return false;
		}
		FState::FBaseResources* Base = State->BaseResources.GetPayload();
		if (Base == nullptr) return false;
		const FVertexDeclarationRHIRef VertexDeclaration(
			Primitive.VertexFactory->GetDeclaration()
		);
		FGBufferRenderer::FPipeline* Pipeline =
			GBuffer.EnsurePipeline_RenderThread({.Material = Item.PipelineKey.Material, .Rasterizer = Item.PipelineKey.Rasterizer, .Depth = Item.PipelineKey.Depth, .VertexDeclaration = VertexDeclaration, .VertexDomain = EGBufferVertexDomain::Skeletal});
		if (Pipeline == nullptr) return false;

		FStaticMeshTransformUniform Transform;
		Transform.LocalToClip = Math::TransposeToFloat(
			View.ViewProjectionMatrix * Primitive.LocalToWorld
		);
		Transform.LocalToWorld = Math::TransposeToFloat(Primitive.LocalToWorld);
		Transform.NormalToWorld = Math::TransposeToFloat(
			Math::Transpose(Math::Inverse(Primitive.LocalToWorld))
		);
		Transform.TransformParams.x = Math::LinearDeterminant(
			FMatrix4f(Primitive.LocalToWorld)) < 0.0f ?
										  -1.0f :
										  1.0f;
		Transform.TransformParams.y = static_cast<float>(
			Primitive.Pose->Matrices.size()
		);
		const FRHIUniformBufferRange TransformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Transform, sizeof(Transform));
		const FStaticMeshMaterialUniform MaterialUniform =
			MakeStaticMeshMaterialUniform(Item.MaterialBinding, true);
		const FRHIUniformBufferRange MaterialBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&MaterialUniform, sizeof(MaterialUniform)
			);
		std::array<FRHISampler*, 8> Samplers{};
		for (size_t Role = 0; Role < Samplers.size(); ++Role)
		{
			const auto It = Base->MaterialSamplerCache.find(
				GetMaterialSamplerKey(Item.MaterialBinding.Samplers[Role])
			);
			if (It == Base->MaterialSamplerCache.end()) return false;
			FSamplerRHIRef* Sampler = It->second.GetPayload();
			if (Sampler == nullptr) return false;
			Samplers[Role] = Sampler->GetReference();
		}
		const FGBufferRenderer::FVertexParameters VertexParameters{
			.Transform = TransformBuffer,
			.SkinPalette = Primitive.PaletteRange
		};
		const FGBufferRenderer::FFragmentParameters FragmentParameters =
			MakeGBufferFragmentParameters(Item.MaterialBinding, DefaultTextures, MaterialBuffer, Samplers);
		if (!GBuffer.BindPipeline_RenderThread(
				CommandList, *Pipeline, VertexParameters, FragmentParameters
			))
		{
			return false;
		}
		Primitive.VertexFactory->BindStreams(CommandList);
		CommandList.BindIndexBuffer(Primitive.RenderData->IndexBuffer.GetRHI(), 0);
		CommandList.DrawIndexed(
			Item.Section->IndexCount, Item.Section->FirstIndex, 0
		);
		return true;
	}

	auto FSkeletalMeshRenderer::DrawSection_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedSkeletalMeshPrimitive& Primitive, const FPreparedSkeletalMeshDraw& Item, bool bShadowDepth, bool bHybridRetained
	) -> bool
	{
		const FSkeletalMeshRenderData& Data = *Primitive.RenderData;
		const FSkeletalMeshRenderSection& Section = *Item.Section;
		const FMatrix& LocalToWorld = Primitive.LocalToWorld;
		const FMaterialRenderData& Material = Item.Material;
		const FMaterialRenderBinding& Binding = Item.MaterialBinding;
		FStaticMeshTransformUniform Transform;
		Transform.LocalToClip = Math::TransposeToFloat(View.ViewProjectionMatrix * LocalToWorld);
		Transform.LocalToWorld = Math::TransposeToFloat(LocalToWorld);
		Transform.NormalToWorld = Math::TransposeToFloat(
			Math::Transpose(Math::Inverse(LocalToWorld))
		);
		Transform.TransformParams.x = Math::LinearDeterminant(
			FMatrix4f(LocalToWorld)) < 0.0f ?
										  -1.0f :
										  1.0f;
		Transform.TransformParams.y = static_cast<float>(
			Primitive.Pose->Matrices.size()
		);
		const FRHIUniformBufferRange TransformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Transform, sizeof(Transform));

		Primitive.VertexFactory->BindStreams(CommandList);
		CommandList.BindIndexBuffer(Data.IndexBuffer.GetRHI(), 0);
		FState::FBaseResources* Base = State->BaseResources.GetPayload();
		FEffectiveStaticMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey) : Item.PipelineKey;
		EffectivePipelineKey.bHybridRetained =
			!bShadowDepth && bHybridRetained;
		auto* PipelineEntry = bShadowDepth ? State->ShadowPipelines.Find(EffectivePipelineKey) : State->Pipelines.Find(EffectivePipelineKey);
		FState::FPipelinePayload* Pipeline = PipelineEntry != nullptr ? PipelineEntry->Slot.GetPayload() : nullptr;
		if (Base == nullptr || Pipeline == nullptr) return false;
		CommandList.SetGraphicsPipelineState(*Pipeline->PipelineState);
		if (bShadowDepth)
		{
			const FRHIRasterizerState Rasterizer =
				MakeShadowRasterizerState(Item.PipelineKey.Rasterizer);
			CommandList.SetDepthBias(Rasterizer.DepthBiasConstantFactor, Rasterizer.DepthBiasClamp, Rasterizer.DepthBiasSlopeFactor);
		}
		FSkeletalMeshVertexShader::FParameters VertexParameters;
		VertexParameters.Transform = TransformBuffer;
		VertexParameters.SkinPalette = Primitive.PaletteRange;
		SetShaderParameters(CommandList, Pipeline->VertexShader, VertexParameters);
		if (bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				   != EMaterialBlendMode::Masked)
		{
			CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
			return true;
		}
		FStaticMeshMaterialUniform MaterialUniform;
		MaterialUniform.BaseColor = Binding.BaseColor;
		MaterialUniform.EmissiveMetallic = FVector4f(
			Binding.Emissive, Binding.Metallic
		);
		MaterialUniform.NormalRoughness = FVector4f(
			Binding.Normal, Binding.Roughness
		);
		MaterialUniform.SurfaceParams = FVector4f(
			Binding.AmbientOcclusion, Binding.OpacityMask,
			RenderMode == ERenderMode::Lit
					&& Material.PipelineIdentity.ShaderMap.ShadingModel
						   == EMaterialShadingModel::Lit ?
				1.0f :
				0.0f,
			0.0f
		);
		for (size_t Role = 0; Role < Binding.Textures.size(); ++Role)
			MaterialUniform.UVTransforms[Role] = FVector4f(
				Binding.UVScales[Role].x, Binding.UVScales[Role].y,
				Binding.UVOffsets[Role].x, Binding.UVOffsets[Role].y
			);
		MaterialUniform.UVChannels0 = FVector4f(Binding.UVChannels[0], Binding.UVChannels[1], Binding.UVChannels[2], Binding.UVChannels[3]);
		MaterialUniform.UVChannels1 = FVector4f(Binding.UVChannels[4], Binding.UVChannels[5], Binding.UVChannels[6], Binding.UVChannels[7]);
		MaterialUniform.UVRotations0 = FVector4f(Binding.UVRotations[0], Binding.UVRotations[1], Binding.UVRotations[2], Binding.UVRotations[3]);
		MaterialUniform.UVRotations1 = FVector4f(Binding.UVRotations[4], Binding.UVRotations[5], Binding.UVRotations[6], Binding.UVRotations[7]);
		const FRHIUniformBufferRange MaterialBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&MaterialUniform, sizeof(MaterialUniform)
			);
		FStaticMeshFragmentShader::FParameters FragmentParameters;
		FragmentParameters.Lighting = Lighting;
		FragmentParameters.Material = MaterialBuffer;
		auto ResolveTexture = [&](size_t Role, EDefaultTexture Fallback) {
			FRHITexture* Texture = Binding.Textures[Role] != nullptr ? Binding.Textures[Role]->GetReferencedTexture_RenderThread() : nullptr;
			return Texture != nullptr ? Texture : DefaultTextures.Get_RenderThread(Fallback);
		};
		FragmentParameters.BaseColorTexture = ResolveTexture(0, EDefaultTexture::White);
		FragmentParameters.NormalTexture = ResolveTexture(1, EDefaultTexture::FlatNormal);
		FragmentParameters.MetallicTexture = ResolveTexture(2, EDefaultTexture::White);
		FragmentParameters.RoughnessTexture = ResolveTexture(3, EDefaultTexture::White);
		FragmentParameters.AmbientOcclusionTexture = ResolveTexture(4, EDefaultTexture::White);
		FragmentParameters.EmissiveTexture = ResolveTexture(5, EDefaultTexture::Black);
		FragmentParameters.OpacityTexture = ResolveTexture(6, EDefaultTexture::White);
		FragmentParameters.OpacityMaskTexture = ResolveTexture(7, EDefaultTexture::White);
		std::array<FRHISampler*, 8> Samplers{};
		for (size_t Role = 0; Role < Samplers.size(); ++Role)
		{
			const auto It = Base->MaterialSamplerCache.find(
				GetMaterialSamplerKey(Binding.Samplers[Role])
			);
			if (It == Base->MaterialSamplerCache.end()) return false;
			FSamplerRHIRef* Sampler = It->second.GetPayload();
			if (Sampler == nullptr) return false;
			Samplers[Role] = Sampler->GetReference();
		}
		FragmentParameters.BaseColorSampler = Samplers[0];
		FragmentParameters.NormalSampler = Samplers[1];
		FragmentParameters.MetallicSampler = Samplers[2];
		FragmentParameters.RoughnessSampler = Samplers[3];
		FragmentParameters.AmbientOcclusionSampler = Samplers[4];
		FragmentParameters.EmissiveSampler = Samplers[5];
		FragmentParameters.OpacitySampler = Samplers[6];
		FragmentParameters.OpacityMaskSampler = Samplers[7];
		if (bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				   == EMaterialBlendMode::Masked)
		{
			FStaticMeshShadowFragmentShader::FParameters ShadowParameters;
			ShadowParameters.Material = MaterialBuffer;
			ShadowParameters.OpacityMaskTexture =
				FragmentParameters.OpacityMaskTexture;
			ShadowParameters.OpacityMaskSampler = Samplers[7];
			SetShaderParameters(CommandList, Pipeline->ShadowFragmentShader, ShadowParameters);
			CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
			return true;
		}
		FRHITexture* Irradiance = EnvironmentLighting.GetIrradiance_RenderThread();
		FRHITexture* Prefiltered = EnvironmentLighting.GetPrefiltered_RenderThread();
		FRHITexture* Brdf = EnvironmentLighting.GetBrdfLut_RenderThread();
		FRHISampler* EnvironmentSampler = EnvironmentLighting.GetSampler_RenderThread();
		const bool bEnvironment = Irradiance && Prefiltered && Brdf && EnvironmentSampler;
		FragmentParameters.EnvironmentIrradiance = bEnvironment ? Irradiance : DefaultTextures.GetCube_RenderThread();
		FragmentParameters.EnvironmentPrefiltered = bEnvironment ? Prefiltered : DefaultTextures.GetCube_RenderThread();
		FragmentParameters.EnvironmentBrdfLut = bEnvironment ? Brdf : DefaultTextures.Get_RenderThread(EDefaultTexture::Black);
		FragmentParameters.EnvironmentSampler = bEnvironment ? EnvironmentSampler : Samplers[0];
		FragmentParameters.DirectionalShadowTexture =
			Item.DirectionalShadowTexture != nullptr ? Item.DirectionalShadowTexture : DefaultTextures.GetArray_RenderThread();
		FragmentParameters.DirectionalShadowSampler =
			Item.DirectionalShadowSampler != nullptr ? Item.DirectionalShadowSampler : Samplers[0];
		SetShaderParameters(CommandList, Pipeline->FragmentShader, FragmentParameters);
		CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
		return true;
	}

	auto FSkeletalMeshRenderer::ReleaseResources_RenderThread() -> void
	{
		State->BaseResources.Reset();
		State->ShaderMaps.Reset();
		State->ShadowShaderMaps.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}
} // namespace Durin

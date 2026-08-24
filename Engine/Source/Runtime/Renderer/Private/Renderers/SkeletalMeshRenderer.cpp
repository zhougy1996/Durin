#include "Renderers/SkeletalMeshRenderer.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/MeshRendererExecution.h"
#include "Renderers/MeshRendererShared.h"

namespace Durin
{
	using namespace RendererPrivate;

	struct FSkeletalMeshRenderer::FState
	{
		struct FShaderMapPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkeletalMeshVertexShader> VertexShader;
			TShaderRef<FSurfaceFragmentShader> FragmentShader;
			TShaderRef<FSurfaceOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragmentShader;
		};
		struct FPipelinePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkeletalMeshVertexShader> VertexShader;
			TShaderRef<FSurfaceFragmentShader> FragmentShader;
			TShaderRef<FSurfaceOpaqueShadowFragmentShader>
				OpaqueShadowFragmentShader;
			TShaderRef<FSurfaceMaskedShadowFragmentShader> ShadowFragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderMapPayload>
			ShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FMaterialShaderMapIdentity, FShaderMapPayload>
			ShadowShaderMaps{ERenderResourceGenerationDependency::Shader};
		TRendererResourceSlotCache<FEffectiveMeshPipelineKey, FPipelinePayload> Pipelines{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
		TRendererResourceSlotCache<FEffectiveMeshPipelineKey, FPipelinePayload> ShadowPipelines{
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
			++Result.SharedPrimitiveFactBuilds;
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
				++Result.SharedSectionFactBuilds;
				FPreparedSkeletalMeshDraw Item;
				Item.Material = Proxy.ResolveMaterialRenderData_RenderThread(
					Section.MaterialSlotIndex
				);
				FMaterialRenderBinding LogicalBinding;
				if (!ResolveMaterialBinding(Item.Material, LogicalBinding,
						"SkeletalMeshMaterialSelection"))
					continue;
				Item.PrimitiveIndex = PrimitiveIndex;
				Item.SectionIndex = SectionIndex;
				Item.Section = &Section;
				Item.ShaderMapIdentity = Item.Material.PlanningPassIdentity.ShaderMap;
				Item.PipelineKey.Material = Item.Material.PlanningPassIdentity;
				Item.PipelineKey.VertexDomain = EVertexDeformationDomain::Skeletal;
				Item.PipelineKey.Rasterizer.PolygonMode =
					RasterMode == ERasterMode::Wireframe ? ERHIPolygonMode::Line : ERHIPolygonMode::Fill;
				Item.PipelineKey.Rasterizer.CullMode =
					Item.Material.PlanningPassIdentity.bTwoSided ? ERHICullMode::None : ERHICullMode::Back;
				Item.PipelineKey.Rasterizer.FrontFace = Determinant < 0.0 ? ERHIFrontFace::CounterClockwise : ERHIFrontFace::Clockwise;
				Item.PipelineKey.Depth.bEnableTest = true;
				Item.PipelineKey.Depth.CompareOp =
					View.DepthConvention == ESceneDepthConvention::ReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::Less;
				const EMaterialBlendMode BlendMode =
					Item.Material.PlanningPassIdentity.ShaderMap.BlendMode;
				Item.Pass = BlendMode == EMaterialBlendMode::Masked ? EMeshBasePass::Masked : BlendMode == EMaterialBlendMode::Translucent ? EMeshBasePass::Translucent :
																												   EMeshBasePass::Opaque;
				if (Mode == ERenderPreparationMode::ShadowDepth
					&& Item.Pass == EMeshBasePass::Translucent)
					continue;
				const auto DepthPolicy = Item.Material.PlanningPassIdentity.DepthWritePolicy;
				Item.PipelineKey.Depth.bEnableWrite =
					DepthPolicy == EMaterialDepthWritePolicy::Enabled
					|| (DepthPolicy == EMaterialDepthWritePolicy::Automatic
						&& Item.Pass != EMeshBasePass::Translucent);
				if (Item.Pass == EMeshBasePass::Translucent)
					Item.PipelineKey.ColorBlend = FRHIColorBlendState::StraightAlpha();
				const FVector4 Center = LocalToWorld
										* FVector4(Section.LocalBounds.GetCenter(), 1.0);
				if (!Math::IsFinite(Center)) continue;
				Item.SortCenter = FVector3(Center);
				const FVector3 Offset = Item.SortCenter - View.ViewLocation;
				Item.TranslucentDistanceSquared = Math::Dot(Offset, Offset);
				if (!std::isfinite(Item.TranslucentDistanceSquared)) continue;
				Item.bCastsShadow = Item.Pass != EMeshBasePass::Translucent;
				Item.SortKey = MakeSkeletalMeshDrawSortKey(
					Result.Primitives[PrimitiveIndex], Item
				);
				auto* Bucket = Item.Pass == EMeshBasePass::Opaque ? &Result.Opaque : Item.Pass == EMeshBasePass::Masked ? &Result.Masked :
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
		AssignResolvedIndices(Result.Opaque, Result.Masked, Result.Translucent);
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
		RendererPrivate::FSurfaceMaterialResources& InSurfaceMaterials
	)
		: Coordinator(InCoordinator)
		, SurfaceMaterials(InSurfaceMaterials)
		, State(std::make_unique<FState>())
	{
	}

	FSkeletalMeshRenderer::~FSkeletalMeshRenderer() = default;

	auto FSkeletalMeshRenderer::EnsureMaterialSamplers_RenderThread(
		const FMaterialRenderBinding& MaterialBinding
	) -> bool
	{
		return SurfaceMaterials.Ensure_RenderThread(
			MaterialBinding, ESurfaceMaterialPass::GBuffer);
	}

	auto FSkeletalMeshRenderer::EnsureSectionResources_RenderThread(
		const FPreparedSkeletalMeshPrimitive& Primitive,
		const FPreparedSkeletalMeshDraw& Item,
		const FMaterialRenderBinding& MaterialBinding,
		bool bShadowDepth,
		bool bHybridRetained
	) -> bool
	{
		if (Primitive.VertexFactory == nullptr) return false;
		const FMaterialRenderData& Material = Item.Material;
		if (!Material.CompiledProgram) return false;
		using FShaderResult = TRenderResourceCreateResult<FState::FShaderMapPayload>;
		auto& ShaderMapCache = bShadowDepth ? State->ShadowShaderMaps : State->ShaderMaps;
		auto& ShaderEntry = ShaderMapCache.FindOrAdd(
			Material.PlanningPassIdentity.ShaderMap
		);
		FState::FShaderMapPayload* ShaderPayload = ShaderEntry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &Material, bShadowDepth]() -> FShaderResult {
				const FMaterialShaderMapIdentity& Identity =
					Material.PlanningPassIdentity.ShaderMap;
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
				FShaderType& FragmentType = FSurfaceFragmentShader::StaticType();
				FShaderType& ShadowFragmentType =
					FSurfaceMaskedShadowFragmentShader::StaticType();
				FShaderType& OpaqueShadowFragmentType =
					FSurfaceOpaqueShadowFragmentShader::StaticType();
				std::vector<const FShaderType*> Types{&VertexType};
				if (!bShadowDepth)
					Types.push_back(&FragmentType);
				else if (Identity.BlendMode == EMaterialBlendMode::Masked)
					Types.push_back(&ShadowFragmentType);
				else
					Types.push_back(&OpaqueShadowFragmentType);
				std::shared_ptr<FShaderMapBase> ShaderMap;
				std::string Error;
				const bool bOpaqueShadow = bShadowDepth
					&& Identity.BlendMode != EMaterialBlendMode::Masked;
				bool bInitialized = false;
				if (bOpaqueShadow)
				{
					ShaderMap = std::make_shared<FShaderMapBase>();
					bInitialized = ShaderMap->InitializeFromShaderTypes(
						Types, Options, Error);
				}
				else
				{
					const FShaderType& GeneratedFragmentType = bShadowDepth
						? ShadowFragmentType : FragmentType;
					bInitialized = InitializeCompiledMaterialShaderMap(
						VertexType, GeneratedFragmentType,
						*Material.CompiledProgram,
						bShadowDepth ? "ShadowFragmentMain" : "FragmentMain",
						Options, ShaderMap, Error);
				}
				if (!bInitialized)
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
				auto* Fragment = !bShadowDepth ? static_cast<FSurfaceFragmentShader*>(
													 ShaderMap->GetShader(&FragmentType)
												 ) :
												 nullptr;
				auto* ShadowFragment = bShadowDepth
											   && Identity.BlendMode == EMaterialBlendMode::Masked ?
										   static_cast<FSurfaceMaskedShadowFragmentShader*>(
											   ShaderMap->GetShader(&ShadowFragmentType)
										   ) :
										   nullptr;
				auto* OpaqueShadowFragment =
					bShadowDepth && Identity.BlendMode != EMaterialBlendMode::Masked ? static_cast<FSurfaceOpaqueShadowFragmentShader*>(
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
		FEffectiveMeshPipelineKey EffectivePipelineKey =
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
				Initializer.RenderTargetLayout = bShadowDepth
					? RenderTargetLayouts::MakeDirectionalShadowDepth()
					: (EffectivePipelineKey.bHybridRetained
						? (EffectivePipelineKey.Material.ShaderMap.BlendMode
								== EMaterialBlendMode::Translucent
							? RenderTargetLayouts::MakeHybridSortedTranslucency()
							: RenderTargetLayouts::MakeHybridRetainedForward())
						: RenderTargetLayouts::MakeSceneTargets());
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

		const ESurfaceMaterialPass SurfacePass = bShadowDepth
			? (Item.PipelineKey.Material.ShaderMap.BlendMode
					== EMaterialBlendMode::Masked
				? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::OpaqueShadow)
			: ESurfaceMaterialPass::Forward;
		return SurfaceMaterials.Ensure_RenderThread(
			MaterialBinding, SurfacePass);
	}

	auto FSkeletalMeshRenderer::PrepareResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPreparedSkeletalPaletteTable& PreparedPalettes,
		FResolvedSkeletalPaletteTable& ResolvedPalettes,
		const FPreparedSkeletalMeshView& PreparedView,
		FResolvedSkeletalMeshView& ResolvedView,
		bool bPrepareLitOpaqueForward
	) -> FGeometryResolutionResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		ResolvedView.Draws.resize(PreparedView.GetNumSections());
		ResolvedView.PaletteRanges.resize(PreparedView.Primitives.size());
		for (size_t PrimitiveIndex = 0;
			PrimitiveIndex < PreparedView.Primitives.size(); ++PrimitiveIndex)
		{
			const auto& Primitive = PreparedView.Primitives[PrimitiveIndex];
			FRHIStorageBufferRange PaletteRange;
			switch (ResolveSkeletalPalette_RenderThread(
				CommandList, PreparedPalettes, ResolvedPalettes, Primitive,
				PaletteRange
			))
			{
			case ESkeletalPaletteResolveResult::Uploaded:
				++ResolvedView.Observations.UploadedPalettes;
				ResolvedView.Observations.UploadedPaletteMatrices += Primitive.Pose->Matrices.size();
				ResolvedView.Observations.UploadedPaletteBytes += PaletteRange.Size;
				break;
			case ESkeletalPaletteResolveResult::Reused:
				++ResolvedView.Observations.ReusedPalettes;
				break;
			case ESkeletalPaletteResolveResult::Rejected:
				++ResolvedView.Observations.RejectedPalettes;
				break;
			}
			if (PaletteRange.Buffer != nullptr)
				ResolvedView.PaletteRanges[PrimitiveIndex] = PaletteRange;
		}
		ForEachBasePassBucket(PreparedView, [&](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++ResolvedView.Observations.ResourcePreparationAttemptedDraws;
				FMaterialRenderBinding MaterialBinding;
				if (!ResolvePreparedMaterialBinding(Draw.Material, MaterialBinding,
						"SkeletalMeshMaterialBinding"))
					continue;
				auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
				Record.MaterialBinding = std::move(MaterialBinding);
				const FMaterialRenderBinding& StoredBinding =
					*Record.MaterialBinding;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const bool bNeedsForwardPipeline =
					Pass == EMeshBasePass::Translucent
					|| bPrepareLitOpaqueForward
					|| Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
						   != EMaterialShadingModel::Lit;
				const bool bReady = Primitive != nullptr
									   && ResolvedView.GetPaletteRange(Draw).Buffer != nullptr
									   && (bNeedsForwardPipeline ? EnsureSectionResources_RenderThread(*Primitive, Draw, StoredBinding) : EnsureMaterialSamplers_RenderThread(StoredBinding));
				if (bReady)
				{
					Record.bReady = true;
					++ResolvedView.Observations.ResourcePreparationSuccessfulDraws;
				}
			}
		});
		check(PreparedView.RequestedPaletteUploads == ResolvedView.Observations.UploadedPalettes + ResolvedView.Observations.ReusedPalettes + ResolvedView.Observations.RejectedPalettes);
		check(ResolvedView.Observations.UploadedPaletteBytes == ResolvedView.Observations.UploadedPaletteMatrices * sizeof(FMatrix4f));
		return FinalizeResourcePreparation(ResolvedView);
	}

	auto FSkeletalMeshRenderer::PrepareHybridRetainedResources_RenderThread(
		const FPreparedSkeletalMeshView& PreparedView,
		const FResolvedSkeletalMeshView& ResolvedView
	) -> bool
	{
		check(IsInRenderingThread());
		bool bReady = true;
		ForEachBasePassBucket(PreparedView, [this, &PreparedView,
			&ResolvedView, &bReady](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				if (Pass != EMeshBasePass::Translucent
					&& Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
						   == EMaterialShadingModel::Lit)
					continue;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const FMaterialRenderBinding* MaterialBinding =
					ResolvedView.GetMaterialBinding(Draw);
				bReady = Primitive != nullptr && MaterialBinding != nullptr
						 && EnsureSectionResources_RenderThread(
							 *Primitive, Draw, *MaterialBinding, false, true
						 )
						 && bReady;
			}
		});
		return bReady;
	}

	auto FSkeletalMeshRenderer::PrepareShadowResources_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPreparedSkeletalPaletteTable& PreparedPalettes,
		FResolvedSkeletalPaletteTable& ResolvedPalettes,
		const FPreparedSkeletalMeshView& PreparedView,
		FResolvedSkeletalMeshView& ResolvedView
	) -> FGeometryResolutionResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		check(PreparedView.Translucent.empty());
		ResolvedView.Draws.resize(PreparedView.GetNumSections());
		ResolvedView.PaletteRanges.resize(PreparedView.Primitives.size());
		for (size_t PrimitiveIndex = 0;
			PrimitiveIndex < PreparedView.Primitives.size(); ++PrimitiveIndex)
		{
			const auto& Primitive = PreparedView.Primitives[PrimitiveIndex];
			FRHIStorageBufferRange PaletteRange;
			switch (ResolveSkeletalPalette_RenderThread(
				CommandList, PreparedPalettes, ResolvedPalettes, Primitive,
				PaletteRange
			))
			{
			case ESkeletalPaletteResolveResult::Uploaded:
				++ResolvedView.Observations.UploadedPalettes;
				ResolvedView.Observations.UploadedPaletteMatrices += Primitive.Pose->Matrices.size();
				ResolvedView.Observations.UploadedPaletteBytes += PaletteRange.Size;
				break;
			case ESkeletalPaletteResolveResult::Reused:
				++ResolvedView.Observations.ReusedPalettes;
				break;
			case ESkeletalPaletteResolveResult::Rejected:
				++ResolvedView.Observations.RejectedPalettes;
				break;
			}
			if (PaletteRange.Buffer != nullptr)
				ResolvedView.PaletteRanges[PrimitiveIndex] = PaletteRange;
		}
		ForEachShadowBucket(PreparedView, [this, &PreparedView, &ResolvedView](const auto& Bucket) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++ResolvedView.Observations.ResourcePreparationAttemptedDraws;
				FMaterialRenderBinding MaterialBinding;
				if (!ResolvePreparedMaterialBinding(Draw.Material, MaterialBinding,
						"SkeletalMeshShadowMaterialBinding"))
					continue;
				auto& Record = ResolvedView.Draws[Draw.ResolvedIndex];
				Record.MaterialBinding = std::move(MaterialBinding);
				const FMaterialRenderBinding& StoredBinding =
					*Record.MaterialBinding;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const bool bReady = Primitive != nullptr
									   && ResolvedView.GetPaletteRange(Draw).Buffer != nullptr
									   && EnsureSectionResources_RenderThread(*Primitive, Draw, StoredBinding, true);
				Record.bReady = bReady;
				ResolvedView.Observations.ResourcePreparationSuccessfulDraws +=
					bReady ? 1u : 0u;
			}
		});
		return FinalizeResourcePreparation(ResolvedView);
	}

	auto FSkeletalMeshRenderer::ExecuteShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& ShadowView,
		const FRHIUniformBufferRange& FallbackLighting,
		const FPreparedSkeletalMeshView& PreparedView,
		FResolvedSkeletalMeshView& ResolvedView
	) -> bool
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		bool bComplete = true;
		ForEachShadowBucket(PreparedView, [this, &CommandList, &ShadowView,
			&FallbackLighting, &PreparedView, &ResolvedView,
			&bComplete](const auto& Bucket) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++ResolvedView.Observations.AttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				if (Primitive != nullptr && ResolvedView.IsReady(Draw)
					&& DrawSection_RenderThread(
						CommandList, ShadowView, FallbackLighting,
						ERenderMode::Unlit, *Primitive, Draw, ResolvedView, true
					))
					++ResolvedView.Observations.SuccessfulDraws;
				else
				{
					bComplete = false;
					++ResolvedView.Observations.RejectedDraws;
				}
			}
		});
		FinalizeExecution(ResolvedView, PreparedView.GetNumSections(), false);
		return bComplete;
	}

	auto FSkeletalMeshRenderer::Execute_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedSkeletalMeshView& PreparedView, FResolvedSkeletalMeshView& ResolvedView
	) -> void
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
			return;
		ForEachBasePassBucket(PreparedView, [&](const auto& Bucket, EMeshBasePass Pass) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				++ResolvedView.Observations.AttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				const bool bComplete = Primitive != nullptr
									   && Primitive->PrimitiveId != InvalidPrimitiveSceneId
									   && Primitive->RenderData != nullptr
									   && Primitive->VertexFactory != nullptr
									   && Primitive->Pose != nullptr
									   && ResolvedView.GetPaletteRange(Draw).Buffer != nullptr
									   && Draw.Section != nullptr && Draw.Pass == Pass
									   && Draw.SortKey.Pipeline[0] == static_cast<uint32>(Pass)
									   && Draw.ShaderMapIdentity
											  == Draw.Material.PlanningPassIdentity.ShaderMap;
				if (!bComplete || !ResolvedView.IsReady(Draw))
				{
					++ResolvedView.Observations.RejectedDraws;
					continue;
				}
				if (DrawSection_RenderThread(CommandList, View, Lighting,
					RenderMode, *Primitive, Draw, ResolvedView))
					++ResolvedView.Observations.SuccessfulDraws;
				else
					++ResolvedView.Observations.RejectedDraws;
			}
		});
		FinalizeExecution(ResolvedView, PreparedView.GetNumSections());
	}

	auto FSkeletalMeshRenderer::ExecutePreparedDraw_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedSkeletalMeshDraw& Draw, const FPreparedSkeletalMeshView& PreparedView, FResolvedSkeletalMeshView& ResolvedView, bool bHybridRetained
	) -> void
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		++ResolvedView.Observations.AttemptedDraws;
		const FPreparedSkeletalMeshPrimitive* Primitive =
			PreparedView.GetPrimitive(Draw);
		const bool bComplete = Primitive != nullptr
							   && Primitive->PrimitiveId != InvalidPrimitiveSceneId
							   && Primitive->RenderData != nullptr
							   && Primitive->VertexFactory != nullptr && Primitive->Pose != nullptr
							   && ResolvedView.GetPaletteRange(Draw).Buffer != nullptr
							   && Draw.Section != nullptr && Draw.Pass == Pass
							   && Draw.SortKey.Pipeline[0] == static_cast<uint32>(Pass)
							   && Draw.ShaderMapIdentity == Draw.Material.PlanningPassIdentity.ShaderMap;
		if (!bComplete || !ResolvedView.IsReady(Draw))
		{
			++ResolvedView.Observations.RejectedDraws;
			return;
		}
		if (DrawSection_RenderThread(CommandList, View, Lighting, RenderMode,
			*Primitive, Draw, ResolvedView, false, bHybridRetained))
			++ResolvedView.Observations.SuccessfulDraws;
		else
			++ResolvedView.Observations.RejectedDraws;
	}

	auto FSkeletalMeshRenderer::ExecutePass_RenderThread(
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, EMeshBasePass Pass, const FPreparedSkeletalMeshView& PreparedView, FResolvedSkeletalMeshView& ResolvedView
	) -> void
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		if (RenderMode != ERenderMode::Unlit && RenderMode != ERenderMode::Lit)
			return;
		const auto& Bucket = GetBasePassBucket(PreparedView, Pass);
		for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			ExecutePreparedDraw_RenderThread(CommandList, View, Lighting,
				RenderMode, Pass, Draw, PreparedView, ResolvedView);
	}

	auto FSkeletalMeshRenderer::FinalizeExecution_RenderThread(
		FResolvedSkeletalMeshView& ResolvedView
	) -> void
	{
		check(IsInRenderingThread());
		FinalizeExecution(ResolvedView, ResolvedView.Observations.AttemptedDraws);
	}

	auto FSkeletalMeshRenderer::ExecuteGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FGBufferRenderer& GBuffer,
		const FPreparedSkeletalMeshView& PreparedView,
		FResolvedSkeletalMeshView& ResolvedView
	) -> FGeometryExecutionResult
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		bool bComplete = true;
		bool bRenderedGeometry = false;
		ResolvedView.Observations.GBufferSkippedDraws += PreparedView.Translucent.size();
		ForEachShadowBucket(PreparedView, [this, &CommandList, &View, &GBuffer,
			&PreparedView, &ResolvedView, &bComplete,
			&bRenderedGeometry](const auto& Bucket) {
			for (const FPreparedSkeletalMeshDraw& Draw : Bucket)
			{
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					++ResolvedView.Observations.GBufferSkippedDraws;
					continue;
				}
				++ResolvedView.Observations.GBufferAttemptedDraws;
				const FPreparedSkeletalMeshPrimitive* Primitive =
					PreparedView.GetPrimitive(Draw);
				if (Primitive != nullptr && ResolvedView.IsReady(Draw)
					&& DrawGBufferSection_RenderThread(
						CommandList, View, GBuffer, *Primitive, Draw,
						ResolvedView
					))
				{
					bRenderedGeometry = true;
					++ResolvedView.Observations.GBufferSuccessfulDraws;
				}
				else
				{
					bComplete = false;
					++ResolvedView.Observations.GBufferRejectedDraws;
				}
			}
		});
		check(ResolvedView.Observations.GBufferAttemptedDraws == ResolvedView.Observations.GBufferSuccessfulDraws + ResolvedView.Observations.GBufferRejectedDraws);
		return {bComplete, bRenderedGeometry,
			ResolvedView.Observations.GBufferAttemptedDraws,
			ResolvedView.Observations.GBufferSuccessfulDraws,
			ResolvedView.Observations.GBufferRejectedDraws,
			ResolvedView.Observations.GBufferSkippedDraws};
	}

	auto FSkeletalMeshRenderer::DrawGBufferSection_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		FGBufferRenderer& GBuffer,
		const FPreparedSkeletalMeshPrimitive& Primitive,
		const FPreparedSkeletalMeshDraw& Item,
		const FResolvedSkeletalMeshView& ResolvedView
	) -> bool
	{
		if (Primitive.RenderData == nullptr || Primitive.VertexFactory == nullptr
			|| Primitive.Pose == nullptr || Item.Section == nullptr)
		{
			return false;
		}
		const FVertexDeclarationRHIRef VertexDeclaration(
			Primitive.VertexFactory->GetDeclaration()
		);
		FGBufferRenderer::FPipeline* Pipeline =
			GBuffer.EnsurePipeline_RenderThread({.Material = Item.PipelineKey.Material, .CompiledProgram = Item.Material.CompiledProgram, .Rasterizer = Item.PipelineKey.Rasterizer, .Depth = Item.PipelineKey.Depth, .VertexDeclaration = VertexDeclaration, .VertexDomain = EGBufferVertexDomain::Skeletal});
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
		FResolvedSurfaceMaterial SurfaceMaterial;
		const FMaterialRenderBinding* MaterialBinding =
			ResolvedView.GetMaterialBinding(Item);
		if (MaterialBinding == nullptr || !SurfaceMaterials.Resolve_RenderThread(
				*MaterialBinding, ESurfaceMaterialPass::GBuffer, true,
				View.Settings.Mode.bEnableSpecularAA,
				nullptr, nullptr, SurfaceMaterial)) return false;
		const FSurfaceMaterialUniform& MaterialUniform = SurfaceMaterial.Uniform;
		const FRHIUniformBufferRange MaterialBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&MaterialUniform, sizeof(MaterialUniform)
			);
		const FGBufferRenderer::FVertexParameters VertexParameters{
			.Transform = TransformBuffer,
			.SkinPalette = ResolvedView.GetPaletteRange(Item)
		};
		FGBufferRenderer::FFragmentParameters FragmentParameters;
		FragmentParameters.Material = MaterialBuffer;
		FragmentParameters.Textures = SurfaceMaterial.Textures;
		FragmentParameters.Samplers = SurfaceMaterial.Samplers;
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
		FRHICommandListImmediate& CommandList, const FSceneView& View, const FRHIUniformBufferRange& Lighting, ERenderMode RenderMode, const FPreparedSkeletalMeshPrimitive& Primitive, const FPreparedSkeletalMeshDraw& Item, const FResolvedSkeletalMeshView& ResolvedView, bool bShadowDepth, bool bHybridRetained
	) -> bool
	{
		const FSkeletalMeshRenderData& Data = *Primitive.RenderData;
		const FSkeletalMeshRenderSection& Section = *Item.Section;
		const FMatrix& LocalToWorld = Primitive.LocalToWorld;
		const FMaterialRenderData& Material = Item.Material;
		const FMaterialRenderBinding* Binding =
			ResolvedView.GetMaterialBinding(Item);
		if (Binding == nullptr) return false;
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
		FEffectiveMeshPipelineKey EffectivePipelineKey =
			bShadowDepth ? MakeShadowPipelineKey(Item.PipelineKey) : Item.PipelineKey;
		EffectivePipelineKey.bHybridRetained =
			!bShadowDepth && bHybridRetained;
		auto* PipelineEntry = bShadowDepth ? State->ShadowPipelines.Find(EffectivePipelineKey) : State->Pipelines.Find(EffectivePipelineKey);
		FState::FPipelinePayload* Pipeline = PipelineEntry != nullptr ? PipelineEntry->Slot.GetPayload() : nullptr;
		if (Pipeline == nullptr) return false;
		CommandList.SetGraphicsPipelineState(*Pipeline->PipelineState);
		if (bShadowDepth)
		{
			const FRHIRasterizerState Rasterizer =
				MakeShadowRasterizerState(Item.PipelineKey.Rasterizer);
			CommandList.SetDepthBias(Rasterizer.DepthBiasConstantFactor, Rasterizer.DepthBiasClamp, Rasterizer.DepthBiasSlopeFactor);
		}
		FSkeletalMeshVertexShader::FParameters VertexParameters;
		VertexParameters.Transform = TransformBuffer;
		VertexParameters.SkinPalette = ResolvedView.GetPaletteRange(Item);
		SetShaderParameters(CommandList, Pipeline->VertexShader, VertexParameters);
		if (bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				   != EMaterialBlendMode::Masked)
		{
			return ExecuteMeshSurfacePass_RenderThread(
				CommandList, ESurfaceMaterialPass::OpaqueShadow, Lighting,
				nullptr, {}, Pipeline->FragmentShader,
				Pipeline->ShadowFragmentShader,
				[&] { CommandList.DrawIndexed(
					Section.IndexCount, Section.FirstIndex, 0); });
		}
		const bool bMaskedShadow = bShadowDepth
			&& Item.PipelineKey.Material.ShaderMap.BlendMode
				== EMaterialBlendMode::Masked;
		FResolvedSurfaceMaterial SurfaceMaterial;
		if (!SurfaceMaterials.Resolve_RenderThread(
				*Binding,
				bMaskedShadow ? ESurfaceMaterialPass::MaskedShadow
					: ESurfaceMaterialPass::Forward,
				RenderMode == ERenderMode::Lit
					&& Material.PlanningPassIdentity.ShaderMap.ShadingModel
						== EMaterialShadingModel::Lit,
				View.Settings.Mode.bEnableSpecularAA,
				ResolvedView.DirectionalShadowTexture,
				ResolvedView.DirectionalShadowSampler,
				SurfaceMaterial)) return false;
		const FRHIUniformBufferRange MaterialBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&SurfaceMaterial.Uniform, sizeof(SurfaceMaterial.Uniform)
			);
		return ExecuteMeshSurfacePass_RenderThread(
			CommandList,
			bMaskedShadow ? ESurfaceMaterialPass::MaskedShadow
				: ESurfaceMaterialPass::Forward,
			Lighting, &SurfaceMaterial, MaterialBuffer,
			Pipeline->FragmentShader, Pipeline->ShadowFragmentShader,
			[&] { CommandList.DrawIndexed(
				Section.IndexCount, Section.FirstIndex, 0); });
	}

	auto FSkeletalMeshRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->ShaderMaps.Reset();
		State->ShadowShaderMaps.Reset();
		State->Pipelines.Reset();
		State->ShadowPipelines.Reset();
	}
} // namespace Durin

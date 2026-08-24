#include "RenderGraph.h"

#include "RHICommandList.h"

#include <gtest/gtest.h>

namespace Durin
{
	namespace
	{
		auto WholeColor(uint32 Mips = 1) -> FRHITextureSubresourceRange
		{
			return {ERHITextureAspect::Color, 0, Mips, 0, 1};
		}

		auto MakeGraphTexture(const char* Name, uint8 Mips = 1) -> FRHITexture
		{
			return FRHITexture(FRHITextureCreateDesc::Create2D(
				Name, 64, 64, EPixelFormat::RGBA8_UNORM)
				.SetNumMips(Mips)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::Storage
					| ETextureCreateFlags::SourceCopy
					| ETextureCreateFlags::DestinationCopy));
		}
	} // namespace

	TEST(FRenderGraphTests, CompilesStableHazardOrderAndExactTextureTransitions)
	{
		FRHITexture Texture = MakeGraphTexture("SceneColor");
		FRenderGraphBuilder Builder;
		const auto SceneColor = Builder.CreateTexture(
			"SceneColor", &Texture, ERHIAccess::GraphicsShaderRead);
		const auto Independent = Builder.AddPass(
			"Independent", ERenderGraphPassType::Copy);
		const auto Produce = Builder.AddPass(
			"Produce", ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(Produce, SceneColor, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store);
		const auto Consume = Builder.AddPass(
			"Consume", ERenderGraphPassType::Compute);
		Builder.UseTexture(Consume, SceneColor, WholeColor(),
			ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 3u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Independent");
		EXPECT_EQ(Result.Graph->GetPasses()[1].Name, "Produce");
		EXPECT_EQ(Result.Graph->GetPasses()[2].Name, "Consume");
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0],
			(FRenderGraphDependency{1, 2, "SceneColor",
				ERenderGraphDependencyKind::Value}));
		ASSERT_EQ(Result.Graph->GetPasses()[1].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[1].TextureTransitions[0],
			(FRHITextureTransition{&Texture, WholeColor(), ERHIAccess::Discard,
				ERHIAccess::ColorAttachmentReadWrite}));
		ASSERT_EQ(Result.Graph->GetPasses()[2].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[2].TextureTransitions[0],
			(FRHITextureTransition{&Texture, WholeColor(),
				ERHIAccess::ColorAttachmentReadWrite,
				ERHIAccess::ComputeShaderRead}));
		ASSERT_EQ(Result.Graph->GetFinalTextureTransitions().size(), 1u);
		EXPECT_EQ(Result.Graph->GetFinalTextureTransitions()[0],
			(FRHITextureTransition{&Texture, WholeColor(),
				ERHIAccess::ComputeShaderRead,
				ERHIAccess::GraphicsShaderRead}));
	}

	TEST(FRenderGraphTests, CompilesBufferRawWarAndWawDependencies)
	{
		FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
			"Work", 64, 4, EBufferUsageFlags::UnorderedAccess
				| EBufferUsageFlags::SourceCopy));
		FRenderGraphBuilder Builder;
		const auto Work = Builder.CreateBuffer("Work", &Buffer);
		const auto Write = Builder.AddPass("Write", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Write, Work, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Read = Builder.AddPass("Read", ERenderGraphPassType::Copy);
		Builder.UseBuffer(Read, Work, 0, 64, ERenderGraphUse::Read,
			ERHIAccess::TransferRead);
		const auto Rewrite = Builder.AddPass("Rewrite", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Rewrite, Work, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite);

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 2u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Kind,
			ERenderGraphDependencyKind::Value);
		EXPECT_EQ(Result.Graph->GetDependencies()[1].Kind,
			ERenderGraphDependencyKind::Execution);
		EXPECT_EQ(Result.Graph->GetPasses()[0].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::Discard);
		EXPECT_EQ(Result.Graph->GetPasses()[1].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::ComputeShaderReadWrite);
		EXPECT_EQ(Result.Graph->GetPasses()[2].BufferTransitions[0].ExpectedBefore,
			ERHIAccess::TransferRead);
	}

	TEST(FRenderGraphTests, RejectsMissingProducerForeignHandleAndCycle)
	{
		FRHITexture Texture = MakeGraphTexture("Missing");
		FRenderGraphBuilder MissingProducer;
		const auto Logical = MissingProducer.CreateTexture("Missing", &Texture);
		const auto Read = MissingProducer.AddPass("Read", ERenderGraphPassType::Graphics);
		MissingProducer.UseTexture(Read, Logical, WholeColor(),
			ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		auto Missing = MissingProducer.Compile();
		EXPECT_FALSE(Missing.IsSuccess());
		EXPECT_NE(Missing.Error.find("before its producer"), std::string::npos);

		FRenderGraphBuilder ForeignOwner;
		const auto Foreign = ForeignOwner.CreateTexture("Foreign", &Texture);
		FRenderGraphBuilder ForeignUse;
		const auto Pass = ForeignUse.AddPass("Use", ERenderGraphPassType::Graphics);
		ForeignUse.UseTexture(Pass, Foreign, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Invalid = ForeignUse.Compile();
		EXPECT_FALSE(Invalid.IsSuccess());
		EXPECT_NE(Invalid.Error.find("invalid resource handle"), std::string::npos);

		FRenderGraphBuilder Cyclic;
		const auto A = Cyclic.AddPass("A", ERenderGraphPassType::Compute);
		const auto B = Cyclic.AddPass("B", ERenderGraphPassType::Compute);
		Cyclic.AddDependency(A, B);
		Cyclic.AddDependency(B, A);
		auto Cycle = Cyclic.Compile();
		EXPECT_FALSE(Cycle.IsSuccess());
		EXPECT_EQ(Cycle.Error, "graph contains a dependency cycle");
	}

	TEST(FRenderGraphTests, NormalizesDisjointAndPartiallyOverlappingSubresources)
	{
		FRHITexture Texture = MakeGraphTexture("MipChain", 4);
		FRenderGraphBuilder Builder;
		const auto Chain = Builder.CreateTexture("MipChain", &Texture);
		const auto Mip0 = Builder.AddPass("Mip0", ERenderGraphPassType::Compute);
		Builder.UseTexture(Mip0, Chain, {ERHITextureAspect::Color, 0, 1, 0, 1},
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Mip1 = Builder.AddPass("Mip1", ERenderGraphPassType::Compute);
		Builder.UseTexture(Mip1, Chain, {ERHITextureAspect::Color, 1, 1, 0, 1},
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		auto Disjoint = Builder.Compile();
		ASSERT_TRUE(Disjoint.IsSuccess()) << Disjoint.Error;
		EXPECT_TRUE(Disjoint.Graph->GetDependencies().empty());

		FRenderGraphBuilder Partial;
		const auto PartialChain = Partial.CreateTexture("MipChain", &Texture);
		const auto Whole = Partial.AddPass("Whole", ERenderGraphPassType::Compute);
		Partial.UseTexture(Whole, PartialChain, WholeColor(4),
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto OneMip = Partial.AddPass("OneMip", ERenderGraphPassType::Compute);
		Partial.UseTexture(OneMip, PartialChain,
			{ERHITextureAspect::Color, 1, 1, 0, 1}, ERenderGraphUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Overlap = Partial.Compile();
		ASSERT_TRUE(Overlap.IsSuccess()) << Overlap.Error;
		ASSERT_EQ(Overlap.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Overlap.Graph->GetDependencies()[0].Kind,
			ERenderGraphDependencyKind::Value);
		EXPECT_EQ(Overlap.Graph->GetPasses()[0].TextureTransitions.size(), 3u);
		EXPECT_EQ(Overlap.Graph->GetPasses()[1].TextureTransitions.size(), 1u);
	}

	TEST(FRenderGraphTests, DiscardedAttachmentStoreCannotBecomeAProducer)
	{
		FRHITexture Texture = MakeGraphTexture("Discarded");
		FRenderGraphBuilder Builder;
		const auto Target = Builder.CreateTexture("Discarded", &Texture);
		const auto Clear = Builder.AddPass("Clear", ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(Clear, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::DontCare);
		const auto Read = Builder.AddPass("Read", ERenderGraphPassType::Graphics);
		Builder.UseTexture(Read, Target, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
	}

	TEST(FRenderGraphTests, PreservesImportedInitialAndFinalStates)
	{
		FRHITexture Texture = MakeGraphTexture("Imported");
		FRenderGraphBuilder Builder;
		const auto Imported = Builder.ImportTexture("Imported", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Compute = Builder.AddPass("Compute", ERenderGraphPassType::Compute);
		Builder.UseTexture(Compute, Imported, WholeColor(),
			ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses()[0].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].TextureTransitions[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		ASSERT_EQ(Result.Graph->GetFinalTextureTransitions().size(), 1u);
		EXPECT_EQ(Result.Graph->GetFinalTextureTransitions()[0].RequiredAfter,
			ERHIAccess::GraphicsShaderRead);
	}

	TEST(FRenderGraphTests, RejectsAttachmentLoadWithoutPriorContents)
	{
		FRHITexture Texture = MakeGraphTexture("Load");
		FRenderGraphBuilder Builder;
		const auto Target = Builder.CreateTexture("Load", &Texture);
		const auto Load = Builder.AddPass("Load", ERenderGraphPassType::Graphics);
		Builder.UseColorAttachment(Load, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Load,
			ERHIRenderTargetStoreAction::Store);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Error.find("before its producer"), std::string::npos);
	}

	TEST(FRenderGraphTests, DumpIsDeterministicAndSyntheticCompileCostIsBounded)
	{
		auto CompileFixture = [] {
			static FRHIBuffer Buffer(FRHIBufferCreateDesc::Create(
				"Fixture", 512, 4, EBufferUsageFlags::UnorderedAccess));
			FRenderGraphBuilder Builder;
			const auto Work = Builder.CreateBuffer("Fixture", &Buffer);
			for (uint32 Index = 0; Index < 128; ++Index)
			{
				const auto Pass = Builder.AddPass("Pass" + std::to_string(Index),
					ERenderGraphPassType::Compute);
				Builder.UseBuffer(Pass, Work, 0, 512, ERenderGraphUse::Write,
					ERHIAccess::ComputeShaderReadWrite, Index == 0);
			}
			return Builder.Compile();
		};
		auto First = CompileFixture();
		auto Second = CompileFixture();
		ASSERT_TRUE(First.IsSuccess()) << First.Error;
		ASSERT_TRUE(Second.IsSuccess()) << Second.Error;
		EXPECT_EQ(First.Graph->Dump(), Second.Graph->Dump());
		EXPECT_LT(First.Graph->GetCompileMicroseconds(), 250000u);
		EXPECT_LT(Second.Graph->GetCompileMicroseconds(), 250000u);
		EXPECT_EQ(First.Graph->GetDependencies().size(), 127u);
	}

	TEST(FRenderGraphTests, CullsUnreachableBranchesAndReportsExactLifetimes)
	{
		FRHIBuffer RetainedBuffer(FRHIBufferCreateDesc::Create(
			"Retained", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRHIBuffer CulledBuffer(FRHIBufferCreateDesc::Create(
			"Culled", 64, 4, EBufferUsageFlags::UnorderedAccess));
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Retained = Builder.CreateBuffer("Retained", &RetainedBuffer);
		const auto Culled = Builder.CreateBuffer("Culled", &CulledBuffer);
		const auto Produce = Builder.AddPass("Produce", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Produce, Retained, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Consume = Builder.AddPass("Present", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Consume, Retained, 0, 64, ERenderGraphUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Consume, "present");
		const auto Unused = Builder.AddPass("Unused", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Unused, Culled, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);

		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 2u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Produce");
		EXPECT_EQ(Result.Graph->GetPasses()[1].Name, "Present");
		ASSERT_EQ(Result.Graph->GetResourceLifetimes().size(), 2u);
		EXPECT_EQ(Result.Graph->GetResourceLifetimes()[0].FirstPass, 0u);
		EXPECT_EQ(Result.Graph->GetResourceLifetimes()[0].LastPass, 1u);
		EXPECT_FALSE(Result.Graph->GetResourceLifetimes()[0].bCulled);
		EXPECT_TRUE(Result.Graph->GetResourceLifetimes()[1].bCulled);
		ASSERT_EQ(Result.Graph->GetCullingDecisions().size(), 3u);
		EXPECT_FALSE(Result.Graph->GetCullingDecisions()[0].bCulled);
		EXPECT_EQ(Result.Graph->GetCullingDecisions()[0].Reason, "value dependency");
		EXPECT_EQ(Result.Graph->GetCullingDecisions()[1].Reason, "present");
		EXPECT_TRUE(Result.Graph->GetCullingDecisions()[2].bCulled);
	}

	TEST(FRenderGraphTests, RejectsDuplicateImportedIdentityAndDomainMismatch)
	{
		FRHITexture Texture = MakeGraphTexture("Shared");
		FRenderGraphBuilder Duplicate;
		Duplicate.ImportTexture("First", &Texture, ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
		Duplicate.ImportTexture("Second", &Texture, ERHIAccess::GraphicsShaderRead,
			ERHIAccess::GraphicsShaderRead);
		auto DuplicateResult = Duplicate.Compile();
		EXPECT_FALSE(DuplicateResult.IsSuccess());
		EXPECT_NE(DuplicateResult.Error.find("duplicate imported physical"),
			std::string::npos);

		FRenderGraphBuilder Domain;
		const auto Imported = Domain.ImportTexture("Shared", &Texture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Copy = Domain.AddPass("Copy", ERenderGraphPassType::Copy);
		Domain.UseTexture(Copy, Imported, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto DomainResult = Domain.Compile();
		EXPECT_FALSE(DomainResult.IsSuccess());
		EXPECT_NE(DomainResult.Error.find("incompatible with pass domain"),
			std::string::npos);
	}

	TEST(FRenderGraphTests, DiscardValueCullingDoesNotRetainOverwrittenProducer)
	{
		FRHITexture Texture = MakeGraphTexture("Versioned");
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Resource = Builder.CreateTexture("Versioned", &Texture);
		const auto Old = Builder.AddPass("Old", ERenderGraphPassType::Compute);
		Builder.UseTexture(Old, Resource, WholeColor(), ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		const auto Replacement = Builder.AddPass("Replacement",
			ERenderGraphPassType::Compute);
		Builder.UseTexture(Replacement, Resource, WholeColor(),
			ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto Consume = Builder.AddPass("Consume", ERenderGraphPassType::Compute);
		Builder.UseTexture(Consume, Resource, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::ComputeShaderRead);
		Builder.MarkPassRoot(Consume, "output");
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 2u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Replacement");
		EXPECT_TRUE(Result.Graph->GetCullingDecisions()[0].bCulled);
	}

	TEST(FRenderGraphTests, RetainedLogicalResourcesPublishExactPreparationCapture)
	{
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		Builder.SetBackingResolver([](auto, auto&, std::string&) { return true; });
		FRenderGraphBufferDesc Desc{
			.Buffer = FRHIBufferDesc(64, 4, EBufferUsageFlags::UnorderedAccess),
			.BackingClass = "test-pool"};
		const auto Retained = Builder.CreateBuffer("Retained", Desc);
		const auto Culled = Builder.CreateBuffer("Culled", Desc);
		const auto Produce = Builder.AddPass("Produce", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Produce, Retained, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		Builder.MarkPassRoot(Produce, "effect");
		const auto Unused = Builder.AddPass("Unused", ERenderGraphPassType::Compute);
		Builder.UseBuffer(Unused, Culled, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		const auto Capture = Result.Graph->Capture();
		ASSERT_EQ(Capture.Resources.size(), 2u);
		EXPECT_EQ(Capture.Resources[0].Preparation, "requested");
		EXPECT_EQ(Capture.Resources[0].BackingClass, "test-pool");
		EXPECT_EQ(Capture.Resources[1].Preparation, "culled");
		ASSERT_EQ(Capture.Uses.size(), 1u);
		EXPECT_EQ(Capture.Uses[0].Version, 1u);
	}

	TEST(FRenderGraphTests, PassResourceViewRejectsUndeclaredLookup)
	{
		FRHITexture DeclaredTexture = MakeGraphTexture("Declared");
		FRHITexture HiddenTexture = MakeGraphTexture("Hidden");
		FRenderGraphBuilder Builder;
		const auto Declared = Builder.ImportTexture("Declared", &DeclaredTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Hidden = Builder.ImportTexture("Hidden", &HiddenTexture,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto Pass = Builder.AddPass("Pass", ERenderGraphPassType::Graphics,
			[=](FRHICommandListImmediate&, const FRenderGraphPassResources& Resources) {
				Resources.GetTexture(Hidden);
			});
		Builder.UseTexture(Pass, Declared, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_DEATH(Result.Graph->Execute(FRHICommandListImmediate::Get()),
			"undeclared texture");
	}

	TEST(FRenderGraphTests, ManagedAttachmentExitStateDrivesFollowingTransition)
	{
		FRHITexture Texture = MakeGraphTexture("Managed");
		FRenderGraphBuilder Builder;
		const auto Target = Builder.CreateTexture("Managed", &Texture);
		const auto Render = Builder.AddPass("Render", ERenderGraphPassType::Graphics);
		Builder.UseManagedColorAttachment(Render, Target, WholeColor(),
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		const auto Consume = Builder.AddPass("Consume", ERenderGraphPassType::Compute);
		Builder.UseTexture(Consume, Target, WholeColor(), ERenderGraphUse::Read,
			ERHIAccess::ComputeShaderRead);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		EXPECT_TRUE(Result.Graph->GetPasses()[0].TextureTransitions.empty());
		ASSERT_EQ(Result.Graph->GetPasses()[1].TextureTransitions.size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[1].TextureTransitions[0].ExpectedBefore,
			ERHIAccess::GraphicsShaderRead);
		EXPECT_EQ(Result.Graph->Capture().Transitions.size(), 3u);
	}

	TEST(FRenderGraphTests, IncompleteBackingPublicationRecordsNoCallback)
	{
		bool bExecuted = false;
		FRenderGraphBuilder Builder;
		Builder.SetBackingResolver([](auto, auto&, std::string&) { return true; });
		const auto Buffer = Builder.CreateBuffer("Logical",
			FRenderGraphBufferDesc{.Buffer = FRHIBufferDesc(
				64, 4, EBufferUsageFlags::UnorderedAccess)});
		const auto Pass = Builder.AddPass("Write", ERenderGraphPassType::Compute,
			[&](FRHICommandListImmediate&, const FRenderGraphPassResources&) {
				bExecuted = true;
			});
		Builder.UseBuffer(Pass, Buffer, 0, 64, ERenderGraphUse::Write,
			ERHIAccess::ComputeShaderReadWrite, true);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		std::string Error;
		EXPECT_FALSE(Result.Graph->Execute(FRHICommandListImmediate::Get(), &Error));
		EXPECT_FALSE(bExecuted);
		EXPECT_NE(Error.find("omitted retained resource"), std::string::npos);
	}

	TEST(FRenderGraphTests, ExplicitEffectRootSurvivesWithoutResourceOutputs)
	{
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Timestamp = Builder.AddPass(
			"Timestamp", ERenderGraphPassType::Graphics);
		Builder.MarkPassRoot(Timestamp, "timestamp");
		Builder.AddPass("Unused", ERenderGraphPassType::Graphics);
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 1u);
		EXPECT_EQ(Result.Graph->GetPasses()[0].Name, "Timestamp");
	}

	TEST(FRenderGraphTests, LogicalTokensDriveDependenciesAndLifetimesWithoutRHIState)
	{
		FRenderGraphBuilder Builder;
		Builder.EnablePassCulling();
		const auto Prepared = Builder.CreateToken("Prepared");
		const auto Output = Builder.CreateToken("Output");
		const auto Prepare = Builder.AddPass("Prepare", ERenderGraphPassType::Graphics);
		Builder.UseToken(Prepare, Prepared, ERenderGraphUse::Write);
		const auto Render = Builder.AddPass("Render", ERenderGraphPassType::Graphics);
		Builder.UseToken(Render, Prepared, ERenderGraphUse::Read);
		Builder.UseToken(Render, Output, ERenderGraphUse::Write);
		Builder.MarkPassRoot(Render, "present");
		auto Result = Builder.Compile();
		ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
		ASSERT_EQ(Result.Graph->GetPasses().size(), 2u);
		ASSERT_EQ(Result.Graph->GetDependencies().size(), 1u);
		EXPECT_EQ(Result.Graph->GetDependencies()[0].Cause, "Prepared");
		EXPECT_TRUE(Result.Graph->GetPasses()[0].BufferTransitions.empty());
		EXPECT_TRUE(Result.Graph->GetPasses()[0].TextureTransitions.empty());
		EXPECT_EQ(Result.Graph->GetResourceLifetimes()[0].FirstPass, 0u);
		EXPECT_EQ(Result.Graph->GetResourceLifetimes()[0].LastPass, 1u);
	}

	TEST(FRenderGraphTests, EnforcesDeterministicStructuralBudgets)
	{
		FRenderGraphBuilder Builder;
		Builder.SetBudget({.MaxPasses = 1});
		Builder.AddPass("First", ERenderGraphPassType::Graphics);
		Builder.AddPass("Second", ERenderGraphPassType::Graphics);
		auto Result = Builder.Compile();
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_EQ(Result.Error,
			"render graph budget exceeded: passes actual=2 limit=1");
	}

	TEST(FRenderGraphTests, CaptureOwnsPointerFreeDiagnosticsBeyondGraphLifetime)
	{
		FRenderGraphCapture Capture;
		{
			FRenderGraphBuilder Builder;
			Builder.EnablePassCulling();
			const auto Value = Builder.CreateToken("Value");
			const auto Produce = Builder.AddPass(
				"Produce", ERenderGraphPassType::Compute);
			Builder.UseToken(Produce, Value, ERenderGraphUse::Write);
			const auto Consume = Builder.AddPass(
				"Consume", ERenderGraphPassType::Graphics);
			Builder.UseToken(Consume, Value, ERenderGraphUse::Read);
			Builder.MarkPassRoot(Consume, "present");
			auto Result = Builder.Compile();
			ASSERT_TRUE(Result.IsSuccess()) << Result.Error;
			Capture = Result.Graph->Capture();
			EXPECT_EQ(Capture.Dump, Result.Graph->Dump());
		}
		ASSERT_EQ(Capture.Passes.size(), 2u);
		EXPECT_EQ(Capture.Passes[0].Name, "Produce");
		EXPECT_EQ(Capture.Statistics.DeclaredPasses, 2u);
		EXPECT_EQ(Capture.Statistics.ScheduledPasses, 2u);
		EXPECT_EQ(Capture.Statistics.Dependencies, 1u);
		EXPECT_EQ(Capture.Dependencies[0].Cause, "Value");
		EXPECT_NE(Capture.Dump.find("name=Consume"), std::string::npos);
	}
} // namespace Durin

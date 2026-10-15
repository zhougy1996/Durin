#include "MetalBindingPlan.h"
#include "MetalShaderFixture.h"
#include <gtest/gtest.h>

namespace Durin
{
	namespace
	{
		class FBindingTestResource final : public FRHIResource
		{
		public:
			FBindingTestResource() : FRHIResource(ERHIResourceType::Sampler) {}
		};
		struct FFixture
		{
			FPipelineLayoutDesc Layout;
			std::array<ShaderFormat::FMetalBindingMap, 2> Maps;
			std::shared_ptr<const FMetalBindingPlan> Plan;
			FFixture()
			{
				const auto Both = EShaderStageFlags::Vertex | EShaderStageFlags::Fragment;
				Layout.BindingLayouts.resize(8);
				Layout.BindingLayouts[7].BindingLayouts = {{Both, 65000, ERHIBindingType::UniformBufferDynamic, 2},
					{EShaderStageFlags::Fragment, 65535, ERHIBindingType::Sampler, 1}};
				Layout.PushConstantRanges = {{Both, 4, 4}, {Both, 12, 8}};
				const std::vector<ShaderFormat::FLogicalBinding> Logical{{7, Layout.BindingLayouts[7].BindingLayouts[0]},
					{7, Layout.BindingLayouts[7].BindingLayouts[1]}};
				Maps[0] = *ShaderFormat::BuildMetalBindingMap(EShaderFrequency::Vertex, std::span(Logical).first(1), true);
				Maps[1] = *ShaderFormat::BuildMetalBindingMap(EShaderFrequency::Fragment, Logical, true);
				Plan = Prepare();
			}
			auto Prepare() const -> std::shared_ptr<const FMetalBindingPlan>
			{
				const std::array Stages{
					FMetalStageBindings{EShaderFrequency::Vertex, Maps[0].Bindings, Maps[0].PushConstantBufferSlot},
					FMetalStageBindings{EShaderFrequency::Fragment, Maps[1].Bindings, Maps[1].PushConstantBufferSlot}};
				return PrepareMetalBindings(Layout, Stages);
			}
		};
	}

	TEST(FMetalBindingPlanTests, SparseArraysFanOutToCompilerAgreedStageSlots)
	{
		FFixture Fixture;
		ASSERT_TRUE(Fixture.Plan);
		EXPECT_EQ(Fixture.Plan->Parameters.size(), 3u);
		ASSERT_EQ(Fixture.Plan->Operations.size(), 5u);
		EXPECT_EQ(Fixture.Plan->Operations[0].NativeSlot, 16u);
		EXPECT_EQ(Fixture.Plan->Operations[1].NativeSlot, 17u);
		EXPECT_EQ(Fixture.Plan->Operations[2].ParameterIndex, 0u);
		EXPECT_EQ(Fixture.Plan->Operations[2].NativeSlot, 0u);
		EXPECT_EQ(Fixture.Plan->Operations[4].NativeSlot, 0u);
		EXPECT_EQ(Fixture.Plan->Push[0].NativeSlot, 18u);
		EXPECT_EQ(Fixture.Plan->Push[1].NativeSlot, 2u);
		EXPECT_EQ(Fixture.Plan->Push[0].Extent, 20u);
		EXPECT_LT(Fixture.Plan->RetainedBytes(), 4096u);
	}

	TEST(FMetalBindingPlanTests, FailedPreparationNeverPublishesPartialOperations)
	{
		FFixture Fixture;
		Fixture.Maps[0].Bindings[0].Count = 1;
		EXPECT_FALSE(Fixture.Prepare());
		Fixture.Maps[0].Bindings[0].Count = 2;
		Fixture.Maps[0].Bindings[0].Slot = 0;
		EXPECT_FALSE(Fixture.Prepare());
		Fixture.Maps[0].Bindings[0].Slot = 16;
		Fixture.Maps[0].PushConstantBufferSlot = UINT32_MAX;
		EXPECT_FALSE(Fixture.Prepare());
		Fixture.Maps[0].PushConstantBufferSlot = 18;
		Fixture.Layout.BindingLayouts[7].BindingLayouts[0].ArraySize = 65537;
		EXPECT_FALSE(Fixture.Prepare());
		Fixture.Layout.BindingLayouts[7].BindingLayouts[0].ArraySize = 2;
		Fixture.Layout.PushConstantRanges[0].Size = 65536;
		EXPECT_FALSE(Fixture.Prepare());
	}

	TEST(FMetalBindingPlanTests, DenseUpdatesRetainOwnersAndResetOnPipelineReplacement)
	{
		FFixture Fixture;
		FMetalParameterState State;
		ASSERT_TRUE(Fixture.Plan);
		State.Reset(Fixture.Plan);
		EXPECT_FALSE(State.IsComplete());
		auto Resource = MakeRefCount<FBindingTestResource>();
		auto Other = MakeRefCount<FBindingTestResource>();
		FRHIShaderParameterResource Parameter{Resource.GetReference(), 7, 65000, 0, ERHIBindingType::UniformBufferDynamic, 256, 64};
		const auto Index = State.Find(Parameter, EShaderStageFlags::Vertex);
		ASSERT_TRUE(Index);
		State.Write(*Index, Parameter);
		EXPECT_EQ(Resource->GetRefCount(), 2u);
		Parameter.Offset = 512; Parameter.Resource = Other.GetReference();
		State.Write(*Index, Parameter);
		EXPECT_EQ(Resource->GetRefCount(), 1u);
		EXPECT_EQ(Other->GetRefCount(), 2u);
		EXPECT_EQ(State.Get(*Index).Offset, 512u);
		Parameter.ArrayElement = 1;
		State.Write(*State.Find(Parameter, EShaderStageFlags::Fragment), Parameter);
		Parameter.Type = ERHIBindingType::Sampler; Parameter.ArrayElement = 0; Parameter.BindingIndex = 65535;
		EXPECT_FALSE(State.Find(Parameter, EShaderStageFlags::Vertex));
		State.Write(*State.Find(Parameter, EShaderStageFlags::Fragment), Parameter);
		EXPECT_TRUE(State.IsComplete());
		Parameter.ArrayElement = 1;
		EXPECT_FALSE(State.Find(Parameter, EShaderStageFlags::Fragment));
		State.Reset(Fixture.Prepare());
		EXPECT_FALSE(State.IsComplete());
		EXPECT_EQ(Other->GetRefCount(), 1u);
		State.Reset();
		EXPECT_FALSE(State.IsComplete());
	}

	TEST(FMetalBindingPlanTests, PushCoverageRejectsHolesAndTracksPartialWritesWithoutSearchingAtSubmit)
	{
		FFixture Fixture;
		FMetalPushState State;
		ASSERT_TRUE(Fixture.Plan);
		State.Reset(&Fixture.Plan->Push[0]);
		EXPECT_FALSE(State.IsComplete());
		const std::array<uint32, 3> Data{1, 2, 3};
		EXPECT_FALSE(State.Write(0, 4, Data.data()));
		EXPECT_FALSE(State.Write(4, 12, Data.data()));
		EXPECT_FALSE(State.Write(UINT32_MAX, 4, Data.data()));
		EXPECT_TRUE(State.Write(4, 4, Data.data()));
		EXPECT_TRUE(State.Write(12, 4, Data.data() + 1));
		EXPECT_FALSE(State.IsComplete());
		EXPECT_TRUE(State.Write(16, 4, Data.data() + 2));
		EXPECT_TRUE(State.IsComplete());
		EXPECT_TRUE(State.Write(16, 4, Data.data()));
		EXPECT_TRUE(State.IsComplete());
		State.Reset(&Fixture.Plan->Push[0]);
		EXPECT_FALSE(State.IsComplete());
	}

	TEST(FMetalBindingPlanTests, FirstAndRepeatedSubmissionOnlyVisitPreparedOperations)
	{
		FFixture Fixture;
		ASSERT_TRUE(Fixture.Plan);
		FMetalParameterState State;
		State.Reset(Fixture.Plan);
		auto Resource = MakeRefCount<FBindingTestResource>();
		for (const auto& Slot : Fixture.Plan->Parameters)
		{
			FRHIShaderParameterResource Value{Resource.GetReference(), Slot.SetIndex, Slot.BindingIndex, Slot.ArrayElement, Slot.Type};
			State.Write(*State.Find(Value, EShaderStageFlags::Fragment), Value);
		}
		const auto Before = State.GetCounters();
		const auto FormatBefore = ShaderFormat::GetMetalFormatWorkCounters();
		uint64 Executed = 0;
		for (uint32 Submission = 0; Submission < 100; ++Submission)
		{
			ASSERT_TRUE(State.IsComplete());
			for (const auto& Operation : State.OperationsForSubmission())
			{
				EXPECT_EQ(State.Get(Operation.ParameterIndex).Resource, Resource.GetReference());
				++Executed;
			}
		}
		const auto After = State.GetCounters();
		EXPECT_EQ(After.ParameterLookups, Before.ParameterLookups);
		EXPECT_EQ(After.MappingAllocations, Before.MappingAllocations);
		EXPECT_EQ(After.SubmissionBatches - Before.SubmissionBatches, 100u);
		EXPECT_EQ(After.SubmittedOperations - Before.SubmittedOperations, Executed);
		EXPECT_EQ(Executed, 500u);
		EXPECT_EQ(ShaderFormat::GetMetalFormatWorkCounters().MapBuilds, FormatBefore.MapBuilds);
		EXPECT_EQ(ShaderFormat::GetMetalFormatWorkCounters().MetadataDecodes, FormatBefore.MetadataDecodes);
		RecordProperty("submission_batches", int(After.SubmissionBatches - Before.SubmissionBatches));
		RecordProperty("submitted_operations", int(Executed));
		RecordProperty("submission_parameter_lookups", int(After.ParameterLookups - Before.ParameterLookups));
		RecordProperty("submission_mapping_allocations", int(After.MappingAllocations - Before.MappingAllocations));
		RecordProperty("submission_map_builds", int(ShaderFormat::GetMetalFormatWorkCounters().MapBuilds - FormatBefore.MapBuilds));
		RecordProperty("submission_metadata_decodes", int(ShaderFormat::GetMetalFormatWorkCounters().MetadataDecodes - FormatBefore.MetadataDecodes));
		RecordProperty("prepared_plan_retained_bytes", int(Fixture.Plan->RetainedBytes()));
	}

	TEST(FMetalBindingPlanTests, ComputeStorageArraysUseDenseStateAcrossEncoderReemission)
	{
		FPipelineLayoutDesc Layout;
		Layout.BindingLayouts.resize(2);
		Layout.BindingLayouts[1].BindingLayouts = {{EShaderStageFlags::Compute, 65535, ERHIBindingType::StorageImage, 2}};
		const std::array Logical{ShaderFormat::FLogicalBinding{1, Layout.BindingLayouts[1].BindingLayouts[0]}};
		const auto Map = ShaderFormat::BuildMetalBindingMap(EShaderFrequency::Compute, Logical, false);
		ASSERT_TRUE(Map);
		const std::array Stages{FMetalStageBindings{EShaderFrequency::Compute, Map->Bindings, Map->PushConstantBufferSlot}};
		const auto Plan = PrepareMetalBindings(Layout, Stages);
		ASSERT_TRUE(Plan);
		ASSERT_EQ(Plan->Operations.size(), 2u);
		FMetalParameterState State;
		State.Reset(Plan);
		auto Resource = MakeRefCount<FBindingTestResource>();
		FRHIShaderParameterResource Value{Resource.GetReference(), 1, 65535, 0, ERHIBindingType::StorageImage};
		for (uint32 Element = 0; Element < 2; ++Element)
		{
			Value.ArrayElement = Element;
			EXPECT_FALSE(State.Find(Value, EShaderStageFlags::Fragment));
			State.Write(*State.Find(Value, EShaderStageFlags::Compute), Value);
		}
		ASSERT_TRUE(State.IsComplete());
		// A fresh native encoder replays every prepared operation without rebuilding state.
		for (uint32 Encoder = 0; Encoder < 2; ++Encoder)
		{
			uint32 ExpectedSlot = 0;
			for (const auto& Operation : State.OperationsForSubmission())
			{
				EXPECT_EQ(Operation.Stage, EShaderStageFlags::Compute);
				EXPECT_EQ(Operation.NativeSlot, ExpectedSlot++);
				EXPECT_EQ(State.Get(Operation.ParameterIndex).ArrayElement, Operation.NativeSlot);
			}
			EXPECT_EQ(ExpectedSlot, 2u);
		}
	}

	TEST(FMetalBindingPlanTests, NativeFixtureFinalizesOwnedArtifactsAndRejectsCorruptMaps)
	{
		const std::string Source = "kernel void mainA() {}";
		const auto Code = std::as_bytes(std::span(Source));
		auto Fixture = FMetalShaderFixture::Create("Fixture", EShaderFrequency::Compute, Code, FXxHash128::HashBuffer(Code));
		Fixture.Target = MetalShaderTarget; Fixture.CodeFormat = EShaderCodeFormat::Msl20Source;
		Fixture.ComputeThreadGroupSize = {1, 1, 1}; Fixture.SetEntryPoint("mainA");
		Fixture.RemapIdentity = ComputeMetalBindingRemapIdentity(Fixture.Frequency, {}, UINT32_MAX);
		auto Desc = FinalizeMetalFixture(Fixture);
		EXPECT_TRUE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Fixture.RemapIdentity.HashLow ^= 1;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(FinalizeMetalFixture(Fixture)));
		Fixture.ComputeThreadGroupSize = {};
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(FinalizeMetalFixture(Fixture)));
		EXPECT_TRUE(ShaderFormat::ValidateShaderCreateDesc(Desc));
	}
}

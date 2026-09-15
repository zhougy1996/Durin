#pragma once

#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialTestSupport.h"
#include "Materials/MaterialCookedProgram.h"

#include <iostream>

namespace Durin::Testing
{
	// Shares variant correctness assertions; payload sizing is opt-in qualification work.
	inline auto CheckInstanceVariantsForTest(bool bMeasurePayloads) -> void
	{
		struct FFixtureScope
		{
			std::vector<Durin::DObject*> Objects;
			~FFixtureScope()
			{
				for (auto* Object : Objects) Durin::MarkAsGarbage(Object);
				Durin::CollectGarbage();
			}
		} Scope;
		auto* Root = Durin::NewObject<Durin::DMaterial>(nullptr, "VariantFixtureRoot");
		Scope.Objects.push_back(Root);
		ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Root));
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Root);
		ASSERT_TRUE(Root->GetMaterialCompileStatus().IsCurrent());
		Durin::FMaterialIRCompilerInput Input;
		Durin::FMaterialCompilerEnvironment Environment;
		std::string Error;
		ASSERT_TRUE(Durin::BuildDefaultMaterialCompilerEnvironment(Environment, Error)) << Error;
		ASSERT_TRUE(Durin::SnapshotMaterialCompilerInput(*Root, Environment, Input));

		const auto Before = Durin::GetMaterialCompilationDiagnostics();
		std::array<Durin::DMaterialInstance*, 8> Instances{};
		std::vector<Durin::FMaterialProgramIdentity> Identities;
		uint32 CompatibleOwners = 0;
		for (size_t Index = 0; Index < Instances.size(); ++Index)
		{
			auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
				nullptr, Durin::FName(std::format("VariantFixture{}", Index)));
			Scope.Objects.push_back(Instance);
			Instances[Index] = Instance;
			ASSERT_TRUE(Instance->SetParent(Index == 7 ? Instances[6]
				: static_cast<Durin::DMaterialInterface*>(Root)));
			auto Properties = Root->GetStaticProperties();
			if (Index == 1) Properties.OpacityMaskThreshold = 0.25f;
			if (Index == 2 || Index == 3)
			{
				Properties.BlendMode = Durin::EMaterialBlendMode::Masked;
				Properties.OpacityMaskThreshold = Index == 2 ? 0.25f : 0.75f;
			}
			if (Index == 4) Properties.BlendMode = Durin::EMaterialBlendMode::Translucent;
			if (Index == 5)
			{
				Properties.bTwoSided = true;
				Properties.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Disabled;
			}
			if (Index != 0 && Index != 7)
				ASSERT_TRUE(Instance->SetPropertyOverrides({true, true, true, true, true, Properties}));
			Input.StaticProperties = Instance->GetStaticProperties();
			const auto Normalized = Durin::NormalizeMaterialIR(Input);
			ASSERT_TRUE(Normalized);
			if (std::ranges::find(Identities, Normalized.Identity) == Identities.end())
				Identities.push_back(Normalized.Identity);
			// Count complete accepted owners only after aggregate finish below.
		}
		ASSERT_TRUE(Instances[7]->SetParent(Instances[2]));
		EXPECT_EQ(Instances[7]->GetStaticProperties().BlendMode, Durin::EMaterialBlendMode::Masked);
		ASSERT_TRUE(Instances[7]->SetParent(Instances[6]));
		ASSERT_TRUE(Instances[0]->SetScalarParameterValue(Durin::MaterialParameters::MetallicName(), 0.7f));
		Durin::FAssetCompilingManager::Get().FinishAllCompilation();
		const auto After = Durin::GetMaterialCompilationDiagnostics();
		for (auto* Instance : Instances)
			if (Instance->GetAcceptedCompiledProgram()) ++CompatibleOwners;
		EXPECT_EQ(Identities.size(), 4u);
		EXPECT_EQ(CompatibleOwners, 8u);
		EXPECT_GT(After.AcceptedRequests - Before.AcceptedRequests, 0u);
		EXPECT_EQ(After.InFlightCount, 0u);
		EXPECT_EQ(After.OutstandingConsumerCount, 0u);
		EXPECT_EQ(After.PendingPublicationCount, 0u);
		if (!bMeasurePayloads) return;
		uint64 InstancePayloadBytes = 0;
		for (auto* Instance : Instances)
		{
			Durin::FByteBuffer Payload;
			ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(*Instance->GetAcceptedCompiledProgram(),
				Instance->GetRenderableStaticProperties(), Durin::ECookTargetPlatform::Win64,
				Durin::ECookTargetProfile::Game, Payload, Error)) << Error;
			InstancePayloadBytes += Payload.size();
		}
		Durin::FByteBuffer Bytes;
		ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(*Root->GetAcceptedCompiledProgram(),
			Root->GetRenderableStaticProperties(), Durin::ECookTargetPlatform::Win64,
			Durin::ECookTargetProfile::Game, Bytes, Error)) << Error;
		std::cout << "Variant baseline: owners=" << Instances.size()
			<< " effective_identities=" << Identities.size()
			<< " compatible_instances=" << CompatibleOwners
			<< " instance_requests=" << After.AcceptedRequests - Before.AcceptedRequests
			<< " total_requests=" << After.AcceptedRequests
			<< " completed_requests=" << After.CompletedRequests
			<< " retained_programs=" << After.RetainedProgramCount
			<< " retained_bytes=" << After.RetainedProgramBytes
			<< " instance_dmat_bytes=" << InstancePayloadBytes
			<< " root_dmat_bytes=" << Bytes.size() << '\n';
	}
}

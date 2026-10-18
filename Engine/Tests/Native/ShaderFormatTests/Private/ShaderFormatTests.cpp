#include "MetalShaderFormat.h"
#include "VulkanShaderFormat.h"
#include <gtest/gtest.h>

namespace Durin
{
	TEST(FShaderFormatTests, BinaryMetalProfileRequiresLibraryAndPreservesBindingContract)
	{
		ASSERT_TRUE(IsMetalShaderTarget(MetalLibraryShaderTarget));
		EXPECT_NE(MetalLibraryShaderTarget, MetalShaderTarget);
		ASSERT_TRUE(ShaderFormat::ResolveMetalCompileSettings(MetalLibraryShaderTarget));
		auto Unknown = MetalLibraryShaderTarget;
		Unknown.SettingsDigest.HashLow ^= 1;
		EXPECT_FALSE(IsMetalShaderTarget(Unknown));

		FByteBuffer Bytes(88);
		Bytes[0] = std::byte{'M'}; Bytes[1] = std::byte{'T'};
		Bytes[2] = std::byte{'L'}; Bytes[3] = std::byte{'B'};
		Bytes[16] = std::byte{88};
		FShaderCode Shader;
		Shader.Target = MetalLibraryShaderTarget;
		Shader.CodeFormat = EShaderCodeFormat::MetalLibrary;
		Shader.Frequency = EShaderFrequency::Fragment;
		// An opaque library need not contain the entry name as plain text.
		Shader.BinaryEntryPoint = "fragment_entry_not_in_bytes";
		Shader.Code = FSharedByteBuffer::Copy(Bytes);
		Shader.CodeHash = FXxHash128::HashBuffer(Shader.Code);
		Shader.Reflection.ResourceBindings.push_back({.Name = "Texture", .StageFlags = EShaderStageFlags::Fragment,
			.SetIndex = 2, .BindingIndex = 31, .Type = ERHIBindingType::Texture, .ArraySize = 2});
		const auto Bindings = ShaderFormat::BuildLogicalBindings(Shader.Reflection);
		const auto Map = ShaderFormat::BuildMetalBindingMap(Shader.Frequency, Bindings, false);
		ASSERT_TRUE(Map);
		const auto Metadata = ShaderFormat::EncodeMetalMetadata(Shader.Frequency, *Map);
		ASSERT_TRUE(Metadata);
		Shader.PlatformData = *Metadata;
		ASSERT_TRUE(ShaderFormat::ValidateShaderCode(Shader));
		const auto Identity = ShaderFormat::ComputeShaderIdentity(Shader);
		Shader.CodeFormat = EShaderCodeFormat::Msl20Source;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCode(Shader));
		EXPECT_NE(ShaderFormat::ComputeShaderIdentity(Shader), Identity);
		Shader.CodeFormat = EShaderCodeFormat::MetalLibrary;
		Shader.Target = MetalShaderTarget;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCode(Shader));
		Shader.Target = MetalLibraryShaderTarget;
		Shader.Reflection.ResourceBindings[0].ArraySize = 3;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCode(Shader));
		for (size_t Size = 0; Size < Bytes.size(); ++Size)
			EXPECT_FALSE(ShaderFormat::ValidateMetalLibrary(FByteView(Bytes).first(Size)));
		Bytes[16] = std::byte{87};
		EXPECT_FALSE(ShaderFormat::ValidateMetalLibrary(Bytes));
		Bytes[16] = std::byte{88}; Bytes[0] = std::byte{0};
		EXPECT_FALSE(ShaderFormat::ValidateMetalLibrary(Bytes));
	}

	TEST(FShaderFormatTests, CanonicalMetalCodecRoundTripsSparseArraysForEveryStage)
	{
		for (const auto Frequency : {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute})
		{
			std::vector<ShaderFormat::FLogicalBinding> Bindings{
				{7, {(EShaderStageFlags::Vertex | EShaderStageFlags::Fragment | EShaderStageFlags::Compute), 65000, ERHIBindingType::Texture, 2}},
				{0, {(EShaderStageFlags::Vertex | EShaderStageFlags::Fragment | EShaderStageFlags::Compute), 99, ERHIBindingType::UniformBufferDynamic}},
				{7, {(EShaderStageFlags::Vertex | EShaderStageFlags::Fragment | EShaderStageFlags::Compute), 65535, ERHIBindingType::Sampler, 2}}};
			const auto Map = ShaderFormat::BuildMetalBindingMap(Frequency, Bindings, true);
			ASSERT_TRUE(Map);
			EXPECT_EQ(Map->Bindings[0].Slot, Frequency == EShaderFrequency::Vertex ? 16u : 0u);
			EXPECT_EQ(Map->Bindings[1].Slot, 0u);
			EXPECT_EQ(Map->Bindings[2].Slot, 0u);
			EXPECT_EQ(Map->PushConstantBufferSlot, Frequency == EShaderFrequency::Vertex ? 17u : 1u);
			std::ranges::reverse(Bindings);
			const auto Reordered = ShaderFormat::BuildMetalBindingMap(Frequency, Bindings, true);
			ASSERT_TRUE(Reordered);
			EXPECT_EQ(Map->Bindings, Reordered->Bindings);
			EXPECT_EQ(Map->Identity, Reordered->Identity);
			const auto Bytes = ShaderFormat::EncodeMetalMetadata(Frequency, *Map);
			ASSERT_TRUE(Bytes);
			const auto Decoded = ShaderFormat::DecodeMetalMetadata(Frequency, Bytes->GetBytes());
			ASSERT_TRUE(Decoded);
			EXPECT_EQ(Decoded->Bindings, Map->Bindings);
			EXPECT_EQ(Decoded->Identity, Map->Identity);
			EXPECT_EQ(Decoded->PushConstantBufferSlot, Map->PushConstantBufferSlot);
			const auto EncodedAgain = ShaderFormat::EncodeMetalMetadata(Frequency, *Decoded);
			ASSERT_TRUE(EncodedAgain);
			EXPECT_TRUE(std::ranges::equal(Bytes->GetBytes(), EncodedAgain->GetBytes()));
		}
	}

	TEST(FShaderFormatTests, RejectsEveryTruncationCorruptFieldAndWrongStage)
	{
		const std::array<ShaderFormat::FLogicalBinding, 1> Bindings{
			ShaderFormat::FLogicalBinding{0, {EShaderStageFlags::Compute, 8, ERHIBindingType::StorageBuffer}}};
		const auto Map = ShaderFormat::BuildMetalBindingMap(EShaderFrequency::Compute, Bindings, true);
		ASSERT_TRUE(Map);
		const auto Encoded = ShaderFormat::EncodeMetalMetadata(EShaderFrequency::Compute, *Map);
		ASSERT_TRUE(Encoded);
		for (size_t Size = 0; Size < Encoded->size(); ++Size)
			EXPECT_FALSE(ShaderFormat::DecodeMetalMetadata(EShaderFrequency::Compute, Encoded->GetBytes().first(Size)));
		for (const size_t Offset : {0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u, 36u})
		{
			FByteBuffer Corrupt(Encoded->begin(), Encoded->end());
			Corrupt[Offset] ^= std::byte{0x80};
			EXPECT_FALSE(ShaderFormat::DecodeMetalMetadata(EShaderFrequency::Compute, Corrupt));
		}
		FByteBuffer Trailing(Encoded->begin(), Encoded->end()); Trailing.push_back(std::byte{0});
		EXPECT_FALSE(ShaderFormat::DecodeMetalMetadata(EShaderFrequency::Compute, Trailing));
		EXPECT_FALSE(ShaderFormat::DecodeMetalMetadata(EShaderFrequency::Vertex, Encoded->GetBytes()));
	}

	TEST(FShaderFormatTests, RejectsLogicalCollisionsOverflowAndArtifactInterfaceMismatch)
	{
		std::vector<ShaderFormat::FLogicalBinding> Bindings{
			{0, {EShaderStageFlags::Compute, 65535, ERHIBindingType::Texture, 128}}};
		EXPECT_TRUE(ShaderFormat::BuildMetalBindingMap(EShaderFrequency::Compute, Bindings, false));
		Bindings[0].Declaration.ArraySize = 129;
		EXPECT_FALSE(ShaderFormat::BuildMetalBindingMap(EShaderFrequency::Compute, Bindings, false));
		Bindings[0].Declaration.ArraySize = 1;
		Bindings.push_back({0, {EShaderStageFlags::Compute, 65535, ERHIBindingType::Sampler}});
		EXPECT_FALSE(ShaderFormat::BuildMetalBindingMap(EShaderFrequency::Compute, Bindings, false));
		Bindings.pop_back();
		const auto Map = ShaderFormat::BuildMetalBindingMap(EShaderFrequency::Compute, Bindings, false);
		ASSERT_TRUE(Map);
		const auto Bytes = ShaderFormat::EncodeMetalMetadata(EShaderFrequency::Compute, *Map);
		ASSERT_TRUE(Bytes);
		const std::string Source = "#include <metal_stdlib>\nkernel void main0() {}\n";
		const auto Code = std::as_bytes(std::span(Source));
		EXPECT_TRUE(ShaderFormat::ValidateShaderCodeParts(MetalShaderTarget, EShaderCodeFormat::Msl20Source, 1,
			EShaderFrequency::Compute, Code, "main0", Bytes->GetBytes(), Bindings, {}));
		Bindings[0].Declaration.ArraySize = 2;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCodeParts(MetalShaderTarget, EShaderCodeFormat::Msl20Source, 1,
			EShaderFrequency::Compute, Code, "main0", Bytes->GetBytes(), Bindings, {}));
		EXPECT_FALSE(ShaderFormat::ValidateShaderCodeParts(MetalShaderTarget, EShaderCodeFormat::Msl20Source, 99,
			EShaderFrequency::Compute, Code, "main0", Bytes->GetBytes(), Bindings, {}));
	}

	TEST(FShaderFormatTests, IdentityCoversExecutionDataAndOwnedCreationRejectsDivergence)
	{
		const std::array<uint32, 5> Words{0x07230203u, 0x00010500u, 0, 1, 0};
		FShaderCode Value;
		Value.BinaryEntryPoint = "main";
		Value.Code = FSharedByteBuffer::Copy(std::as_bytes(std::span(Words)));
		Value.CodeHash = FXxHash128::HashBuffer(Value.Code);
		Value.ShaderIdentity = ShaderFormat::ComputeShaderIdentity(Value);
		const auto Identity = Value.ShaderIdentity;
		Value.BinaryEntryPoint = "alternate";
		EXPECT_NE(ShaderFormat::ComputeShaderIdentity(Value), Identity);
		Value.BinaryEntryPoint = "main";
		Value.PlatformDataVersion = 2;
		EXPECT_NE(ShaderFormat::ComputeShaderIdentity(Value), Identity);
		Value.PlatformDataVersion = 1;
		Value.PlatformData = FSharedByteBuffer::Copy(std::as_bytes(std::span(Words)).first(4));
		EXPECT_NE(ShaderFormat::ComputeShaderIdentity(Value), Identity);
		Value.PlatformData = {};
		auto Artifact = std::make_shared<const FShaderCode>(std::move(Value));
		auto Desc = FRHIShaderCreateDesc::CreateVertex("OwnedArtifact", Artifact->Code.GetBytes(), FXxHash128::HashBuffer(Artifact->Code.GetBytes()));
		Desc.CodeOwner = Artifact;
		Desc.SetEntryPoint(Artifact->BinaryEntryPoint.c_str());
		Artifact.reset();
		EXPECT_TRUE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		const auto Owned = Desc;
		Desc.Hash.HashLow ^= 1;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Owned; Desc.Code = Desc.Code.first(4);
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Owned; Desc.EntryPoint = nullptr;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Owned;
		auto CorruptOwner = *Owned.CodeOwner;
		CorruptOwner.CodeHash.HashLow ^= 1;
		Desc.Hash = CorruptOwner.CodeHash;
		Desc.CodeOwner = std::make_shared<const FShaderCode>(std::move(CorruptOwner));
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Owned;
		Desc.Frequency = EShaderFrequency::Fragment;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		auto UnknownTarget = MetalShaderTarget;
		UnknownTarget.SettingsDigest.HashHigh ^= 1;
		EXPECT_FALSE(ShaderFormat::ResolveMetalCompileSettings(UnknownTarget));
		const auto Settings = ShaderFormat::ResolveMetalCompileSettings(MetalShaderTarget);
		ASSERT_TRUE(Settings);
		EXPECT_EQ(Settings->MslLanguageVersion, 200u);
		EXPECT_EQ(Settings->BindingRemapSchema, 1u);
	}

	TEST(FShaderFormatTests, RawCreationValidatesCodeHashStageAndEntryPoint)
	{
		const std::array<uint32, 5> Words{0x07230203u, 0x00010500u, 0, 1, 0};
		const auto Code = std::as_bytes(std::span(Words));
		const auto Valid = FRHIShaderCreateDesc::CreateVertex("Raw", Code, FXxHash128::HashBuffer(Code));
		ASSERT_TRUE(ShaderFormat::ValidateShaderCreateDesc(Valid));
		auto Desc = Valid;
		Desc.Hash.HashLow ^= 1;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Valid; Desc.Frequency = EShaderFrequency(UINT32_MAX);
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Valid; Desc.EntryPoint = "";
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Valid; Desc.EntryPoint = nullptr;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Valid; Desc.Code = Code.first(4);
		Desc.Hash = FXxHash128::HashBuffer(Desc.Code);
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Valid; Desc.ComputeThreadGroupSize = {1, 1, 1};
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Valid; Desc.Target.SettingsDigest.HashLow = 1;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Desc = Valid; Desc.ShaderIdentity.HashLow = 1;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
	}

	TEST(FShaderFormatTests, RehashedArtifactsStillRejectInvalidLogicalInterfaces)
	{
		const std::array<uint32, 5> Words{0x07230203u, 0x00010500u, 0, 1, 0};
		FShaderCode Value;
		Value.BinaryEntryPoint = "main"; Value.Frequency = EShaderFrequency::Fragment;
		Value.Code = FSharedByteBuffer::Copy(std::as_bytes(std::span(Words)));
		Value.CodeHash = FXxHash128::HashBuffer(Value.Code);
		FPipelineLayoutDesc Layout;
		Layout.BindingLayouts.resize(1);
		Layout.BindingLayouts[0].BindingLayouts = {{EShaderStageFlags::Fragment, 65000, ERHIBindingType::Texture, 2}};
		Layout.PushConstantRanges = {{EShaderStageFlags::Fragment, 0, 16}};
		const auto Create = [&](const FPipelineLayoutDesc& Interface) {
			auto Copy = Value;
			Copy.Reflection = {};
			for (uint32 Set = 0; Set < Interface.BindingLayouts.size(); ++Set)
			for (const auto& Binding : Interface.BindingLayouts[Set].BindingLayouts)
				Copy.Reflection.ResourceBindings.push_back({"", Binding.StageFlags, Set, Binding.Slot, Binding.Type, Binding.ArraySize});
			Copy.Reflection.PushConstantRanges = Interface.PushConstantRanges;
			Copy.ShaderIdentity = ShaderFormat::ComputeShaderIdentity(Copy);
			auto Desc = FRHIShaderCreateDesc::CreateFragment("Checked", Copy.Code.GetBytes(), Copy.CodeHash);
			Desc.ShaderIdentity = Copy.ShaderIdentity;
			Desc.CodeOwner = std::make_shared<const FShaderCode>(std::move(Copy));
			return Desc;
		};
		ASSERT_TRUE(ShaderFormat::ValidateShaderCreateDesc(Create(Layout)));
		const auto Reject = [&](const auto& Mutate) {
			auto Invalid = Layout; Mutate(Invalid);
			EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Create(Invalid)));
		};
		Reject([](auto& L) { L.BindingLayouts[0].BindingLayouts[0].Type = ERHIBindingType(UINT32_MAX); });
		Reject([](auto& L) { L.BindingLayouts[0].BindingLayouts[0].StageFlags = EShaderStageFlags::None; });
		Reject([](auto& L) { L.BindingLayouts[0].BindingLayouts[0].StageFlags = EShaderStageFlags::Vertex; });
		Reject([](auto& L) { L.BindingLayouts[0].BindingLayouts[0].ArraySize = 0; });
		Reject([](auto& L) { L.BindingLayouts[0].BindingLayouts[0].Slot = 65536; });
		Reject([](auto& L) { L.BindingLayouts[0].BindingLayouts.push_back(L.BindingLayouts[0].BindingLayouts[0]); });
		Reject([](auto& L) { L.PushConstantRanges[0].StageFlags = EShaderStageFlags::Compute; });
		Reject([](auto& L) { L.PushConstantRanges[0].Size = 0; });
		Reject([](auto& L) { L.PushConstantRanges[0].Offset = UINT32_MAX; });
		Reject([](auto& L) { L.PushConstantRanges[0].Offset = 2; });
		Reject([](auto& L) { L.PushConstantRanges.push_back({EShaderStageFlags::Fragment, 8, 4}); });
		auto Desc = Create(Layout); Desc.ShaderIdentity.HashLow ^= 1;
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Desc));
		Value.BinaryEntryPoint = std::string("main\0hidden", 11);
		EXPECT_FALSE(ShaderFormat::ValidateShaderCreateDesc(Create(Layout)));
		const std::array Bindings{ShaderFormat::FLogicalBinding{0, Layout.BindingLayouts[0].BindingLayouts[0]}};
		EXPECT_FALSE(ShaderFormat::ValidateShaderCodeParts(Value.Target, Value.CodeFormat, Value.PlatformDataVersion,
			Value.Frequency, Value.Code.GetBytes(), Value.BinaryEntryPoint, {}, Bindings, Layout.PushConstantRanges));
	}
}

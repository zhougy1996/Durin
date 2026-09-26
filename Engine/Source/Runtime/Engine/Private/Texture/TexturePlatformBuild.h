#pragma once

#if DURIN_WITH_EDITOR
#include "Asset/AssetDerivedDataBuild.h"
#include "TexturePlatformCodec.h"
#include "Texture/TextureBuildOutcome.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/TextureSource.h"

namespace Durin::TexturePrivate
{
	// Typed family codecs and snapshot resolution; the executor owns all cache flow.
	template <typename TPlatform, typename TResolve, typename TBuild>
	struct TTexturePlatformBuildAdapter
	{
		using FProduct = std::unique_ptr<TPlatform>;
		using FError = FTextureBuildError;
		const FTextureSource& Source;
		DerivedData::FBuildFunctionDescriptor Function;
		std::vector<DerivedData::FBuildInputReference> Inputs;
		ECookTargetProfile TargetProfile;
		TResolve ResolveSource;
		TBuild BuildProduct;

		auto GetFunction() const -> DerivedData::FBuildFunctionDescriptor { return Function; }
		auto GetInputs() const -> std::span<const DerivedData::FBuildInputReference> { return Inputs; }
		auto MakeError(DerivedData::EBuildFailure) const -> FError
		{
			return {ETextureBuildFailure::InvalidInput, ETextureBuildStage::Normalize,
				"Texture build definition does not match its captured source or producer."};
		}
		auto IsCancelled(const FError&) const -> bool { return false; }
		auto ValidateBindings(const DerivedData::FBuildDefinition&) const -> std::expected<void, FError>
		{
			if (!Source.IsValid() || Source.GetOwner())
				return std::unexpected(MakeError(DerivedData::EBuildFailure::InputMismatch));
			for (const auto& Input : Inputs)
				if (Input.Identity != Source.GetIdentity())
					return std::unexpected(MakeError(DerivedData::EBuildFailure::InputMismatch));
			return {};
		}
		auto Resolve() { return ResolveSource(Source); }
		auto Build(auto& Prepared) -> std::expected<FProduct, FError> { return BuildProduct(Prepared); }
		auto Validate(const FProduct& Product) const -> std::expected<void, FError>
		{
			if (!Product || !Product->IsValid())
				return std::unexpected(FError{ETextureBuildFailure::InvalidBuilderOutput,
					ETextureBuildStage::Build, "Texture build returned invalid platform data."});
			return {};
		}
		auto Decode(const FSharedByteBuffer& Bytes) -> std::expected<FProduct, FError>
		{
			auto Product = std::make_unique<TPlatform>();
			if (auto Decoded = TexturePrivate::DecodePlatformData(Bytes.GetBytes(), TargetProfile, *Product); !Decoded)
			{
				return std::unexpected(FError{ETextureBuildFailure::InvalidBuilderOutput,
					ETextureBuildStage::Build, "Texture cached payload is invalid.", {}, std::move(Decoded.error())});
			}
			return Product;
		}
		auto Encode(FProduct& Product) -> std::expected<FByteBuffer, FError>
		{
			auto Encoded = TexturePrivate::EncodePlatformData(*Product, TargetProfile);
			if (!Encoded)
			{
				return std::unexpected(FError{ETextureBuildFailure::InvalidBuilderOutput,
					ETextureBuildStage::Build, "Texture payload could not be encoded.", {}, std::move(Encoded.error())});
			}
			return std::move(*Encoded);
		}
	};
}
#endif

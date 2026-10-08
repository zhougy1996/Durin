#pragma once

#include "CoreMinimal.h"

#include "RDG/RDGParameters.h"

namespace Durin
{
	struct FInstanceAccessParameters final
	{
		std::array<std::optional<FRDGTextureAccess>, 2> Textures;
		std::optional<FRDGBufferAccess> Buffer;

		static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
		{
			static const std::array Members{
				MakeRDGTextureAccessMetadata<FInstanceAccessParameters, decltype(Textures)>(
					"Textures", offsetof(FInstanceAccessParameters, Textures)),
				MakeRDGBufferAccessMetadata<FInstanceAccessParameters, decltype(Buffer)>(
					"Buffer", offsetof(FInstanceAccessParameters, Buffer))};
			static const auto Metadata = MakeInlineRDGParametersMetadata<FInstanceAccessParameters>(
				"FInstanceAccessParameters", Members);
			return &Metadata;
		}
	};

	struct FNestedInstanceAccessParameters final
	{
		std::array<FInstanceAccessParameters, 2> Inputs;

		static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
		{
			static const std::array Members{
				MakeRDGNestedParameterMemberMetadata<FNestedInstanceAccessParameters, decltype(Inputs)>(
					"Inputs", offsetof(FNestedInstanceAccessParameters, Inputs), FInstanceAccessParameters::GetRDGParametersMetadata())};
			static const auto Metadata = MakeInlineRDGParametersMetadata<FNestedInstanceAccessParameters>(
				"FNestedInstanceAccessParameters", Members);
			return &Metadata;
		}
	};

	struct FInstanceShaderAccessParameters final
	{
		FRDGTextureAccess Texture;
		std::optional<FRDGBufferAccess> Buffer;

		static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
		{
			static const std::array Members{
				WithRDGShaderBinding(MakeRDGTextureAccessMetadata<FInstanceShaderAccessParameters, decltype(Texture)>(
					"Texture", offsetof(FInstanceShaderAccessParameters, Texture)), ERHIBindingType::Texture, "Texture"),
				WithRDGShaderBinding(MakeRDGBufferAccessMetadata<FInstanceShaderAccessParameters, decltype(Buffer)>(
					"Buffer", offsetof(FInstanceShaderAccessParameters, Buffer)), ERHIBindingType::StorageBuffer, "Buffer")};
			static const auto Metadata = MakeInlineRDGParametersMetadata<FInstanceShaderAccessParameters>(
				"FInstanceShaderAccessParameters", Members);
			return &Metadata;
		}
	};

		struct FLargeTokenGraphParameters final
		{
			std::array<FRDGTokenParameter, 128> Tokens;

			static auto GetRDGParametersMetadata()
				-> const FRDGParametersMetadata*
			{
				static const std::array Members{
					MakeRDGResourceParameterMemberMetadata<
						FLargeTokenGraphParameters, decltype(Tokens),
						FRDGTokenParameter>("Tokens",
							offsetof(FLargeTokenGraphParameters, Tokens),
							ERDGParameterMemberKind::Token,
							ERDGResourceKind::Token,
							ERDGParameterRangeKind::None,
							ERDGUse::Write, ERHIAccess::None, true),
				};
				static const auto Metadata =
					MakeInlineRDGParametersMetadata<FLargeTokenGraphParameters>(
						"FLargeTokenGraphParameters", Members);
				return &Metadata;
			}
		};

}

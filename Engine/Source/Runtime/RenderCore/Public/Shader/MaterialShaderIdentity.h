#pragma once

#include "RenderCoreAPI.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"
#include "RHIResources.h"

#include <compare>
#include <string>
#include <type_traits>

namespace Durin
{
	struct FMaterialProgramIdentity
	{
		FXxHash128 Digest;

		auto IsValid() const -> bool { return !Digest.IsZero(); }
		auto ToString() const -> std::string { return Digest.ToString(); }
		auto operator==(const FMaterialProgramIdentity&) const -> bool = default;
		auto operator<(const FMaterialProgramIdentity& Other) const -> bool
		{
			return Digest < Other.Digest;
		}
	};

	using FMaterialRenderLayoutVersion = uint32;
	inline constexpr FMaterialRenderLayoutVersion CurrentMaterialRenderLayoutVersion = 3;
	inline constexpr FGuid MaterialRenderLayoutV3Id{
		0xd71bc1d4, 0xa5894f47, 0x9b5c08b5, 0xf42d75b2};

	struct FMaterialRenderLayoutIdentity
	{
		FMaterialRenderLayoutVersion Version = CurrentMaterialRenderLayoutVersion;
		FGuid Id = MaterialRenderLayoutV3Id;

		auto operator<=>(const FMaterialRenderLayoutIdentity&) const = default;
	};

	template<typename Tag>
	struct TMaterialShaderEnumKey
	{
		uint8 Value = 0;

		constexpr TMaterialShaderEnumKey() = default;
		constexpr explicit TMaterialShaderEnumKey(uint8 InValue) : Value(InValue) {}

		template<typename Enum>
			requires std::is_enum_v<Enum>
		constexpr TMaterialShaderEnumKey(Enum InValue)
			: Value(static_cast<uint8>(InValue))
		{
		}

		template<typename Enum>
			requires std::is_enum_v<Enum>
		constexpr auto operator=(Enum InValue) -> TMaterialShaderEnumKey&
		{
			Value = static_cast<uint8>(InValue);
			return *this;
		}

		constexpr operator uint8() const { return Value; }
		constexpr auto operator==(const TMaterialShaderEnumKey&) const
			-> bool = default;
		auto operator<=>(const TMaterialShaderEnumKey&) const = default;

		template<typename Enum>
			requires std::is_enum_v<Enum>
		friend constexpr auto operator==(
			TMaterialShaderEnumKey Left,
			Enum Right) -> bool
		{
			return Left.Value == static_cast<uint8>(Right);
		}

		template<typename Enum>
			requires std::is_enum_v<Enum>
		friend constexpr auto operator==(
			Enum Left,
			TMaterialShaderEnumKey Right) -> bool
		{
			return Right == Left;
		}
	};

	struct FMaterialShaderBlendModeTag;
	struct FMaterialShaderShadingModelTag;
	using FMaterialShaderBlendModeKey =
		TMaterialShaderEnumKey<FMaterialShaderBlendModeTag>;
	using FMaterialShaderShadingModelKey =
		TMaterialShaderEnumKey<FMaterialShaderShadingModelTag>;

	static_assert(sizeof(FMaterialShaderBlendModeKey) == sizeof(uint8));
	static_assert(sizeof(FMaterialShaderShadingModelKey) == sizeof(uint8));

	struct FMaterialShaderMapIdentity
	{
		FMaterialRenderLayoutIdentity RenderLayout;
		FMaterialProgramIdentity ProgramIdentity;
		FMaterialShaderBlendModeKey BlendMode;
		FMaterialShaderShadingModelKey ShadingModel;
		float OpacityMaskThreshold = 0.333f;

		auto operator==(const FMaterialShaderMapIdentity&) const -> bool = default;
	};

	struct FMaterialShaderPermutationIdentity
	{
		FMaterialShaderMapIdentity Material;
		std::string ShaderType;
		std::string EntryPoint;
		std::string Target;
		uint32 PermutationId = 0;
		EShaderFrequency Frequency = EShaderFrequency::Vertex;

		auto operator==(const FMaterialShaderPermutationIdentity&) const
			-> bool = default;
	};

	struct FMeshMaterialShaderPermutationIdentity
	{
		FMaterialShaderPermutationIdentity Material;
		std::string VertexFactoryType;
		uint32 MeshPassKey = 0;
		uint32 MeshPermutationId = 0;

		auto operator==(const FMeshMaterialShaderPermutationIdentity&) const
			-> bool = default;
	};

	RENDERCORE_API auto GetMaterialShaderIdentityHash(
		const FMaterialShaderPermutationIdentity& Identity) -> FXxHash128;
	RENDERCORE_API auto GetMaterialShaderIdentityHash(
		const FMeshMaterialShaderPermutationIdentity& Identity) -> FXxHash128;
	RENDERCORE_API auto GetMaterialShaderIdentityText(
		const FMaterialShaderPermutationIdentity& Identity) -> std::string;
	RENDERCORE_API auto GetMaterialShaderIdentityText(
		const FMeshMaterialShaderPermutationIdentity& Identity) -> std::string;
	RENDERCORE_API auto MaterialShaderIdentityLess(
		const FMaterialShaderPermutationIdentity& Left,
		const FMaterialShaderPermutationIdentity& Right) -> bool;
	RENDERCORE_API auto MaterialShaderIdentityLess(
		const FMeshMaterialShaderPermutationIdentity& Left,
		const FMeshMaterialShaderPermutationIdentity& Right) -> bool;
}

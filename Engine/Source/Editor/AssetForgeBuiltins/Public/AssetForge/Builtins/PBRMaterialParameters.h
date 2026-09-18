#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin::AssetForge::Builtins
{
	namespace MaterialParameters
	{
		// Identifies one fixed surface role in the canonical parameter schema.
		enum class EMaterialBuiltinParameterRole : uint8
		{
			BaseColor,
			Normal,
			Metallic,
			Roughness,
			AmbientOcclusion,
			Emissive,
			Opacity,
			OpacityMask,
			Count,
		};

		// Identifies one parameter owned by every canonical surface role.
		enum class EMaterialBuiltinParameterKind : uint8
		{
			Value,
			Texture,
			UVChannel,
			UVScale,
			UVOffset,
			UVRotation,
			Count,
		};

		// Groups every persistent identity owned by one built-in surface role.
		struct FMaterialBuiltinParameterIds
		{
			FGuid Value;
			FGuid Texture;
			FGuid UVChannel;
			FGuid UVScale;
			FGuid UVOffset;
			FGuid UVRotation;
		};

		// Maps one surface role to its complete persistent parameter identity group.
		struct FMaterialBuiltinParameterEntry
		{
			EMaterialBuiltinParameterRole Role;
			FMaterialBuiltinParameterIds Parameters;
		};

		inline constexpr size_t BuiltinParameterRoleCount =
			static_cast<size_t>(EMaterialBuiltinParameterRole::Count);
		inline constexpr size_t BuiltinParameterKindCount =
			static_cast<size_t>(EMaterialBuiltinParameterKind::Count);

		// The only authored copy of persistent built-in parameter identities.
		// Existing values must never be regenerated because instances serialize them.
		inline constexpr std::array<FMaterialBuiltinParameterEntry,
			BuiltinParameterRoleCount> BuiltinParameters{{
			{
				.Role = EMaterialBuiltinParameterRole::BaseColor,
				.Parameters = {
				.Value = {0x6c4d841a,0x88e14c35,0xa428910e,0xa8338339},
				.Texture = {0xe0588f9f,0x2cb64c17,0x9e4823fd,0xbc71e936},
				.UVChannel = {0x672ac603,0xe3b849d3,0xa0bd40a7,0x808f40d9},
				.UVScale = {0xefb7f324,0x0be949d7,0xb5e2aaf5,0xfb4e6805},
				.UVOffset = {0x7f06899b,0x33f5416d,0x9d07e4b0,0x86d9f512},
				.UVRotation = {0x35f1f695,0xc8bb4c59,0x89f55c74,0x8e297b22}},
			},
			{
				.Role = EMaterialBuiltinParameterRole::Normal,
				.Parameters = {
				.Value = {0xa21d2ef5,0x01bc40af,0x8912265f,0x401a1013},
				.Texture = {0x5555fb8e,0xc41041d9,0xb9090b80,0x60e5897d},
				.UVChannel = {0x5bd333c3,0x4f7b4794,0x8719fbbc,0xf6c55aa5},
				.UVScale = {0xa5a9c83b,0x4eb44263,0x83a69589,0xbc5c51fa},
				.UVOffset = {0xd8f1ff6d,0x0da845d3,0xb263bf33,0x6b268992},
				.UVRotation = {0xef2664e3,0xf45b4f20,0xaad6baa4,0x6486f63f}},
			},
			{
				.Role = EMaterialBuiltinParameterRole::Metallic,
				.Parameters = {
				.Value = {0x86355aae,0x5820462d,0xbc3690b8,0x402a06d4},
				.Texture = {0xef53c105,0x25e141e2,0x97cc521f,0xffaa7c62},
				.UVChannel = {0x4555094e,0x5e2146f8,0x8fa5461c,0x2855e779},
				.UVScale = {0xd24b6330,0xa6b94232,0xb929e02e,0xee5eb8cb},
				.UVOffset = {0x823917fc,0x577e4492,0xaee15bf5,0x1f7f99c9},
				.UVRotation = {0x3c598714,0x16174535,0x936eb4db,0xe5a210cd}},
			},
			{
				.Role = EMaterialBuiltinParameterRole::Roughness,
				.Parameters = {
				.Value = {0xec8d8285,0xab4549b3,0xaf1321be,0xcd490348},
				.Texture = {0xb2a36b19,0xefbd433d,0xa4ff5687,0x02ebc864},
				.UVChannel = {0x5f31c554,0x120d438c,0xac871567,0xea3dfb2c},
				.UVScale = {0x52b3dde0,0x3355417b,0xbf05eb11,0xa4d57d74},
				.UVOffset = {0xe8c9892e,0xfe2c471b,0xb76eeef3,0xd38a0eab},
				.UVRotation = {0x682ca789,0x18bd4ec4,0xa00f271a,0xd7527e59}},
			},
			{
				.Role = EMaterialBuiltinParameterRole::AmbientOcclusion,
				.Parameters = {
				.Value = {0x4ff53bc5,0x0c1c47a8,0x8453e4b2,0xa6be893c},
				.Texture = {0x88e38c97,0x44ac4b15,0xa203c18f,0x4e2b7d6e},
				.UVChannel = {0x22268e45,0x22ea4186,0x8c8032ae,0xbf3563a6},
				.UVScale = {0x8cd74420,0x60764ea4,0x88fb76ac,0xc08804d4},
				.UVOffset = {0xfcc40232,0xb6604de4,0x95123d02,0xe05dde5e},
				.UVRotation = {0x82b3fdc3,0x0f8840ad,0xbe12abdb,0xbc5732b1}},
			},
			{
				.Role = EMaterialBuiltinParameterRole::Emissive,
				.Parameters = {
				.Value = {0x0f059660,0xe75b4f74,0x934095ee,0x0dad4764},
				.Texture = {0xe544e53b,0x699b4f83,0x8f0b29f7,0x18aa9d02},
				.UVChannel = {0xe3da1eb1,0xb9374251,0xb671d414,0x3589b22a},
				.UVScale = {0xb9e82178,0x3fcd43e7,0x94aa7826,0x15b81866},
				.UVOffset = {0x165e8be8,0x46a44106,0xb22d3a0f,0x25bd23cb},
				.UVRotation = {0x76c5afe3,0xd08148cb,0x86c9125d,0x8accbce1}},
			},
			{
				.Role = EMaterialBuiltinParameterRole::Opacity,
				.Parameters = {
				.Value = {0x76c3ab5f,0x5de94104,0xaa6d0fb6,0xb44ab8a1},
				.Texture = {0xd6e3072e,0xc97146de,0xb699855e,0x6f96c767},
				.UVChannel = {0x9390003c,0x799e47e6,0x8aa7085f,0x02682928},
				.UVScale = {0x15e6d53d,0x890241ca,0x915eb4d1,0x32b0caa0},
				.UVOffset = {0xad888dbb,0x10934047,0x82901991,0x3f0ea763},
				.UVRotation = {0x5751ef57,0xf71d45e4,0x906ce613,0x9d22b2c4}},
			},
			{
				.Role = EMaterialBuiltinParameterRole::OpacityMask,
				.Parameters = {
				.Value = {0x6cd33852,0x373a4803,0xb2847fd3,0x319add4d},
				.Texture = {0xc4b39494,0xac194da8,0xb9b9beef,0x69c94797},
				.UVChannel = {0xfe9b13ed,0x48be4534,0xae1d9f02,0x4e56aed0},
				.UVScale = {0x89485eda,0xbf1d448a,0x8142d9b3,0xcc7705d1},
				.UVOffset = {0xefb2320e,0x8b514460,0xb3e92d3a,0x973d358a},
				.UVRotation = {0x4a40ca6b,0xa7fe48ae,0xb2af647a,0x7027f949}},
			},
		}};

		constexpr auto GetBuiltinParameterIds(EMaterialBuiltinParameterRole Role)
			-> FMaterialBuiltinParameterIds
		{
			for (const FMaterialBuiltinParameterEntry& Entry : BuiltinParameters)
				if (Entry.Role == Role) return Entry.Parameters;
			return {};
		}

		constexpr auto GetBuiltinParameterId(
			EMaterialBuiltinParameterRole Role,
			EMaterialBuiltinParameterKind Kind) -> FGuid
		{
			const FMaterialBuiltinParameterIds Ids = GetBuiltinParameterIds(Role);
			switch (Kind)
			{
			case EMaterialBuiltinParameterKind::Value: return Ids.Value;
			case EMaterialBuiltinParameterKind::Texture: return Ids.Texture;
			case EMaterialBuiltinParameterKind::UVChannel: return Ids.UVChannel;
			case EMaterialBuiltinParameterKind::UVScale: return Ids.UVScale;
			case EMaterialBuiltinParameterKind::UVOffset: return Ids.UVOffset;
			case EMaterialBuiltinParameterKind::UVRotation: return Ids.UVRotation;
			default: return {};
			}
		}

		constexpr auto FindBuiltinParameterRole(
			const FGuid& Id,
			EMaterialBuiltinParameterKind Kind) -> EMaterialBuiltinParameterRole
		{
			for (const FMaterialBuiltinParameterEntry& Entry : BuiltinParameters)
			{
				if (GetBuiltinParameterId(Entry.Role, Kind) == Id)
					return Entry.Role;
			}
			return EMaterialBuiltinParameterRole::Count;
		}

		constexpr auto IsBuiltinParameter(
			const FGuid& Id,
			EMaterialBuiltinParameterKind Kind) -> bool
		{
			return FindBuiltinParameterRole(Id, Kind)
				!= EMaterialBuiltinParameterRole::Count;
		}

		// FName cannot be safely initialized before the name pool, so canonical names
		// are exposed as function-local constants rather than namespace globals.
		ASSETFORGEBUILTINS_API auto BaseColorName() -> const FName&;
		ASSETFORGEBUILTINS_API auto BaseColorTextureName() -> const FName&;
		ASSETFORGEBUILTINS_API auto OpacityName() -> const FName&;
		ASSETFORGEBUILTINS_API auto NormalName() -> const FName&;
		ASSETFORGEBUILTINS_API auto NormalTextureName() -> const FName&;
		ASSETFORGEBUILTINS_API auto MetallicName() -> const FName&;
		ASSETFORGEBUILTINS_API auto MetallicTextureName() -> const FName&;
		ASSETFORGEBUILTINS_API auto RoughnessName() -> const FName&;
		ASSETFORGEBUILTINS_API auto RoughnessTextureName() -> const FName&;
		ASSETFORGEBUILTINS_API auto AmbientOcclusionName() -> const FName&;
		ASSETFORGEBUILTINS_API auto AmbientOcclusionTextureName() -> const FName&;
		ASSETFORGEBUILTINS_API auto EmissiveName() -> const FName&;
		ASSETFORGEBUILTINS_API auto EmissiveTextureName() -> const FName&;
		ASSETFORGEBUILTINS_API auto OpacityTextureName() -> const FName&;
		ASSETFORGEBUILTINS_API auto OpacityMaskName() -> const FName&;
		ASSETFORGEBUILTINS_API auto OpacityMaskTextureName() -> const FName&;
	}

	ASSETFORGEBUILTINS_API auto GetPBRMaterialParameterDefinitions() -> std::span<const FMaterialParameterDefinition>;
	// Creates the ordinary-graph PBR template declarations. Sampling policy belongs
	// to each Texture2D value.
	ASSETFORGEBUILTINS_API auto MakePBRMaterialParameterDefinitions() -> std::vector<FMaterialParameterDefinition>;

	// Resolves the persistent built-in parameter identity owned by one surface output.
	ASSETFORGEBUILTINS_API auto GetMaterialSurfaceParameterId(
		EMaterialSurfaceOutput Output,
		MaterialParameters::EMaterialBuiltinParameterKind Kind) -> FGuid;

}

#include "CookBuildProviders.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "Texture/ITextureCompressorModule.h"
#include "Texture/TextureDerivedDataKey.h"
#include "DerivedDataBuildDefinition.h"
#include "Physics/PhysicsCookDerivedDataKey.h"
#endif
#include "Collision/CollisionGeometry.h"
#include "Materials/MaterialCookedProgram.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::AssetPrivate
{
	auto GetCookBuildProviderInput(std::string_view Family, FByteBuffer& Out) -> bool
	{
		Out.clear();
		if (Family == "material")
		{
			FBinaryWriter Writer;
			Writer.WriteU64(GetMaterialBuildVersion());
			Writer.WriteU32(GetMaterialCompileAdapterVersion());
			Writer.WriteU32(MIR::CurrentVersion);
			Writer.WriteU32(MaterialCookedProgramPayloadSchemaVersion);
			Writer.WriteU32(CurrentMaterialPassContractVersion);
			Out = Writer.TakeBytes();
			return true;
		}
		if (Family == "texture2d" || Family == "texture-cube" || Family == "volume-texture")
		{
#if DURIN_WITH_EDITORONLY_DATA
			const auto Module = ITextureCompressorModule::Get();
			if (!Module) return false;
			const auto Kind = Family == "texture2d" ? ETextureBuildKind::Texture2D
				: Family == "texture-cube" ? ETextureBuildKind::TextureCube : ETextureBuildKind::VolumeTexture;
			const uint64 BuilderVersion = Module->GetBuildVersion(Kind);
			if (BuilderVersion == 0) return false;
			const auto Descriptor = GetTextureBuildDescriptor(Kind);
			FBinaryWriter Writer;
			Writer.WriteU64(BuilderVersion);
			Writer.WriteU32(Descriptor.Version);
			Writer.WriteU32(Descriptor.ConstantsSchema);
			Writer.WriteU32(Descriptor.OutputSchema);
			Writer.WriteU32(TexturePayloadSchemaVersion);
			Out = Writer.TakeBytes();
			return true;
#else
			return false;
#endif
		}
		if (Family == "static-mesh")
		{
#if DURIN_WITH_EDITORONLY_DATA
			const auto Module = IMeshBuilderModule::Get();
			if (!Module) return false;
			const uint64 BuilderVersion = Module->GetBuildVersion();
			if (BuilderVersion == 0) return false;
			FBinaryWriter Writer;
			Writer.WriteU64(BuilderVersion);
			Writer.WriteU32(StaticMeshRenderBuildFunctionVersion);
			Writer.WriteU32(StaticMeshRenderOutputSchemaVersion);
			Writer.WriteU32(StaticMeshPayloadSchemaVersion);
			const auto PhysicsDescriptor = GetPhysicsCookBuildDescriptor();
			Writer.WriteU64(FCollisionCookedData::GetBuildVersion());
			Writer.WriteU32(PhysicsDescriptor.Version);
			Writer.WriteU32(PhysicsDescriptor.ConstantsSchema);
			Writer.WriteU32(PhysicsDescriptor.OutputSchema);
			Writer.WriteU32(PhysicsCollisionPayloadSchemaVersion);
			Out = Writer.TakeBytes();
			return true;
#else
			return false;
#endif
		}
		return false;
	}
}

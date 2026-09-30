#include <gtest/gtest.h>
#include "NativeDObjectTestSupport.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MeshMaterialSlot.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace
{
	using namespace Durin;

	auto ExpectEditorFields(DStructBase* Type, std::initializer_list<const char*> Names) -> void
	{
		ASSERT_NE(Type, nullptr);
		for (const auto* Name : Names)
		{
			const auto* Property = Type->FindPropertyByName(Name);
#if DURIN_WITH_EDITORONLY_DATA
			ASSERT_NE(Property, nullptr) << Name;
			EXPECT_TRUE(Property->HasAnyPropertyFlags(EPropertyFlags::EditorOnly)) << Name;
#else
			EXPECT_EQ(Property, nullptr) << Name;
#endif
		}
	}
}

TEST(FAssetEditorDataTests, VariantReflectionMatchesNativeAssetLayout)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	ExpectEditorFields(DTexture::StaticClass(), {"Source", "AssetImportData"});
	ExpectEditorFields(DTexture2D::StaticClass(), {"MaxResolution", "CompressionQuality", "AlphaMipMode", "AlphaCoverageThreshold"});
	ExpectEditorFields(DTextureCube::StaticClass(), {"SourceLayout", "PanoramaFaceDimension", "PanoramaExposureEV",
		"Output", "OriginalSourceWidth", "OriginalSourceHeight"});
	ExpectEditorFields(DVolumeTexture::StaticClass(), {"BuildSettings"});
	ExpectEditorFields(DStaticMesh::StaticClass(), {"Source", "AssetImportData"});
	ExpectEditorFields(DMaterialInterface::StaticClass(), {"ImportProvenance"});
	ExpectEditorFields(DMaterial::StaticClass(), {"ExpressionCollection", "GraphPresentation"});
	ExpectEditorFields(DMaterialFunction::StaticClass(), {"ExpressionCollection", "Presentation"});
	ExpectEditorFields(DMaterialExpressionFunctionCall::StaticClass(), {"Function"});
	ExpectEditorFields(FMeshMaterialSlotDefinition::StaticStruct(), {"SourceName", "SourceMaterialIndex"});
	ExpectEditorFields(DBodySetup::StaticClass(), {"CollisionBuildRevision"});
	EXPECT_NE(DStaticMesh::StaticClass()->FindPropertyByName("MaterialSlots"), nullptr);
	EXPECT_NE(DTexture2D::StaticClass()->FindPropertyByName("Usage"), nullptr);
	EXPECT_NE(FMeshMaterialSlotDefinition::StaticStruct()->FindPropertyByName("Name"), nullptr);
}

#pragma once

#include "ExplicitMaterialProgramTestFixture.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Misc/MountPathTestSupport.h"
#include "MaterialTestSupport.h"
#include "Console/ConsoleCommand.h"
#include "DefaultTextures.h"
#include "DynamicRHI.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "PBRLighting.h"
#include "Preview/PreviewMeshResources.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/AssetThumbnailTestFixtures.h"
#include "Thumbnail/MaterialThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Texture/TextureCubeRenderResource.h"
#include "AssetForge/Builtins/Texture2DImport.h"

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <thread>

namespace
{
	class FScopedPreviewMeshCompiler
	{
	public:
		FScopedPreviewMeshCompiler()
		{
			InitializeDObjectSystem();
			if (Durin::GetStaticMeshCompilationManagerDiagnostics().bAcceptingRequests) return;
			auto& Aggregate = Durin::FAssetCompilingManager::Get();
			Aggregate.Start();
			Registration = Aggregate.RegisterCompiler({
				.Name = Durin::FName("Durin.StaticMesh"),
				.AssetClasses = {Durin::DStaticMesh::StaticClass()},
				.Manager = Durin::AssetPrivate::CreateStaticMeshCompilingManager()}).Handle;
		}
	private:
		Durin::FAssetCompilerRegistrationHandle Registration;
	};

	auto MakeExpandedMaterial(Durin::DObject* Outer, const char* Name)
		-> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(Outer, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)) return nullptr;
		if (!FinishMaterialCompileForTest(*Material)) return nullptr;
		return Material;
	}

}

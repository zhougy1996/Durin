#include "Interfaces/IProjectManager.h"
#include "Plugins/PluginResources.h"
#include "Project/Project.h"
#include "Modules/ModuleManager.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "DObject/Object.h"
#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "AssetRegistry/Scan.h"
#include "AssetRegistry/Catalog.h"
#include "Engine/ProjectGameSettings.h"
#include "Shader/IShaderBuildModule.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include <iostream>

int main(int Argc, char** Argv)
{
    using namespace Durin;
    if (Argc != 3) return 2;
    const std::string Mode = Argv[2];
    GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true; FNameInit();
    auto& Manager = IProjectManager::Get(); auto& Modules = FModuleManager::Get();
    if (!Manager.LoadProjectFile(Argv[1]) || !Manager.PreparePluginStartup())
    { std::cerr << Manager.GetLastError() << "\n"; return Mode == "reject" ? 0 : 3; }
    if (Mode == "reject") return 4;
    std::string Error;
    if (!FMountPaths::InitDefaultMountPoints(&Error)) { std::cerr << Error; return 5; }
    if (!InitializeAssetRuntime()) return 6;
    DObjectInit();
    if (!Manager.LoadModulesForProject() || !Manager.LoadModulesForProject(ELoadingPhase::EngineReady))
    { std::cerr << Manager.GetLastError(); return 7; }
    const auto Finish = [&](int Code) { ShutdownAssetRuntime(); Modules.ShutdownModulesAtExit(); return Code; };
    FYamlDocument Config;
    if (!LoadProjectConfig(std::filesystem::path(GetCurrentProject()->ProjectDir) / "Configs/Project.yaml", Config, Error)) return Finish(8);
    FPackagePath ProjectAsset;
    if (!FPackagePath::TryCreate("/Game/PluginTypedAsset", ProjectAsset)) return Finish(9);
    if (Mode == "disabled")
    {
        if (!GetActivePluginResources().ContentMounts.empty() || !GetActivePluginResources().ShaderRoots.empty()
            || Config.GetRootView().GetView("PluginFixture").GetView("BaseOnly").IsValid()) return Finish(10);
        if (FMountPaths::FindMountForVirtualPath("/Plugins/PluginFixtureRuntime/Asset")) return Finish(11);
        const auto Before = FFileHelper::LoadFileToArray(std::filesystem::path(GetCurrentProject()->ContentDir) / "PluginTypedAsset.dasset");
        if (!RefreshAssetRegistry()) return Finish(29);
        const auto Failed = LoadPackageDetailed(ProjectAsset);
        if (Failed) return Finish(12);
        std::cout << Failed.error().Message << "\n";
        if (Failed.error().Message.find("plugin") == std::string::npos) return Finish(13);
        const auto After = FFileHelper::LoadFileToArray(std::filesystem::path(GetCurrentProject()->ContentDir) / "PluginTypedAsset.dasset");
        if (!Before || !After || *Before != *After || FindResidentPackage(ProjectAsset)) return Finish(14);
        return Finish(0);
    }
    const auto Fixture = Config.GetRootView().GetView("PluginFixture");
    if (Fixture.GetView("Order").GetString() != "project" || Fixture.GetView("BaseOnly").GetString() != "present") return Finish(15);
    if (!FMountPaths::CheckMountDependency("/Game/Test", "/Plugins/PluginFixtureRuntime/Asset")
        || !FMountPaths::CheckMountDependency("/Plugins/PluginFixtureConsumer/Test", "/Plugins/PluginFixtureRuntime/Asset")
        || FMountPaths::CheckMountDependency("/Plugins/PluginFixtureRuntime/Test", "/Game/Test")
        || FMountPaths::CheckMountDependency("/Plugins/PluginFixtureRuntime/Test", "/Plugins/PluginFixtureConsumer/Test")) return Finish(16);
    FPackagePath Asset;
    if (!FPackagePath::TryCreate("/Plugins/PluginFixtureRuntime/Asset", Asset)) return Finish(17);
    if (Mode == "seed")
    {
        auto* Class = FindClassByQualifiedName("Durin::PluginFixture::DFixtureAsset");
        if (!Class) return Finish(18);
        for (const auto& Path : {Asset, ProjectAsset})
        {
            auto* Package = CreatePackage(Path);
            if (!Package || !NewObject(Class, Package, FName(Path.GetPackageName()), EObjectFlags::Public)) return Finish(19);
            FByteBuffer Bytes;
            const auto Encoded = SerializeAssetPackageBytes(Package, Bytes);
            if (!Encoded) { std::cerr << Encoded.Message; return Finish(20); }
            const auto Physical = FMountPaths::ResolveAssetPath(Path.ToString() + ".dasset");
            if (!Physical) return Finish(21);
            std::filesystem::create_directories(Physical->PhysicalPath.parent_path());
            // Explicit fixture creation only; ordinary saving must honor read-only mounts.
            if (!FFileHelper::SaveArrayToFile(Bytes, Physical->PhysicalPath)) return Finish(22);
            if (Path == Asset && SavePackage(Package).Error != EAssetWriteError::ReadOnlyMode) return Finish(23);
        }
    }
    else
    {
        if (Mode == "direct")
        { if (!AdmitAssetPackageToCatalog(Asset)) return Finish(30); }
        else if (!RefreshAssetRegistry()) return Finish(24);
        if (!FindPackageExact(Asset)) return Finish(24);
        const auto Loaded = LoadPackageDetailed(Asset);
        if (!Loaded) { std::cerr << Loaded.error().Message; return Finish(25); }
        if (SavePackage(*Loaded).Error != EAssetWriteError::ReadOnlyMode) return Finish(26);
    }
#if DURIN_WITH_EDITOR
    if (!Modules.LoadModule("ShaderBuild")) return Finish(27);
    auto* Shader = IShaderBuildModule::Get();
    FShaderCompileOptions Options; Options.Target = VulkanShaderTarget;
    Options.EntryPoints = {"VertexMain"}; Options.Frequencies = {EShaderFrequency::Vertex};
    FShaderSourceDependencyFingerprint Fingerprint;
    const auto Fingerprinted = Shader->BuildSourceTreeFingerprint("/Plugins/PluginFixtureRuntime/Fixture", Options, Fingerprint);
    if (!Fingerprinted) { std::cerr << std::string(Fingerprinted.error()); return Finish(28); }
    std::cout << "Shader " << Fingerprint.ContentHash.ToString() << "\n";
#endif
    return Finish(0);
}

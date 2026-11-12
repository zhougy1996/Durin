#include "Plugins/PluginResolution.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Project/Project.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	TEST(PluginResolutionTests, BuildAdmissionRejectsDriftAndPreservesOutput)
	{
		using namespace Durin;
		const auto Folder = Testing::CreateTestFixtureDirectory("PluginBuildAdmission");
		const auto Inventory = Folder / "PluginBuild.json";
		const auto Save = [&](const std::string& Text, const std::filesystem::path& Path) {
			return FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text.data(), Text.size())), Path);
		};
		ASSERT_TRUE(Save("fixture", Folder / "Fixture.dll"));
		FPluginResolution Selection;
		Selection.Modules.push_back({"Fixture", "Water", "Runtime", "Providers"});
		Selection.ManifestJson = R"({"Fingerprint":"0123456789abcdef","Platform":"Win64","RuntimeVariant":"DurinEditor"})";
		FPluginBuildIdentity Identity{"build", "Win64", "x64", "Debug", "DurinEditor", "abi"};
		const std::string Valid = R"({"SchemaVersion":1,"SelectionFingerprint":"0123456789abcdef","Identity":{"EngineBuild":"build","Platform":"Win64","Architecture":"x64","Configuration":"Debug","RuntimeVariant":"DurinEditor","ToolchainABI":"abi"},"Binaries":[{"Module":"Fixture","Owner":"Water","Path":"Fixture.dll","RuntimeDependencies":[]}]})";
		std::vector<FAdmittedPluginBinary> Output;
		std::string Error;
		ASSERT_TRUE(Save(Valid, Inventory));
		ASSERT_TRUE(AdmitPluginBuild(Selection, Identity, Inventory, Output, Error)) << Error;
		ASSERT_EQ(Output.size(), 1);
		EXPECT_EQ(Output.front().Path, std::filesystem::canonical(Folder / "Fixture.dll"));
		for (const auto& [Before, After] : {
			std::pair{"0123456789abcdef", "fedcba9876543210"}, {"build", "other"},
			{"Fixture.dll", "Missing.dll"}, {"Fixture.dll", "../Escape.dll"},
			{"Water", "Other"}, {"Fixture", "Disabled"}})
		{
			auto Bad = Valid;
			Bad.replace(Bad.find(Before), std::string(Before).size(), After);
			ASSERT_TRUE(Save(Bad, Inventory));
			EXPECT_FALSE(AdmitPluginBuild(Selection, Identity, Inventory, Output, Error));
			EXPECT_NE(Error.find("configure/build"), std::string::npos);
			ASSERT_EQ(Output.size(), 1);
			EXPECT_EQ(Output.front().Module.Name, "Fixture");
		}
	}

	TEST(PluginResolutionTests, SharedProjectDescriptorContract)
	{
        std::string Error;
		Durin::FJsonDocument Cases;
		ASSERT_TRUE(Cases.LoadFromFile(std::filesystem::path(DURIN_TEST_DATA_DIR)/"Projects.json"));
		const auto List=Cases.GetRootView().GetView("Cases");
		for (size_t I=0; I<List.Num(); ++I)
		{
			const auto Case=List.GetView(I); SCOPED_TRACE(Case.GetView("Name").GetString());
			const auto Folder=Durin::Testing::CreateTestFixtureDirectory("Descriptor-"+Case.GetView("Name").GetString());
			const auto File=Folder/"Fixture.dproject";
			const auto Text=Case.GetView("Source").GetString();
			ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text.data(),Text.size())),File));
			Durin::FProjectDescriptor Descriptor;
			const auto Loaded=Descriptor.Load(File.generic_string(), Error);
			ASSERT_EQ(Loaded,Case.GetView("Valid").GetBool()) << (Loaded ? "unexpected success" : Error);
			if (!Loaded) continue;
			for (const auto& [Variant,Expected] : {std::pair("DurinGame","Game"),std::pair("DurinEditor","Editor")})
			{
				const auto Roots=Descriptor.GetEnabledRootModules(Variant); ASSERT_TRUE(Roots);
				const auto Names=Case.GetView(Expected); ASSERT_EQ(Roots->size(),Names.Num());
				for (size_t Index=0; Index<Names.Num(); ++Index) EXPECT_EQ((*Roots)[Index],Names.GetView(Index).GetString());
			}
		}
	}

	TEST(PluginResolutionTests, FailedAdmissionAndQueriesPreserveActiveSnapshot)
	{
        std::string Error;
		auto& Manager=Durin::IProjectManager::Get(); Manager.ClearCurrentProject();
		const auto Folder=Durin::Testing::CreateTestFixtureDirectory("AdmissionSnapshot");
		const auto File=Folder/"Fixture.dproject"; const std::string Text=R"({"ProjectName":"Fixture","BaseModules":[],"Plugins":{"Water":true}})";
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text.data(),Text.size())),File));
		ASSERT_TRUE(Manager.LoadProjectFile(File.generic_string()));
		const auto* Active=Durin::GetCurrentProject(); const auto Path=Durin::FPaths::ProjectFile();
		const std::vector<Durin::FMountPoint> Probe{{.VirtualRoot="/Probe/",.Owner=Durin::EMountOwner::Test,.Root=Folder,.ContentPath="."}};
		Durin::Testing::FScopedMountRegistryFixture Registry(Probe); ASSERT_TRUE(Registry.IsValid());
		const auto* MountStorage=Durin::FMountPaths::GetRegisteredMountPoints().data();
		const auto Revision=Durin::FMountPaths::GetMountRegistryRevision();
		EXPECT_FALSE(Manager.LoadProjectFile((Folder/"Missing.dproject").generic_string()));
		EXPECT_EQ(Durin::GetCurrentProject(),Active); EXPECT_EQ(Durin::FPaths::ProjectFile(),Path);
		EXPECT_EQ(Durin::FMountPaths::GetMountRegistryRevision(),Revision);
		Durin::FProjectStatus ProjectStatus;
        ASSERT_TRUE(Manager.QueryStatusForProject(File.generic_string(), ProjectStatus));
		EXPECT_EQ(Durin::GetCurrentProject(),Active); EXPECT_EQ(Durin::FMountPaths::GetMountRegistryRevision(),Revision);
		EXPECT_EQ(Durin::FMountPaths::GetRegisteredMountPoints().data(),MountStorage);
		ASSERT_TRUE(Manager.SetPluginEnabled("water",false, Error));
		EXPECT_TRUE(Active->PluginOverrides.at("Water"));
		ASSERT_TRUE(Manager.SaveCurrentProjectToDisk(Error)); EXPECT_FALSE(Manager.IsCurrentProjectDirty());
		Durin::FProjectDescriptor Reload; ASSERT_TRUE(Reload.Load(File.generic_string(), Error)); EXPECT_FALSE(Reload.PluginOverrides.at("Water"));
		ASSERT_TRUE(Manager.RemovePluginReference("WATER", Error)); ASSERT_TRUE(Manager.SaveCurrentProjectToDisk(Error));
		ASSERT_TRUE(Reload.Load(File.generic_string(), Error)); EXPECT_TRUE(Reload.PluginOverrides.empty());
		ASSERT_TRUE(Manager.LoadModulesForProject());
		EXPECT_FALSE(Manager.LoadProjectFile(File.generic_string()));
		Manager.ClearCurrentProject(); EXPECT_FALSE(Durin::HasCurrentProject()); EXPECT_EQ(Manager.GetCurrentProject(),nullptr);
	}

	TEST(PluginResolutionTests, ProjectDescriptorQueriesAndAtomicSave)
	{
        std::string Error;
		const auto Folder = Durin::Testing::CreateTestFixtureDirectory("ProjectManager");
		const auto File = Folder / "Fixture.dproject";
		const std::string Source = R"({"ProjectName":"Fixture","BaseModules":[],"Plugins":{"Water":true},"Tests":{"Native":{"Root":"Tests"}}})";
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Source.data(), Source.size())), File));
		auto& Manager = Durin::IProjectManager::Get();
		const auto* Previous = Durin::GetCurrentProject();
		Durin::FProjectStatus ProjectStatus;
        const auto Status = Manager.QueryStatusForProject(File.generic_string(), ProjectStatus);
		ASSERT_TRUE(Status);
		EXPECT_EQ(ProjectStatus.Name, "Fixture");
		EXPECT_EQ(Durin::GetCurrentProject(), Previous);
		Durin::FProjectDescriptor Descriptor;
		ASSERT_TRUE(Descriptor.Load(File.generic_string(), Error));
		ASSERT_TRUE(Descriptor.GetEnabledRootModules("DurinGame"));
		EXPECT_TRUE(Descriptor.GetEnabledRootModules("DurinGame")->empty());
		Descriptor.PluginOverrides["Water"] = false;
		ASSERT_TRUE(Descriptor.Save(File.generic_string(), Error));
		Durin::FJsonDocument Saved;
		ASSERT_TRUE(Saved.LoadFromFile(File));
		EXPECT_EQ(Saved.GetRootView().GetView("Tests").GetView("Native").GetView("Root").GetString(), "Tests");
		EXPECT_FALSE(Saved.GetRootView().GetView("Plugins").GetView("Water").GetBool());
		ASSERT_TRUE(Manager.LoadProjectFile(File.generic_string()));
		ASSERT_TRUE(Manager.SetPluginEnabled("Water", false, Error));
		EXPECT_FALSE(Manager.IsCurrentProjectDirty());
		ASSERT_TRUE(Manager.SetPluginEnabled("Water", true, Error));
		EXPECT_TRUE(Manager.IsCurrentProjectDirty());
		const auto Backup=Folder / "Backup.dproject";
		std::filesystem::rename(File, Backup);
		std::filesystem::create_directory(File);
		EXPECT_FALSE(Manager.SaveCurrentProjectToDisk(Error));
		EXPECT_TRUE(Manager.IsCurrentProjectDirty());
		std::filesystem::remove(File);
		std::filesystem::rename(Backup, File);
		ASSERT_TRUE(Manager.RemovePluginReference("Water", Error));
		EXPECT_TRUE(Manager.GetCurrentProject()->PluginOverrides.empty());
		EXPECT_FALSE(Manager.SetPluginEnabled("../Invalid", true, Error));
		Manager.ClearCurrentProject();
	}

	TEST(PluginResolutionTests, TypedDescriptorEditsRoundTrip)
	{
		std::string Error;
		const auto Folder = Durin::Testing::CreateTestFixtureDirectory("TypedDescriptor");
		const auto File = Folder / "Fixture.dproject";
		Durin::FJsonDocument Source;
		ASSERT_TRUE(Source.Parse(R"({"ProjectName":"Fixture","ModuleDirs":{"Z":"Source/Z","A":"Source/A"}})"));
		Durin::FProjectDescriptor Descriptor;
		ASSERT_TRUE(Descriptor.Read(Source, File.generic_string(), Error));
		ASSERT_EQ(*Descriptor.GetEnabledRootModules("DurinGame"), (std::vector<std::string>{"Z", "A"}));
		Descriptor.ModuleDirs.erase("Z");
		Descriptor.ModuleDirs.emplace("B", "Source/B");
		ASSERT_EQ(*Descriptor.GetEnabledRootModules("DurinGame"), (std::vector<std::string>{"A", "B"}));
		Descriptor.Mounts.push_back({.VirtualRoot="/Addon/", .Owner=Durin::EMountOwner::Extension,
			.Root=Folder / "Addon", .ContentPath="Content", .bAutoScan=true,
			.bContentWritable=true, .Dependencies={"/Engine/"}});
		Descriptor.Schema = "durin-project.schema.json";
		Descriptor.NativeTestRoot = "Tests";
		ASSERT_TRUE(Descriptor.Save(File.generic_string(), Error)) << Error;
		Durin::FProjectDescriptor Reload;
		ASSERT_TRUE(Reload.Load(File.generic_string(), Error)) << Error;
		EXPECT_EQ(Reload.Schema, Descriptor.Schema);
		EXPECT_EQ(Reload.NativeTestRoot, Descriptor.NativeTestRoot);
		ASSERT_EQ(Reload.Mounts.size(), 1);
		EXPECT_EQ(Reload.Mounts.front().Root, Descriptor.Mounts.front().Root);
		EXPECT_TRUE(Reload.Mounts.front().bContentWritable);
		EXPECT_EQ(*Reload.GetEnabledRootModules("DurinGame"), (std::vector<std::string>{"A", "B"}));
		Descriptor.BaseModules = std::vector<std::string>{"B"};
		Descriptor.ExtraModules["DurinEditor"] = {"A"};
		ASSERT_TRUE(Descriptor.Save(File.generic_string(), Error)) << Error;
		ASSERT_TRUE(Reload.Load(File.generic_string(), Error));
		EXPECT_EQ(*Reload.GetEnabledRootModules("DurinGame"), (std::vector<std::string>{"B"}));
		EXPECT_EQ(*Reload.GetEnabledRootModules("DurinEditor"), (std::vector<std::string>{"B", "A"}));
		Descriptor.BaseModules = std::vector<std::string>{"Missing"};
		EXPECT_FALSE(Descriptor.Save(File.generic_string(), Error));
		ASSERT_TRUE(Reload.Load(File.generic_string(), Error));
		EXPECT_EQ(*Reload.GetEnabledRootModules("DurinGame"), (std::vector<std::string>{"B"}));
	}

	TEST(PluginResolutionTests, SharedPortableSelectionsAndFailures)
	{
        std::string Error;
		Durin::FJsonDocument Cases;
		ASSERT_TRUE(Cases.LoadFromFile(std::filesystem::path(DURIN_TEST_DATA_DIR) / "Cases.json"));
		const auto List = Cases.GetRootView().GetView("Cases");
		for (size_t I = 0; I < List.Num(); ++I)
		{
			const auto Case = List.GetView(I);
			const auto Name = Case.GetView("Name").GetString();
			SCOPED_TRACE(Name);
			const auto Root = Durin::Testing::CreateTestFixtureDirectory(Name);
			Case.GetView("Files").ForEachObjectMember([&](std::string_view Relative, Durin::FJsonNodeView Text) {
				const std::string Raw = Text.GetString();
				ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
					Durin::FByteView(reinterpret_cast<const std::byte*>(Raw.data()), Raw.size()),
					Root / std::string(Relative)));
			});
			Durin::FPluginResolveRequest Request;
			Request.EngineRoot = Root / "Engine";
			Request.ProjectFile = Root / "Game/Game.dproject";
			Request.RuntimeVariant = Case.GetView("Variant").GetString("DurinEditor");
			Request.Platform = Case.GetView("Platform").GetString("Win64");
			const auto Result = Durin::ResolvePlugins(Request);
			if (Case.Contains("Error"))
			{
				ASSERT_FALSE(Result);
				EXPECT_NE(Result.error().find(Case.GetView("Error").GetString()), std::string::npos) << Result.error();
				if (Case.Contains("Chain")) EXPECT_NE(Result.error().find(Case.GetView("Chain").GetString()), std::string::npos) << Result.error();
			}
			else
			{
				ASSERT_TRUE(Result) << Result.error();
				EXPECT_EQ(Result->ManifestJson, Case.GetView("Manifest").GetString());
				EXPECT_EQ(Result->ManifestJson.find(Root.generic_string()), std::string::npos);
				Durin::FJsonDocument Manifest;
				ASSERT_TRUE(Manifest.Parse(Result->ManifestJson));
				const auto Plugins = Manifest.GetRootView().GetView("Plugins");
				ASSERT_EQ(Result->Plugins.size(), Plugins.Num());
				for (size_t Index=0; Index<Plugins.Num(); ++Index)
					EXPECT_EQ(Result->Plugins[Index], Plugins.GetView(Index).GetView("Id").GetString());
				const auto Modules = Manifest.GetRootView().GetView("Modules");
				ASSERT_EQ(Result->Modules.size(), Modules.Num());
				for (size_t Index=0; Index<Modules.Num(); ++Index)
				{
					const auto Entry = Modules.GetView(Index);
					EXPECT_EQ(Result->Modules[Index].Name, Entry.GetView("Name").GetString());
					EXPECT_EQ(Result->Modules[Index].Owner, Entry.GetView("Owner").GetString());
					EXPECT_EQ(Result->Modules[Index].Host, Entry.GetView("Host").GetString());
					EXPECT_EQ(Result->Modules[Index].Phase, Entry.GetView("Phase").GetString());
				}
				for (const auto& [Logical, Path] : Result->Locations) EXPECT_TRUE(Path.is_absolute());
			}
		}
	}

	TEST(PluginResolutionTests, ForeignWorkspaceRootsDoNotActivatePlugins)
	{
		const auto Root = Durin::Testing::CreateTestFixtureDirectory("ForeignWorkspaceRoots");
		const auto Write = [&](std::string_view Path, std::string_view Text) {
			return Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text.data(), Text.size())), Root / Path);
		};
		ASSERT_TRUE(Write("Engine/Engine.dproject", R"({"ProjectName":"Engine"})"));
		ASSERT_TRUE(Write("Game/Game.dproject", R"({"ProjectName":"Game","ModuleDirs":{"Root":"Source/Root"},"Plugins":{"Water":true}})"));
		ASSERT_TRUE(Write("Game/Source/Root/Root.dmodule", R"({"ModuleName":"Root","PublicDependencies":["Water"]})"));
		ASSERT_TRUE(Write("Game/Plugins/Water/Water.dplugin", R"({"SchemaVersion":1,"Id":"Water","Modules":[{"Name":"Water","Path":"Source/Water"}]})"));
		ASSERT_TRUE(Write("Game/Plugins/Water/Source/Water/Water.dmodule", R"({"ModuleName":"Water"})"));
		Durin::FPluginResolveRequest Request;
		Request.EngineRoot = Root / "Engine";
		Request.ProjectFile = Root / "Engine/Engine.dproject";
		Request.ProjectFiles = {Root / "Game/Game.dproject"};
		const auto Engine = Durin::ResolvePlugins(Request);
		ASSERT_TRUE(Engine) << Engine.error();
		EXPECT_TRUE(Engine->Modules.empty());
		Request.ProjectFile = Root / "Game/Game.dproject";
		const auto Game = Durin::ResolvePlugins(Request);
		ASSERT_TRUE(Game) << Game.error();
		ASSERT_EQ(Game->Modules.size(), 1);
		EXPECT_EQ(Game->Modules.front().Name, "Water");
	}

	TEST(PluginResolutionTests, ProjectOverridesDoNotAlterModuleRoots)
	{
        std::string Error;
		Durin::FJsonDocument Document;
		ASSERT_TRUE(Document.Parse(R"({"Plugins":{"Water":true,"Tools":false}})"));
		auto Overrides = Durin::ReadProjectPluginOverrides(Document.GetRootView());
		ASSERT_TRUE(Overrides);
		EXPECT_EQ(Overrides->size(), 2);
		EXPECT_TRUE(Overrides->at("Water"));
		EXPECT_FALSE(Overrides->at("Tools"));
		ASSERT_TRUE(Document.Parse(R"({"Plugins":{"Water":true,"water":false}})"));
		EXPECT_FALSE(Durin::ReadProjectPluginOverrides(Document.GetRootView()));
		ASSERT_TRUE(Document.Parse(R"({"Plugins":[]})"));
		EXPECT_FALSE(Durin::ReadProjectPluginOverrides(Document.GetRootView()));
	}
}

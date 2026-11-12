#include "Plugins/PluginResolution.h"
#include "Plugins/PluginManagement.h"
#include "Plugins/PluginResources.h"
#include "Plugins/PluginCook.h"
#include "Misc/AppConfig.h"
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
    TEST(PluginResolutionTests, CookedInventoryBindsDetachedSelectionConfigurationAndMounts)
    {
        using namespace Durin;
        const auto Root=Testing::CreateTestFixtureDirectory("CookedInventory");
        const auto Save=[&](std::string_view Name,std::string_view Text) {
            const auto File=Root/Name; std::filesystem::create_directories(File.parent_path());
            if (!FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text.data(),Text.size())),File)) throw std::runtime_error("Cannot write cooked fixture");
        };
        const std::string Project=R"({"ProjectName":"Game","BaseModules":[],"Plugins":{"Water":true}})";
        Save("Project/Game.dproject",Project);
        Save("Project/Configs/Project.yaml","{}"); Save("Project/Configs/DurinGame.yaml","{}");
        Save("CookManifest.bin","manifest"); Save("Shaders/ShaderLibrary.dslb","library");
        FJsonDocument Snapshot;
        const auto ResetSnapshot=[&] {
            ASSERT_TRUE(Snapshot.Parse(R"({"SchemaVersion":1,"Platform":"Win64","Architecture":"x64","Configuration":"Debug","ToolchainABI":"abi","Project.yaml":"{}","DurinGame.yaml":"{}","Mounts":[{"VirtualRoot":"/Plugins/Water/","CanReference":["/Engine/"]}]})"));
            Snapshot.GetMutableRoot().SetChildValue("Selection",R"({"SchemaVersion":1,"Platform":"Win64","RuntimeVariant":"DurinGame","Plugins":[{"Id":"Water"}],"Modules":[]})");
            Snapshot.GetMutableRoot().SetChildValue("ProjectDescriptor",Project);
        };
        ResetSnapshot();
        const FPluginBuildIdentity Host{"host","Win64","x64","Debug","DurinGame","abi"};
        const auto Inventory=Root/"PluginRuntime.json";
        const auto Publish=[&](bool ChangedMount=false) {
            Save("PluginCook.json",Snapshot.ToString());
            FJsonDocument Document;
            ASSERT_TRUE(Document.Parse(R"({"SchemaVersion":1,"Identity":{"EngineBuild":"host","Platform":"Win64","Architecture":"x64","Configuration":"Debug","RuntimeVariant":"DurinGame","ToolchainABI":"abi"},"ProjectFile":"Project/Game.dproject","Plugins":["Water"],"Binaries":[],"Mounts":[{"VirtualRoot":"/Plugins/Water/","CanReference":["/Engine/"]}],"ConfigurationFiles":["Project/Configs/Project.yaml","Project/Configs/DurinGame.yaml"]})"));
            auto Output=Document.GetMutableRoot();
            if (ChangedMount) Output.GetRef("Mounts").GetRef(0).AddArray("CanReference").AppendValue("/Game/");
            auto Files=Output.AddArray("Files");
            for (const auto* Name:{"Project/Game.dproject","Project/Configs/Project.yaml","Project/Configs/DurinGame.yaml","CookManifest.bin","PluginCook.json","Shaders/ShaderLibrary.dslb"})
            {
                auto File=Files.AppendObject(); File.SetChildValue("Path",Name); File.SetChildValue("Sha256",PluginFileSha256(Root/Name));
            }
            Save("PluginRuntime.json",Document.ToString());
        };
        FPluginResolution Selection; std::vector<FAdmittedPluginBinary> Binaries; FPluginResources Resources; std::string Error;
        const auto Admit=[&] {return ReadPluginRuntimeInventory(Inventory,(Root/"Project/Game.dproject").generic_string(),Host,Selection,Binaries,Resources,Error);};
        Publish(); ASSERT_TRUE(Admit()) << Error; EXPECT_EQ(Selection.Plugins,(std::vector<std::string>{"Water"}));
        ASSERT_EQ(Resources.ContentMounts.size(),1); EXPECT_FALSE(Resources.ContentMounts[0].bContentWritable);
        Publish(true); EXPECT_FALSE(Admit()); EXPECT_NE(Error.find("references differ"),std::string::npos) << Error;
        // Failed admission preserves the previously usable outputs.
        EXPECT_EQ(Selection.Plugins,(std::vector<std::string>{"Water"}));
        Save("Project/Configs/DurinGame.yaml","modified"); Publish(); EXPECT_FALSE(Admit());
        EXPECT_NE(Error.find("configuration differs"),std::string::npos) << Error;
        Save("Project/Configs/DurinGame.yaml","{}");
        Snapshot.GetMutableRoot().SetChildValue("Configuration","Release"); Publish(); EXPECT_FALSE(Admit());
        EXPECT_NE(Error.find("target identity differs"),std::string::npos) << Error;
        ResetSnapshot(); Publish(); Save("Shaders/ShaderLibrary.dslb","corrupt"); EXPECT_FALSE(Admit());
        EXPECT_NE(Error.find("modified staged runtime file"),std::string::npos) << Error;
    }

    auto MakePluginManagementFixture(std::string_view Name) -> Durin::FPluginManagementContext
    {
        using namespace Durin;
        const auto Root = Testing::CreateTestFixtureDirectory(std::string(Name));
        const auto Save = [&](const std::filesystem::path& File, std::string_view Text) {
            std::filesystem::create_directories(File.parent_path());
            if (!FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text.data(), Text.size())), File)) throw std::runtime_error("Cannot prepare management fixture");
        };
        Save(Root / "Engine/Engine.dproject", R"({"ProjectName":"Engine","BaseModules":[]})");
        Save(Root / "Game/Game.dproject", R"({"ProjectName":"Game","BaseModules":[],"Plugins":{}})");
        Save(Root / "Game/Plugins/Base/Base.dplugin", R"({"SchemaVersion":1,"Id":"Base","DisplayName":"Base plugin","Modules":[{"Name":"BaseRuntime","Path":"Source"}]})");
        Save(Root / "Game/Plugins/Base/Source/BaseRuntime.dmodule", R"({"ModuleName":"BaseRuntime"})");
        Save(Root / "Game/Plugins/Water/Water.dplugin", R"({"SchemaVersion":1,"Id":"Water","Dependencies":["Base"]})");
        Save(Root / "Game/Plugins/Other/Other.dplugin", R"({"SchemaVersion":1,"Id":"Other","Platforms":["MacOS"]})");
        FPluginManagementContext Context;
        Context.Request.EngineRoot = Root / "Engine"; Context.Request.ProjectFile = Root / "Game/Game.dproject";
        Context.Identity = {"host", "Win64", "x64", "Debug", "DurinEditor", "abi"};
        Context.InventoryFile = Root / "Inventory/PluginBuild.json";
        auto Request = Context.Request; Request.PluginOverrides = std::map<std::string, bool>{{"Water",true}};
        const auto Selection = ResolvePlugins(Request);
        if (!Selection) throw std::runtime_error(Selection.error());
        FJsonDocument Selected; if (!Selected.Parse(Selection->ManifestJson)) throw std::runtime_error("Invalid fixture selection");
        FJsonDocument Inventory; auto Output = Inventory.GetMutableRoot(); Output.EnsureObject();
        Output.SetChildValue("SchemaVersion",2);
        auto Identity = Output.AddObject("Identity");
        for (const auto& [Key, Value] : {std::pair{"EngineBuild","host"}, {"Platform","Win64"}, {"Architecture","x64"}, {"Configuration","Debug"}, {"RuntimeVariant","DurinEditor"}, {"ToolchainABI","abi"}}) Identity.SetChildValue(Key,Value);
        auto Inputs = Output.AddArray("Inputs"); const auto Source = Selected.GetRootView().GetView("Inputs");
        for (size_t I=0; I<Source.Num(); ++I) { auto Item=Inputs.AppendObject(); Item.SetChildValue("Path",Source.GetView(I).GetView("Path").GetString()); Item.SetChildValue("Hash",Source.GetView(I).GetView("Hash").GetString()); }
        auto Binary = Output.AddArray("Binaries").AppendObject();
        Binary.SetChildValue("Module","BaseRuntime"); Binary.SetChildValue("Owner","Base"); Binary.SetChildValue("Path","Base.dll"); Binary.AddArray("RuntimeDependencies");
        Save(Context.InventoryFile, Inventory.ToString()); Save(Root / "Inventory/Base.dll","fixture");
        return Context;
    }
    TEST(PluginResolutionTests, PluginManagementKeepsUnsatisfiedRequestsCheckedWithoutChangingOtherChoices)
    {
        using namespace Durin;
        const auto Context = MakePluginManagementFixture("ManagementPreview");
        const auto Before = FFileHelper::LoadFileToArray(Context.Request.ProjectFile);
        const auto Revision = FMountPaths::GetMountRegistryRevision();
        FPluginEditSession Session; std::string Error;
        ASSERT_TRUE(Session.Open(Context,Error)) << Error;
        ASSERT_EQ(Session.GetRows().size(),3); EXPECT_FALSE(Session.IsDirty());
        ASSERT_TRUE(Session.SetEnabled("Water",true,Error));
        EXPECT_TRUE(Session.GetOverrides().at("Water")); EXPECT_FALSE(Session.GetOverrides().at("Base"));
        const auto Water = [&]() -> const FPluginManagementRow& { return *std::ranges::find(Session.GetRows(),"Water",[](const auto& Row){return Row.Info.Id;}); };
        EXPECT_TRUE(Water().bRequested); EXPECT_FALSE(Water().bNextStart);
        EXPECT_NE(Water().Reason.find("requires Base"),std::string::npos);
        EXPECT_TRUE(Session.IsSelectionValid()); EXPECT_TRUE(Session.IsDirty()); EXPECT_FALSE(Session.RequiresRestart());
        ASSERT_TRUE(Session.SetEnabled("Base",true,Error));
        EXPECT_TRUE(Water().bRequested); EXPECT_TRUE(Water().bNextStart); EXPECT_TRUE(Session.RequiresRestart());
        ASSERT_TRUE(Session.SetEnabled("Base",false,Error));
        EXPECT_TRUE(Water().bRequested); EXPECT_FALSE(Water().bNextStart);
        EXPECT_EQ(*FFileHelper::LoadFileToArray(Context.Request.ProjectFile),*Before); EXPECT_EQ(FMountPaths::GetMountRegistryRevision(),Revision);
        ASSERT_TRUE(Session.Revert(Error)); EXPECT_FALSE(Session.IsDirty());
    }
    TEST(PluginResolutionTests, PluginCheckboxEditsPreserveOtherCheckedDependencies)
    {
        using namespace Durin;
        const auto Context = MakePluginManagementFixture("ManagementIndependentChecks");
        const std::string Descriptor=R"({"ProjectName":"Game","BaseModules":[],"Plugins":{"Water":true}})";
        ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Descriptor.data(),Descriptor.size())),Context.Request.ProjectFile));
        FPluginEditSession Session; std::string Error;
        ASSERT_TRUE(Session.Open(Context,Error)); EXPECT_FALSE(Session.IsDirty());
        ASSERT_TRUE(Session.SetEnabled("Water",false,Error));
        EXPECT_TRUE(Session.GetOverrides().at("Base")); EXPECT_FALSE(Session.GetOverrides().at("Water"));
        const auto Base=std::ranges::find(Session.GetRows(),"Base",[](const auto& Row){return Row.Info.Id;});
        ASSERT_NE(Base,Session.GetRows().end()); EXPECT_TRUE(Base->bRequested); EXPECT_TRUE(Base->bNextStart);
    }
    TEST(PluginResolutionTests, PluginCheckboxEditsUseCachedMetadataWhenDescriptorsAndInventoryAreUnavailable)
    {
        using namespace Durin;
        const auto Context = MakePluginManagementFixture("ManagementCachedChecks");
        FPluginEditSession Session; std::string Error;
        ASSERT_TRUE(Session.Open(Context,Error));
        const auto Root=Context.Request.ProjectFile.parent_path().parent_path();
        std::filesystem::rename(Root/"Game",Root/"GameOffline");
        std::filesystem::rename(Root/"Inventory",Root/"InventoryOffline");
        const auto Begin=std::chrono::steady_clock::now();
        for (size_t I=0; I<1000; ++I)
        {
            ASSERT_TRUE(Session.SetEnabled("Water",true,Error));
            ASSERT_TRUE(Session.SetEnabled("Base",true,Error));
            ASSERT_TRUE(Session.SetEnabled("Base",false,Error));
            ASSERT_TRUE(Session.SetEnabled("Water",false,Error));
        }
        const auto Micros=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-Begin).count();
        RecordProperty("CachedCheckbox4000EditsMicroseconds",std::to_string(Micros));
        EXPECT_FALSE(Session.IsDirty()); EXPECT_TRUE(Session.IsSelectionValid());
        ASSERT_TRUE(Session.SetEnabled("Water",true,Error)); EXPECT_TRUE(Session.IsDirty());
        ASSERT_TRUE(Session.Revert(Error)) << Error;
        EXPECT_FALSE(Session.IsDirty()); EXPECT_FALSE(Session.GetOverrides().at("Water"));
        EXPECT_EQ(Session.GetRows().size(),3);
        EXPECT_FALSE(Session.CheckRestartReady(Error));
    }
    TEST(PluginResolutionTests, PluginManagementRefreshDiscoversChangesAndReloadsSavedStateAndBuildAvailability)
    {
        using namespace Durin;
        const auto Context=MakePluginManagementFixture("ManagementRefresh");
        FPluginEditSession Session; std::string Error;
        ASSERT_TRUE(Session.Open(Context,Error));
        const auto Base=[&]() -> const FPluginManagementRow& { return *std::ranges::find(Session.GetRows(),"Base",[](const auto& Row){return Row.Info.Id;}); };
        EXPECT_TRUE(Base().bBuildAvailable);
        ASSERT_TRUE(Session.SetEnabled("Water",true,Error));
        const auto Root=Context.Request.ProjectFile.parent_path().parent_path();
        const auto Plugin=Root/"Game/Plugins/Tools/Utility/Utility.dplugin";
        std::filesystem::create_directories(Plugin.parent_path());
        const std::string Metadata=R"({"SchemaVersion":1,"Id":"Utility","Version":"2"})";
        ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Metadata.data(),Metadata.size())),Plugin));
        std::filesystem::rename(Root/"Game/Plugins/Other",Root/"OtherOffline");
        const std::string Saved=R"({"ProjectName":"Game","BaseModules":[],"Plugins":{"Base":true}})";
        ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Saved.data(),Saved.size())),Context.Request.ProjectFile));
        const auto DiskBeforeRefresh=FFileHelper::LoadFileToArray(Context.Request.ProjectFile);
        ASSERT_TRUE(DiskBeforeRefresh);
        std::filesystem::rename(Context.InventoryFile,Root/"InventoryOffline.json");
        // Cancel restores the cached baseline without observing disk changes.
        ASSERT_TRUE(Session.Revert(Error)); EXPECT_FALSE(Base().bRequested); EXPECT_TRUE(Base().bBuildAvailable);
        EXPECT_EQ(Session.GetRows().size(),3);
        // Refresh uses the same open/rescan path as the window's Refresh button.
        ASSERT_TRUE(Session.Open(Context,Error)) << Error;
        EXPECT_FALSE(Session.IsDirty()); EXPECT_TRUE(Base().bRequested); EXPECT_FALSE(Base().bBuildAvailable);
        EXPECT_FALSE(Session.GetOverrides().at("Water")); EXPECT_FALSE(Session.GetBuildError().empty());
        EXPECT_EQ(Session.GetRows().size(),3);
        EXPECT_TRUE(std::ranges::none_of(Session.GetRows(),[](const auto& Row){return Row.Info.Id=="Other";}));
        const auto Utility=std::ranges::find(Session.GetRows(),"Utility",[](const auto& Row){return Row.Info.Id;});
        ASSERT_NE(Utility,Session.GetRows().end()); EXPECT_EQ(Utility->Info.Version,"2");
        const auto DiskAfterRefresh=FFileHelper::LoadFileToArray(Context.Request.ProjectFile);
        ASSERT_TRUE(DiskAfterRefresh); EXPECT_EQ(*DiskAfterRefresh,*DiskBeforeRefresh);
        std::filesystem::rename(Root/"InventoryOffline.json",Context.InventoryFile);
        ASSERT_TRUE(Session.Open(Context,Error)); EXPECT_TRUE(Base().bBuildAvailable); EXPECT_TRUE(Session.GetBuildError().empty());
    }
    TEST(PluginResolutionTests, PluginManagementKeepsDraftsWhenBuildsOrWritesAreUnavailable)
    {
        using namespace Durin;
        auto Context = MakePluginManagementFixture("ManagementMissingBuild");
        Context.InventoryFile = Context.InventoryFile.parent_path()/"Missing.json";
        FPluginEditSession Session; std::string Error; std::vector<std::string> Effects;
        ASSERT_TRUE(Session.Open(Context,Error));
        ASSERT_TRUE(Session.SetEnabled("Base",true,Error));
        ASSERT_TRUE(Session.SetEnabled("Water",true,Error));
        EXPECT_TRUE(Session.IsSelectionValid()); EXPECT_FALSE(Session.GetBuildError().empty());
        EXPECT_FALSE(Session.CheckRestartReady(Error)); EXPECT_TRUE(Session.IsDirty());
        const auto Before=FFileHelper::LoadFileToArray(Context.Request.ProjectFile);
        EXPECT_FALSE(Session.Save(Error)); EXPECT_TRUE(Session.IsDirty());
        EXPECT_EQ(*FFileHelper::LoadFileToArray(Context.Request.ProjectFile),*Before);
        ASSERT_TRUE(Session.Revert(Error)); ASSERT_TRUE(Session.CheckRestartReady(Error));
        const std::string Changed=" "; std::ofstream Stream(Context.Request.ProjectFile,std::ios::app); Stream<<Changed; Stream.close();
        EXPECT_FALSE(Session.CheckRestartReady(Error)); EXPECT_NE(Error.find("changed"),std::string::npos);
    }

    TEST(PluginResolutionTests, ResourceRootsAndLayeredDefaultsPreserveOwnership)
    {
        using namespace Durin;
        const auto Folder = Testing::CreateTestFixtureDirectory("PluginResources");
        const auto Save = [&](const std::filesystem::path& File, std::string_view Text) {
            std::filesystem::create_directories(File.parent_path());
            return FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text.data(), Text.size())), File);
        };
        FPluginResolution Selection;
        Selection.Plugins = {"Base", "Water"};
        for (const auto& Id : Selection.Plugins)
        {
            const auto Root = Folder / Id;
            for (const auto* Directory : {"Content", "Shaders", "Configs"}) std::filesystem::create_directories(Root / Directory);
            Selection.Locations["Plugin/" + Id] = Root / (Id + ".dplugin");
        }
        ASSERT_TRUE(Save(Selection.Locations["Plugin/Base"], R"({"Content":"Content","Shaders":"Shaders","Configs":"Configs"})"));
        ASSERT_TRUE(Save(Selection.Locations["Plugin/Water"], R"({"Content":"Content","Shaders":"Shaders","Configs":"Configs","Dependencies":["base"]})"));
        FPluginResources Resources;
        std::string Error;
        ASSERT_TRUE(ResolvePluginResources(Selection, Resources, Error)) << Error;
        ASSERT_EQ(Resources.ContentMounts.size(), 2);
        EXPECT_EQ(Resources.ContentMounts[1].VirtualRoot, "/Plugins/Water/");
        EXPECT_FALSE(Resources.ContentMounts[1].bContentWritable);
        EXPECT_EQ(Resources.ContentMounts[1].Dependencies, (std::vector<std::string>{"/Engine/", "/Plugins/Base/"}));
        EXPECT_EQ(Resources.ShaderRoots[1].SourceDirectory, std::filesystem::canonical(Folder / "Water/Shaders"));
        const auto OriginalRoot = Resources.ContentMounts[1].Root;
        ASSERT_TRUE(Save(Selection.Locations["Plugin/Water"], R"({"Content":"../Base/Content"})"));
        EXPECT_FALSE(ResolvePluginResources(Selection, Resources, Error));
        EXPECT_EQ(Resources.ContentMounts[1].Root, OriginalRoot);
        const std::array Layers{Folder / "Base/Configs/Project.yaml", Folder / "Water/Configs/Project.yaml", Folder / "Project.yaml"};
        ASSERT_TRUE(Save(Layers[0], "Game: {DefaultLevel: '/Plugins/Base/Start', NativeModule: Base}\nList: [a, b]\n"));
        ASSERT_TRUE(Save(Layers[1], "Game: {NativeModule: Water}\nList: [c]\n"));
        ASSERT_TRUE(Save(Layers[2], "Game: {DefaultLevel: ''}\n"));
        FYamlDocument Config;
        ASSERT_TRUE(LoadConfigLayers(Layers, Config, Error)) << Error;
        EXPECT_EQ(Config.GetRootView().GetView("Game").GetView("NativeModule").GetString(), "Water");
        EXPECT_EQ(Config.GetRootView().GetView("Game").GetView("DefaultLevel").GetString(), "");
        ASSERT_EQ(Config.GetRootView().GetView("List").Num(), 1);
        EXPECT_EQ(Config.GetRootView().GetView("List").GetView(0).GetString(), "c");
        const auto Before = Config.ToString();
        ASSERT_TRUE(Save(Layers[2], "[malformed"));
        EXPECT_FALSE(LoadConfigLayers(Layers, Config, Error));
        EXPECT_EQ(Config.ToString(), Before);
    }

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
		Selection.Plugins.push_back("Water");
		Selection.ManifestJson = R"({"Inputs":[{"Path":"Plugin/Water","Hash":"0123456789abcdef"}],"Platform":"Win64","RuntimeVariant":"DurinEditor"})";
		FPluginBuildIdentity Identity{"build", "Win64", "x64", "Debug", "DurinEditor", "abi"};
		const std::string Valid = R"({"SchemaVersion":2,"Inputs":[{"Path":"Plugin/Water","Hash":"0123456789abcdef"}],"Identity":{"EngineBuild":"build","Platform":"Win64","Architecture":"x64","Configuration":"Debug","RuntimeVariant":"DurinEditor","ToolchainABI":"abi"},"Binaries":[{"Module":"Fixture","Owner":"Water","Path":"Fixture.dll","RuntimeDependencies":[]}]})";
		std::vector<FAdmittedPluginBinary> Output;
		std::string Error;
		ASSERT_TRUE(Save(Valid, Inventory));
		ASSERT_TRUE(AdmitPluginBuild(Selection, Identity, Inventory, Output, Error)) << Error;
		ASSERT_EQ(Output.size(), 1);
		EXPECT_EQ(Output.front().Path, std::filesystem::canonical(Folder / "Fixture.dll"));
		// Extra inventory entries grant no permission and unused files may be absent.
		auto Extra = Valid;
		Extra.insert(Extra.rfind(']'), R"(,{"Module":"Disabled","Owner":"Other","Path":"Unused.dll","RuntimeDependencies":[]})");
		ASSERT_TRUE(Save(Extra, Inventory));
		ASSERT_TRUE(AdmitPluginBuild(Selection, Identity, Inventory, Output, Error)) << Error;
		ASSERT_EQ(Output.size(), 1);
		const auto Reject = [&](std::string Bad) {
			EXPECT_TRUE(Save(Bad, Inventory));
			EXPECT_FALSE(AdmitPluginBuild(Selection, Identity, Inventory, Output, Error)) << Bad;
			EXPECT_EQ(Output.size(), 1);
		};
		auto Indirect = Extra;
		Indirect.replace(Indirect.find("\"RuntimeDependencies\":[]"), std::string("\"RuntimeDependencies\":[]").size(),
			R"("RuntimeDependencies":["Unused.dll"])");
		ASSERT_TRUE(Save("unused", Folder / "Unused.dll"));
		Reject(Indirect);
		EXPECT_NE(Error.find("disabled plugin"), std::string::npos);
		auto Duplicate = Extra;
		Duplicate.replace(Duplicate.find("Disabled"), 8, "Fixture");
		Reject(Duplicate);
		auto Legacy = Valid;
		Legacy.replace(Legacy.find("\"SchemaVersion\":2"), 17, "\"SchemaVersion\":1");
		Reject(Legacy);

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

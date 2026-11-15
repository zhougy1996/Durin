#include "GameExport.h"
#include "Asset/Cook.h"
#include "Json/Json.h"
#include "Misc/RuntimePaths.h"
#include "Plugins/PluginResolution.h"
#include "Misc/FileHelper.h"
#include "Plugins/PluginCook.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
    TEST(FGameExportModuleLifecycleTests, LoadsRegisteredToolsModule)
    {
        EXPECT_NE(FModuleManager::Get().LoadModule("GameExport"),nullptr);
    }
    namespace
    {
        auto Write(const std::filesystem::path& File,std::string_view Text) -> void
        { std::filesystem::create_directories(File.parent_path()); std::ofstream(File,std::ios::binary).write(Text.data(),static_cast<std::streamsize>(Text.size())); }
        auto EmptyPE() -> std::string
        {
            std::string B(512,'\0');
            const auto Put=[&](size_t O,uint32 Value,size_t Size){std::memcpy(B.data()+O,&Value,Size);};
            Put(0,0x5a4d,2); Put(0x3c,64,4); Put(64,0x4550,4); Put(68,0x8664,2); Put(64+20,240,2); Put(88,0x20b,2);
            return B;
        }
        auto ImportPE(bool Delay) -> std::string
        {
            std::string B(1024,'\0');
            const auto Put=[&](size_t O,uint32 V,size_t N){std::memcpy(B.data()+O,&V,N);};
            Put(0,0x5a4d,2); Put(0x3c,128,4); Put(128,0x4550,4); Put(132,0x8664,2); Put(134,1,2); Put(148,240,2); Put(152,0x20b,2);
            Put(152+240+8,512,4); Put(152+240+12,0x1000,4); Put(152+240+16,512,4); Put(152+240+20,512,4);
            Put(152+112+(Delay?13:1)*8,0x1000,4); Put(152+112+(Delay?13:1)*8+4,64,4);
            Put(512+(Delay?4:12),0x1080,4); if (Delay) Put(512,1,4);
            std::memcpy(B.data()+640,"leaf.dll",9); return B;
        }
        class FGameExportTests : public testing::Test
        {
        protected:
            std::filesystem::path Root,Engine,Cook,Output,Runtime,Project;
            FJsonDocument Request,Receipt;
            std::vector<std::string> Events;
            void SetUp() override
            {
                Root=Testing::CreateTestFixtureDirectory("GameExport"); Engine=Root/"source/Engine";
                Project=Root/"source/Example/Example.dproject"; Cook=Root/"cook"; Output=Root/"output";
                Runtime=Engine/"Binaries/Win64/Debug/Runtime/DurinGame";
                Write(Engine/"Engine.dproject",R"({"ProjectName":"Engine","BaseModules":[]})");
                Write(Project,R"({"ProjectName":"Example","BaseModules":[]})");
                Write(Root/"source/Durin.dworkspace",R"({"Projects":["Engine/Engine.dproject","Example/Example.dproject"]})");
                Write(Runtime/"DurinGame.exe",EmptyPE()); Write(Runtime/"DurinGame-Core.dll",EmptyPE());
                const auto Policy=Engine/"Source/Developer/GameExport/Contracts/win64-system-v1.json";
                std::filesystem::create_directories(Policy.parent_path()); std::filesystem::copy_file(DURIN_EXPORT_SYSTEM_POLICY,Policy);
                FPluginResolveRequest Resolve; Resolve.EngineRoot=Engine; Resolve.ProjectFile=Project;
                Resolve.ProjectFiles={Engine/"Engine.dproject",Project}; Resolve.RuntimeVariant="DurinGame";
                const auto Selection=ResolvePlugins(Resolve); ASSERT_TRUE(Selection) << Selection.error();
                FJsonDocument Snapshot; auto S=Snapshot.GetMutableRoot(); S.EnsureObject(); S.SetChildValue("SchemaVersion",1);
                S.SetChildValue("EngineVersion","fixture"); S.SetChildValue("Platform","Win64"); S.SetChildValue("Architecture","x64"); S.SetChildValue("Configuration","Debug"); S.SetChildValue("ToolchainABI","fixture-abi");
                S.SetChildValue("Selection",Selection->ManifestJson); S.SetChildValue("ProjectDescriptor",R"({"ProjectName":"Example","BaseModules":[]})");
                S.SetChildValue("PluginStartupConfig","{}");
                S.SetChildValue("EngineGameDefaults","{}\n"); S.SetChildValue("Project.yaml","{}\n"); S.SetChildValue("DurinGame.yaml","{}\n"); S.AddArray("Mounts");
                Write(Cook/"ExportEvidence.json",Snapshot.ToString()); Write(Cook/"Shaders/ShaderLibrary.dslb","fixture shader");
                FJsonDocument Backend; auto B=Backend.GetMutableRoot(); B.EnsureObject(); B.SetChildValue("SchemaVersion",1); B.SetChildValue("Kind","source-stage"); B.SetChildValue("TargetReceipt",(Root/"build/TargetReceipt.json").generic_string()); B.SetChildValue("ArtifactsRoot",(Root/"build").generic_string()); B.SetChildValue("DependenciesRoot",Engine.generic_string());
                Backend.SaveToFile((Root/"backend.json").generic_string());
                ASSERT_TRUE(Receipt.Parse(R"({"SchemaVersion":1,"Status":"succeeded","Identity":{"EngineBuild":"fixture","Project":"Example","Platform":"Win64","Architecture":"x64","Configuration":"Debug","RuntimeVariant":"DurinGame","ToolchainABI":"fixture-abi","Target":"DurinLauncher"},"Products":[],"Modules":[],"Inputs":[],"SelectionFingerprint":"fixture","BuildInputs":[],"CookCompatibility":"fixture-compat","Prerequisites":{"SystemPolicy":"win64-system-v1","CRT":"developer-debug-crt","MinimumVersion":"14.44.0.0","Provenance":"fixture-abi","License":"fixture"}})"));
                AddNativeModule("Core","Engine"); AddNativeModule("DurinLauncher","Engine").GetRef("Linked").AppendValue("Core");
                AddNativeProduct("Core",Runtime/"DurinGame-Core.dll","Binaries/Win64/DurinGame-Core.dll","NativeLibrary");
                AddNativeProduct("DurinLauncher",Runtime/"DurinGame.exe","Binaries/Win64/Example.exe","Executable"); CommitReceipt();
                auto R=Request.GetMutableRoot(); R.EnsureObject(); R.SetChildValue("ProtocolVersion",1); R.SetChildValue("Kind","request"); R.SetChildValue("RequestId","Test1"); R.SetChildValue("Operation","stage");
                R.SetChildValue("ProjectFile",Project.generic_string()); R.SetChildValue("EngineRoot",Engine.generic_string()); R.SetChildValue("Platform","Win64"); R.SetChildValue("Configuration","Debug"); R.SetChildValue("Output",Output.generic_string()); R.SetChildValue("Backend","source"); R.SetChildValue("BackendMetadata",(Root/"backend.json").generic_string()); R.SetChildValue("Build","reuse"); R.SetChildValue("Cook","reuse"); R.SetChildValue("Verify",false); R.SetChildValue("Archive",false); R.SetChildValue("CookDirectory",Cook.generic_string());
            }
            auto AddNativeModule(std::string_view Name,std::string_view Owner) -> FJsonNodeRef
            {
                auto M=Receipt.GetMutableRoot().GetRef("Modules").AppendObject(); M.SetChildValue("Name",Name); M.SetChildValue("Owner",Owner); M.SetChildValue("Host","Runtime");
                M.AddArray("Linked"); M.AddArray("Dynamic"); M.AddArray("RuntimeFiles"); return M;
            }
            auto NativeModule(std::string_view Name) -> FJsonNodeRef
            {
                auto Modules=Receipt.GetMutableRoot().GetRef("Modules");
                for(size_t I=0;I<Modules.Num();++I) if(Modules.GetView(I).GetView("Name").GetString()==Name) return Modules.GetRef(I);
                throw std::runtime_error("Missing fixture module");
            }
            void AddNativeProduct(std::string_view Module,const std::filesystem::path& File,std::string_view Destination,std::string_view Kind)
            {
                auto P=Receipt.GetMutableRoot().GetRef("Products").AppendObject(); P.SetChildValue("Module",Module); P.SetChildValue("Owner",NativeModule(Module).GetView("Owner").GetString()); P.SetChildValue("Host","Runtime");
                auto Source=P.AddObject("Source"); const bool IsEngine=File.generic_string().starts_with(Engine.generic_string()+"/");
                Source.SetChildValue("Root",IsEngine?"Engine":"Artifacts"); Source.SetChildValue("Path",File.lexically_relative(IsEngine?Engine:Root/"build").generic_string());
                P.SetChildValue("Destination",Destination); P.SetChildValue("Kind",Kind); P.SetChildValue("Sha256",FileSha256(File));
                NativeModule(Module).GetRef("RuntimeFiles").AppendValue(Destination);
            }
            void CommitReceipt()
            {
                auto R=Receipt.GetMutableRoot(); auto Products=R.GetRef("Products");
                for(size_t I=0;I<Products.Num();++I) {auto P=Products.GetRef(I); const auto Ref=P.GetView("Source"); P.SetChildValue("Sha256",FileSha256((Ref.GetView("Root").GetString()=="Engine"?Engine:Root/"build")/Ref.GetView("Path").GetString()));}
                R.GetRef("Identity").SetChildValue("EngineBuild","fixture:"+FileSha256(Runtime/"DurinGame-Core.dll"));
                auto Inputs=R.AddArray("Inputs"); std::string Fingerprint;
                for(const auto& [Kind,File]:{std::pair{"Engine",Engine/"Engine.dproject"},std::pair{"Project",Project}})
                {auto I=Inputs.AppendObject(); I.SetChildValue("Root",Kind); I.SetChildValue("Path",File.filename().generic_string()); const auto Hash=FileSha256(File); I.SetChildValue("Sha256",Hash); Fingerprint+=std::string(Kind)+"/"+File.filename().generic_string()+":"+Hash+"\n";}
                R.SetChildValue("InputFingerprint",BytesSha256(std::as_bytes(std::span(Fingerprint.data(),Fingerprint.size()))));
                Write(Root/"build/TargetReceipt.json",Receipt.ToString());
                CommitCook();
            }
            void CommitCook()
            {
                std::ifstream File(Cook/"ExportEvidence.json",std::ios::binary); const std::string Evidence((std::istreambuf_iterator<char>(File)),{});
                FJsonDocument TargetEvidence; ASSERT_TRUE(TargetEvidence.Parse(Evidence));
                TargetEvidence.GetMutableRoot().SetChildValue("GameBuild","fixture:"+FileSha256(Runtime/"DurinGame-Core.dll"));
                TargetEvidence.GetMutableRoot().SetChildValue("TargetReceiptSha256",FileSha256(Root/"build/TargetReceipt.json"));
                TargetEvidence.GetMutableRoot().SetChildValue("CookCompatibility","fixture-compat");
                Write(Cook/"ExportEvidence.json",TargetEvidence.ToString());
                std::string RuntimeJson,Reason; ASSERT_TRUE(MakeRuntimeCookSnapshot(TargetEvidence.ToString(),RuntimeJson,Reason))<<Reason;
                Write(Cook/"RuntimeCook.json",RuntimeJson);
                FCookManifest CM; CM.TargetPlatform=ECookTargetPlatform::Win64; CM.TargetProfile=ECookTargetProfile::Game;
                for (const auto& [Path,Kind]:{std::pair{"ExportEvidence.json",ECookManifestEntryKind::PluginSnapshot},std::pair{"RuntimeCook.json",ECookManifestEntryKind::PluginSnapshot},std::pair{"Shaders/ShaderLibrary.dslb",ECookManifestEntryKind::ShaderLibrary}})
                {
                    const auto Data=FFileHelper::LoadFileToArray(Cook/Path); ASSERT_TRUE(Data);
                    const auto Hash=FXxHash128::HashBuffer(*Data); CM.Entries.push_back({Kind,CookManifestEntryPresent,Path,Data->size(),Hash.HashLow,Hash.HashHigh});
                }
                FByteBuffer Encoded; ASSERT_TRUE(EncodeCookManifest(CM,Encoded)); ASSERT_TRUE(FFileHelper::SaveArrayToFile(Encoded,Cook/"CookManifest.bin"));
            }
            void TearDown() override { Testing::RemoveTestWorkDirectory(Root); }
            int Attempt=0;
            auto ExecuteFresh() -> int
            {
                Request.GetMutableRoot().SetChildValue("Output",(Root/("attempt-"+std::to_string(++Attempt))).generic_string());
                return Execute();
            }
            auto Execute(const std::function<bool()>& Cancel={},const std::function<void(std::string_view)>& Observe={}) -> int
            { Events.clear(); return ExecuteGameExportRequest(Request.ToString(),Cancel,[&](std::string_view E){Events.emplace_back(E); if (Observe) Observe(E);}); }
        };
    }
    TEST_F(FGameExportTests, GeneratedTargetSelectorDisambiguatesPlatformAndPresetForStage)
    {
        FJsonDocument B; ASSERT_TRUE(B.LoadFromFile(Root/"backend.json")); auto Games=B.GetMutableRoot().AddArray("Games");
        for(const auto* Name:{"Default","Alternate","Foreign"}) {
            auto G=Games.AppendObject(); G.SetChildValue("Name",Name); G.SetChildValue("Project","Example");
            G.SetChildValue("Platform",std::string_view(Name)=="Foreign"?"MacOS":"Win64"); G.SetChildValue("Configuration","Debug");
            G.SetChildValue("TargetReceipt",(Root/"build/TargetReceipt.json").generic_string());
            G.SetChildValue("ArtifactsRoot",(Root/"build").generic_string()); G.SetChildValue("DependenciesRoot",Engine.generic_string());
        }
        Write(Root/"backend.json",B.ToString());
        EXPECT_EQ(Execute(),3); EXPECT_FALSE(std::filesystem::exists(Output));
        Request.GetMutableRoot().SetChildValue("Preset","Alternate"); ASSERT_EQ(Execute(),0) << Events.back();
        Request.GetMutableRoot().SetChildValue("Preset","Foreign"); EXPECT_EQ(ExecuteFresh(),3);
    }
    TEST_F(FGameExportTests, PreviewChecksConfigurationWithoutAdmittingCookProducts)
    {
        Write(Cook/"Shaders/ShaderLibrary.dslb","corrupt");
        for(const auto* Operation:{"preview","stage","export"}) {
            Request.GetMutableRoot().SetChildValue("Operation",Operation);
            Request.GetMutableRoot().SetChildValue("Preview",true);
            ASSERT_EQ(Execute(),0) << Events.back();
            EXPECT_FALSE(std::filesystem::exists(Output));
            FJsonDocument Result; ASSERT_TRUE(Result.Parse(Events.back()));
            EXPECT_TRUE(Result.GetRootView().GetView("PlanFingerprint").IsNull());
        }
        Request.GetMutableRoot().SetChildValue("Preview",false);
        EXPECT_NE(Execute(),0); EXPECT_FALSE(std::filesystem::exists(Output/"ExportComplete.json"));
    }
    TEST_F(FGameExportTests, PrebuiltRequiresComponentEvenForConfigurationPreview)
    {
        FJsonDocument B; ASSERT_TRUE(B.LoadFromFile(Root/"backend.json"));
        B.GetMutableRoot().SetChildValue("Kind","prebuilt-stage"); Write(Root/"backend.json",B.ToString());
        Request.GetMutableRoot().SetChildValue("Backend","prebuilt");
        Request.GetMutableRoot().SetChildValue("Preview",true);
        EXPECT_EQ(Execute(),3); EXPECT_FALSE(std::filesystem::exists(Output));
    }
    TEST_F(FGameExportTests, ComponentIsValidatedOncePerProcessAndFailuresAreNotCached)
    {
        const auto Component=Root/"ExportComponent.json", Worker=Root/"worker.exe";
        Write(Worker,"worker");
        FJsonDocument B; ASSERT_TRUE(B.LoadFromFile(Root/"backend.json"));
        B.GetMutableRoot().SetChildValue("Component",Component.generic_string()); Write(Root/"backend.json",B.ToString());
        FJsonDocument C; auto V=C.GetMutableRoot(); V.EnsureObject();
        V.SetChildValue("SchemaVersion",1); V.SetChildValue("ProtocolVersion",1); V.SetChildValue("Name","DurinExportTool");
        V.SetChildValue("Worker","worker.exe"); V.SetChildValue("BackendMetadata","backend.json");
        auto Files=V.AddArray("Files");
        for(const auto* Name:{"worker.exe","backend.json"}) {auto F=Files.AppendObject(); F.SetChildValue("Path",Name); F.SetChildValue("Sha256",FileSha256(Root/Name));}
        Write(Component,C.ToString()); Write(Worker,"corrupt");
        Request.GetMutableRoot().SetChildValue("Preview",true);
        EXPECT_EQ(Execute(),3);
        Write(Worker,"worker"); ASSERT_EQ(Execute(),0) << Events.back();
        // Simulate inventory access becoming unavailable after admission: cached immutable metadata is reused.
        std::filesystem::remove(Worker); std::filesystem::remove(Component);
        EXPECT_EQ(Execute(),0);
    }
    TEST_F(FGameExportTests, ExportUsesFreshDirectoryAndBindsCompletionMarker)
    {
        Request.GetMutableRoot().SetChildValue("Operation","export");
        Request.GetMutableRoot().SetChildValue("Preview",true);
        ASSERT_EQ(Execute(),0) << Events.back(); EXPECT_FALSE(std::filesystem::exists(Output));
        Request.GetMutableRoot().SetChildValue("Preview",false);
        ASSERT_EQ(Execute(),0) << Events.back();
        FJsonDocument Marker; ASSERT_TRUE(Marker.LoadFromFile(Output/"ExportComplete.json"));
        EXPECT_EQ(Marker.GetRootView().GetView("RuntimeManifestSha256").GetString(),FileSha256(Output/"Delivery/Runtime.json"));
        const auto Previous=FileSha256(Output/"ExportComplete.json");
        EXPECT_EQ(Execute(),2); EXPECT_EQ(FileSha256(Output/"ExportComplete.json"),Previous);
    }
    TEST_F(FGameExportTests, FailedExportRemainsUnmarkedAndNeverReplacesAnotherInvocation)
    {
        Request.GetMutableRoot().SetChildValue("Operation","export");
        Write(Cook/"Shaders/ShaderLibrary.dslb","corrupt");
        EXPECT_NE(Execute(),0); EXPECT_FALSE(std::filesystem::exists(Output/"ExportComplete.json"));
        EXPECT_TRUE(std::filesystem::is_directory(Output));
        EXPECT_EQ(Execute(),2);
    }
    TEST_F(FGameExportTests, FailedVerificationAndMarkerPublicationLeaveNoCompletedExport)
    {
        Request.GetMutableRoot().SetChildValue("Operation","export"); Request.GetMutableRoot().SetChildValue("Verify",true);
        EXPECT_EQ(Execute(),9); EXPECT_FALSE(std::filesystem::exists(Output/"ExportComplete.json"));
        Testing::RemoveTestWorkDirectory(Output);
        Request.GetMutableRoot().SetChildValue("Verify",false);
        EXPECT_EQ(Execute({},[&](std::string_view Event){
            FJsonDocument E; if(E.Parse(Event) && E.GetRootView().GetView("Message").GetString()=="Writing completion marker")
                std::filesystem::create_directory(Output/"ExportComplete.json");
        }),11);
        EXPECT_FALSE(std::filesystem::is_regular_file(Output/"ExportComplete.json"));
    }
    TEST_F(FGameExportTests, BuildCookAndArchiveChildrenFailWithoutCompletionOrRollback)
    {
        FJsonDocument B; ASSERT_TRUE(B.LoadFromFile(Root/"backend.json")); auto V=B.GetMutableRoot();
        V.SetChildValue("CMake",DURIN_EXPORT_FAILURE_FIXTURE); V.SetChildValue("AssetTool",DURIN_EXPORT_FAILURE_FIXTURE);
        V.SetChildValue("WorkingDirectory",Root.generic_string());
        auto Game=V.AddArray("Games").AppendObject(); Game.SetChildValue("Name","Fixture"); Game.SetChildValue("Project","Example"); Game.SetChildValue("Configuration","Debug");
        Game.SetChildValue("TargetReceipt",(Root/"build/TargetReceipt.json").generic_string()); Game.SetChildValue("ArtifactsRoot",(Root/"build").generic_string()); Game.SetChildValue("DependenciesRoot",Engine.generic_string());
        V.AddObject("Environment").SetChildValue("DURIN_PROCESS_PROBE_EXIT","17");
        ASSERT_TRUE(B.SaveToFile((Root/"backend.json").generic_string())); Request.GetMutableRoot().SetChildValue("Operation","export");
        Request.GetMutableRoot().SetChildValue("Build","auto"); EXPECT_EQ(Execute(),6);
        EXPECT_FALSE(std::filesystem::exists(Output/"ExportComplete.json")); Testing::RemoveTestWorkDirectory(Output);
        // Cook's cwd is the fixture workspace, so no source-checkout files are touched.
        Request.GetMutableRoot().SetChildValue("Build","reuse"); Request.GetMutableRoot().SetChildValue("Cook","auto"); EXPECT_EQ(Execute(),7);
        EXPECT_FALSE(std::filesystem::exists(Output/"ExportComplete.json")); Testing::RemoveTestWorkDirectory(Output);
        Request.GetMutableRoot().SetChildValue("Cook","reuse"); Request.GetMutableRoot().SetChildValue("Archive",true); EXPECT_EQ(Execute(),10);
        EXPECT_FALSE(std::filesystem::exists(Output/"ExportComplete.json"));
    }
    TEST_F(FGameExportTests, ExplicitReceiptMustMatchSelectedBackendWithoutAllocatingOutput)
    {
        Request.GetMutableRoot().SetChildValue("Operation","export");
        Request.GetMutableRoot().SetChildValue("Receipt",(Cook/"CookManifest.bin").generic_string());
        EXPECT_NE(ExecuteFresh(),0); EXPECT_FALSE(std::filesystem::exists(Output));
        Request.GetMutableRoot().SetChildValue("Receipt",(Root/"build/TargetReceipt.json").generic_string());
        Request.GetMutableRoot().SetChildValue("Preview",true);
        EXPECT_EQ(Execute(),0); EXPECT_FALSE(std::filesystem::exists(Output));
    }
    TEST_F(FGameExportTests, LowLevelStagePreviewUsesReadOnlyConfigurationCheck)
    {
        Request.GetMutableRoot().SetChildValue("Preview",true);
        EXPECT_EQ(Execute(),0); EXPECT_FALSE(std::filesystem::exists(Output));
    }
    TEST_F(FGameExportTests, ExportCancellationBeforeAllocationLeavesNoMarker)
    {
        Request.GetMutableRoot().SetChildValue("Operation","export");
        EXPECT_EQ(Execute([]{return true;}),130);
        EXPECT_FALSE(std::filesystem::exists(Output));
    }
    TEST_F(FGameExportTests, PreviewDoesNotCreateOutputOrClaimProductFingerprint)
    {
        Request.GetMutableRoot().SetChildValue("Operation","preview"); ASSERT_EQ(Execute(),0);
        EXPECT_FALSE(std::filesystem::exists(Output)); FJsonDocument Preview; ASSERT_TRUE(Preview.Parse(Events.back()));
        Request.GetMutableRoot().SetChildValue("Operation","stage"); ASSERT_EQ(Execute(),0);
        FJsonDocument Result; ASSERT_TRUE(Result.Parse(Events.back()));
        EXPECT_TRUE(Preview.GetRootView().GetView("PlanFingerprint").IsNull());
        EXPECT_TRUE(Result.GetRootView().GetView("PlanFingerprint").IsString());
        EXPECT_TRUE(std::filesystem::is_regular_file(Output/"Runtime.json")); EXPECT_FALSE(std::filesystem::exists(Output/"Engine/Content"));
        EXPECT_TRUE(ReadRuntimePathContext(Output/"Runtime.json",Output/"Binaries/Win64/Example.exe",false));
        EXPECT_FALSE(std::filesystem::exists(Output/"Example.dproject"));
    }
    TEST_F(FGameExportTests, ChangedTargetReceiptRequiresRecookAndPreservesCommittedOutput)
    {
        ASSERT_EQ(Execute(),0) << Events.back();
        const auto Previous=FileSha256(Output/"Runtime.json");
        // Even an equivalent re-encoded receipt differs from the captured Cook input.
        Write(Root/"build/TargetReceipt.json",Receipt.ToString()+"\n");
        EXPECT_NE(ExecuteFresh(),0);
        EXPECT_EQ(FileSha256(Output/"Runtime.json"),Previous);
        EXPECT_NE(Events.back().find("Cook target receipt differs"),std::string::npos);
    }
    TEST_F(FGameExportTests, CorruptCookAndChangedSelectionPreserveCommittedOutput)
    {
        Write(Output/"previous","keep"); Write(Cook/"Shaders/ShaderLibrary.dslb","corrupt");
        EXPECT_NE(ExecuteFresh(),0); EXPECT_TRUE(std::filesystem::exists(Output/"previous"));
        Write(Project,R"({"ProjectName":"Changed","BaseModules":[]})"); EXPECT_NE(ExecuteFresh(),0);
        EXPECT_TRUE(std::filesystem::exists(Output/"previous"));
    }
    TEST_F(FGameExportTests, ChangedPluginStartupConfigurationRequiresRecook)
    {
        ASSERT_EQ(Execute(),0) << Events.back();
        Write(Output/"previous","keep");
        Write(Project.parent_path()/"Configs/Plugins.json",R"({"Plugins":{}})");
        EXPECT_NE(ExecuteFresh(),0);
        EXPECT_TRUE(std::filesystem::exists(Output/"previous"));
        EXPECT_NE(Events.back().find("Plugin startup configuration differs"),std::string::npos);
    }
    TEST_F(FGameExportTests, CancellationAcknowledgesCleanupWithoutReplacingOutput)
    {
        Write(Output/"previous","keep"); EXPECT_EQ(Execute([]{return true;}),130);
        EXPECT_TRUE(std::filesystem::exists(Output/"previous"));
        ASSERT_GE(Events.size(),2u); FJsonDocument Ack; ASSERT_TRUE(Ack.Parse(Events[Events.size()-2]));
        EXPECT_EQ(Ack.GetRootView().GetView("Kind").GetString(),"cancelAcknowledged");
        EXPECT_TRUE(Ack.GetRootView().GetView("ChildrenRetired").GetBool());
    }
    TEST_F(FGameExportTests, RejectsProtocolAndUnsupportedCapabilities)
    {
        Request.GetMutableRoot().SetChildValue("ProtocolVersion",2); EXPECT_EQ(Execute(),2);
        Request.GetMutableRoot().SetChildValue("ProtocolVersion",1); Request.GetMutableRoot().SetChildValue("Backend","prebuilt"); EXPECT_EQ(Execute(),3);
        EXPECT_FALSE(std::filesystem::exists(Output));
    }
    TEST_F(FGameExportTests, OrdinaryAndDelayImportsSelectOnlyRequiredThirdPartyLibraries)
    {
        Write(Runtime/"leaf.dll",EmptyPE()); Write(Runtime/"unrelated.dll",EmptyPE());
        AddNativeProduct("Core",Runtime/"leaf.dll","Binaries/Win64/leaf.dll","NativeLibrary");
        for (bool Delay:{false,true})
        {
            Write(Runtime/"DurinGame-Core.dll",ImportPE(Delay)); CommitReceipt();
            if(std::filesystem::exists(Output)) { Testing::RemoveTestWorkDirectory(Output); Testing::RemoveTestWorkDirectory(std::filesystem::path(Output.generic_string()+".evidence")); }
            ASSERT_EQ(Execute(),0) << Events.back();
            const auto Staged=Output/"Binaries/Win64";
            EXPECT_TRUE(std::filesystem::exists(Staged/"leaf.dll")); EXPECT_FALSE(std::filesystem::exists(Staged/"unrelated.dll"));
        }
        std::filesystem::remove(Runtime/"leaf.dll"); EXPECT_NE(ExecuteFresh(),0);
        EXPECT_TRUE(std::filesystem::exists(Output/"Runtime.json"));
    }
    TEST_F(FGameExportTests, CancelDuringCopyLeavesPartialDirectoryWithoutCompletion)
    {
        bool Cancelled=false;
        EXPECT_EQ(Execute([&]{return Cancelled;},[&](std::string_view Event){
            FJsonDocument D; ASSERT_TRUE(D.Parse(Event));
            if(D.GetRootView().GetView("Phase").GetString()=="stage") Cancelled=true;
        }),130);
        EXPECT_TRUE(std::filesystem::is_directory(Output));
        EXPECT_FALSE(std::filesystem::exists(Output/"ExportComplete.json"));
        EXPECT_EQ(Execute(),2);
    }
    TEST_F(FGameExportTests, WriterOwnershipAndOverlappingInputsRejectBeforePublication)
    {
        Write(Output/"previous","keep");
        EXPECT_EQ(Execute(),2); EXPECT_TRUE(std::filesystem::exists(Output/"previous"));
        Request.GetMutableRoot().SetChildValue("Output",Runtime.generic_string()); EXPECT_NE(Execute(),0);
        EXPECT_TRUE(std::filesystem::exists(Runtime/"DurinGame.exe"));
        const auto Dependencies=Root/"dependencies"; Write(Dependencies/"keep","dependency");
        FJsonDocument Backend; ASSERT_TRUE(Backend.LoadFromFile(Root/"backend.json"));
        Backend.GetMutableRoot().SetChildValue("DependenciesRoot",Dependencies.generic_string()); Write(Root/"backend.json",Backend.ToString());
        Request.GetMutableRoot().SetChildValue("Output",Dependencies.generic_string()); EXPECT_NE(Execute(),0);
        EXPECT_TRUE(std::filesystem::is_regular_file(Dependencies/"keep"));
    }
    TEST_F(FGameExportTests, ReceiptAloneBindsDetachedPluginRuntimeEnvelope)
    {
        const std::string Descriptor=R"({"ProjectName":"Example","BaseModules":[]})";
        const std::string Startup=R"({"Plugins":{"Water":true}})";
        Write(Project.parent_path()/"Configs/Plugins.json",Startup);
        Write(Project,Descriptor);
        Write(Project.parent_path()/"Plugins/Water/Water.dplugin",R"({"SchemaVersion":1,"Id":"Water","Modules":[{"Name":"Water","Path":"Source"}]})");
        Write(Project.parent_path()/"Plugins/Water/Source/Water.dmodule",R"({"ModuleName":"Water"})");
        FPluginResolveRequest Resolve; Resolve.EngineRoot=Engine; Resolve.ProjectFile=Project; Resolve.ProjectFiles={Engine/"Engine.dproject",Project}; Resolve.RuntimeVariant="DurinGame";
        const auto Selection=ResolvePlugins(Resolve); ASSERT_TRUE(Selection) << Selection.error();
        FJsonDocument Snapshot; ASSERT_TRUE(Snapshot.LoadFromFile(Cook/"ExportEvidence.json"));
        Snapshot.GetMutableRoot().SetChildValue("Selection",Selection->ManifestJson); Snapshot.GetMutableRoot().SetChildValue("ProjectDescriptor",Descriptor);
        Snapshot.GetMutableRoot().SetChildValue("PluginStartupConfig",Startup);
        Write(Cook/"ExportEvidence.json",Snapshot.ToString()); CommitCook();
        const auto Inventory=Root/"build/PluginBuild.json";
        Write(Inventory.parent_path()/"Runtime/Water.dll",EmptyPE()); Write(Inventory.parent_path()/"Runtime/native.dll",EmptyPE());
        FJsonDocument Frozen; ASSERT_TRUE(Frozen.Parse(Selection->ManifestJson));
        auto Captured=Receipt.GetMutableRoot().AddArray("BuildInputs"); const auto Inputs=Frozen.GetRootView().GetView("Inputs");
        for(size_t I=0;I<Inputs.Num();++I) {auto Item=Captured.AppendObject(); Item.SetChildValue("Path",Inputs.GetView(I).GetView("Path").GetString()); Item.SetChildValue("Hash",Inputs.GetView(I).GetView("Hash").GetString());}
        auto Water=AddNativeModule("Water","Water"); Water.GetRef("Linked").AppendValue("Core");
        Water.SetChildValue("Phase","Providers"); Water.SetChildValue("Binary","Binaries/Win64/Water.dll");
        AddNativeProduct("Water",Inventory.parent_path()/"Runtime/Water.dll","Binaries/Win64/Water.dll","NativeLibrary");
        AddNativeProduct("Water",Inventory.parent_path()/"Runtime/native.dll","Binaries/Win64/native.dll","NativeLibrary"); CommitReceipt();
        Write(Inventory,"not a valid legacy inventory");
        ASSERT_EQ(Execute(),0) << Events.back();
        const auto Previous=FileSha256(Output/"Runtime.json"),Original=Receipt.ToString();
        const auto RejectReceipt=[&] {Write(Root/"build/TargetReceipt.json",Receipt.ToString()); CommitCook(); EXPECT_NE(ExecuteFresh(),0); EXPECT_EQ(FileSha256(Output/"Runtime.json"),Previous); ASSERT_TRUE(Receipt.Parse(Original));};
        NativeModule("Water").SetChildValue("Phase","EngineReady"); RejectReceipt();
        NativeModule("Water").SetChildValue("Binary","Binaries/Win64/Missing.dll"); RejectReceipt();
        auto BuiltInputs=Receipt.GetMutableRoot().GetRef("BuildInputs");
        for(size_t I=0;I<BuiltInputs.Num();++I) if(BuiltInputs.GetView(I).GetView("Path").GetString().starts_with("Plugin/Water")) {BuiltInputs.GetRef(I).SetChildValue("Hash","stale"); break;}
        RejectReceipt(); CommitReceipt();
        FJsonDocument RuntimeManifest; ASSERT_TRUE(RuntimeManifest.LoadFromFile(Output/"Runtime.json"));
        const auto View=RuntimeManifest.GetRootView(); EXPECT_EQ(View.GetView("Plugins").GetView(0).GetString(),"Water"); EXPECT_EQ(View.GetView("Binaries").Num(),1u);
        FPluginResolution Admitted; std::vector<FAdmittedPluginBinary> Binaries; FPluginResources Resources; std::string Error;
        const FPluginBuildIdentity Host{"fixture:"+FileSha256(Runtime/"DurinGame-Core.dll"),"Win64","x64","Debug","DurinGame","fixture-abi"};
        EXPECT_TRUE(ReadPluginRuntimeInventory(Output/"Runtime.json","",Host,Admitted,Binaries,Resources,Error)) << Error;
        EXPECT_EQ(Binaries.size(),1u);
        std::filesystem::remove(Inventory.parent_path()/"Runtime/native.dll"); EXPECT_NE(ExecuteFresh(),0); EXPECT_TRUE(std::filesystem::exists(Output/"Runtime.json"));
    }
    TEST_F(FGameExportTests, StageMarkerFailureLeavesUnmarkedDeliveryAndRejectsRetry)
    {
        const auto Code=Execute({},[&](std::string_view Event){
            FJsonDocument D; ASSERT_TRUE(D.Parse(Event));
            if(D.GetRootView().GetView("Phase").GetString()=="publish")
                std::filesystem::create_directory(Output/"ExportComplete.json");
        });
        EXPECT_EQ(Code,11);
        EXPECT_TRUE(std::filesystem::exists(Output/"Runtime.json"));
        EXPECT_FALSE(std::filesystem::is_regular_file(Output/"ExportComplete.json"));
        EXPECT_EQ(Execute(),2);
    }
    TEST_F(FGameExportTests, InvalidCorrelationUsesSchemaValidFailureIdentity)
    {
        Request.GetMutableRoot().SetChildValue("RequestId","123-invalid"); EXPECT_EQ(Execute(),2);
        FJsonDocument Result; ASSERT_TRUE(Result.Parse(Events.back()));
        EXPECT_EQ(Result.GetRootView().GetView("RequestId").GetString(),"InvalidRequest");
    }
    TEST_F(FGameExportTests, DeliveryContainsOnlyRuntimeSnapshotAndExternalEvidence)
    {
        ASSERT_EQ(Execute(),0)<<Events.back();
        EXPECT_TRUE(std::filesystem::exists(Output/"Binaries/Win64/Example.exe"));
        EXPECT_TRUE(std::filesystem::exists(Output/"Config/Defaults/DurinGame.yaml"));
        EXPECT_FALSE(std::filesystem::exists(Output/"Content/ExportEvidence.json"));
        EXPECT_FALSE(std::filesystem::exists(Output/"NativeInputs.txt"));
        FJsonDocument Minimal; ASSERT_TRUE(Minimal.LoadFromFile(Output/"Content/RuntimeCook.json"));
        EXPECT_FALSE(Minimal.GetRootView().Contains("ProjectDescriptor")); EXPECT_FALSE(Minimal.GetRootView().Contains("PluginStartupConfig"));
        FJsonDocument Frozen; ASSERT_TRUE(Frozen.Parse(Minimal.GetRootView().GetView("Selection").GetString()));
        EXPECT_FALSE(Frozen.GetRootView().Contains("Inputs"));
        const auto Bytes=FFileHelper::LoadFileToArray(Output/"Content/CookManifest.bin"); ASSERT_TRUE(Bytes);
        FCookManifest CM; ASSERT_TRUE(DecodeCookManifest(*Bytes,CM));
        EXPECT_TRUE(std::ranges::none_of(CM.Entries,[](const auto& E){return E.RelativePath=="ExportEvidence.json";}));
        const auto Evidence=std::filesystem::path(Output.generic_string()+".evidence");
        ASSERT_TRUE(std::filesystem::is_directory(Evidence));
        EXPECT_TRUE(std::filesystem::is_regular_file(Evidence/"ExportEvidence.json"));
        EXPECT_TRUE(std::filesystem::is_regular_file(Evidence/"TargetReceipt.json"));
        FJsonDocument Selected; ASSERT_TRUE(Selected.LoadFromFile(Evidence/"TargetReceipt.json"));
        EXPECT_EQ(Selected.GetRootView().GetView("CookCompatibility").GetString(),Receipt.GetRootView().GetView("CookCompatibility").GetString());
        Request.GetMutableRoot().SetChildValue("Output",(Root/"RenamedExport").generic_string()); ASSERT_EQ(Execute(),0);
        FJsonDocument A,B; ASSERT_TRUE(A.LoadFromFile(Output/"Runtime.json")); ASSERT_TRUE(B.LoadFromFile(Root/"RenamedExport/Runtime.json"));
        EXPECT_EQ(A.GetRootView().GetView("ApplicationId").GetString(),B.GetRootView().GetView("ApplicationId").GetString());
        EXPECT_EQ(A.ToString(),B.ToString());
    }
    TEST_F(FGameExportTests, MissingFrozenConfigOrRuntimeSnapshotPreservesOutput)
    {
        Write(Output/"previous","keep"); std::filesystem::remove(Cook/"RuntimeCook.json");
        EXPECT_NE(ExecuteFresh(),0); EXPECT_TRUE(std::filesystem::is_regular_file(Output/"previous"));
    }
    TEST_F(FGameExportTests, DynamicModuleAndRuntimeDataAreDeclaredWithoutDirectoryDiscovery)
    {
        Write(Runtime/"Dynamic.dll",EmptyPE()); Write(Runtime/"payload.bin","declared data"); Write(Runtime/"DurinGame-Stale.dll",EmptyPE());
        AddNativeModule("Dynamic","Engine").GetRef("Linked").AppendValue("Core");
        NativeModule("DurinLauncher").GetRef("Dynamic").AppendValue("Dynamic");
        AddNativeProduct("Dynamic",Runtime/"Dynamic.dll","Binaries/Win64/Dynamic.dll","NativeLibrary");
        AddNativeProduct("Dynamic",Runtime/"payload.bin","Resources/payload.bin","RuntimeData"); CommitReceipt();
        ASSERT_EQ(Execute(),0)<<Events.back();
        EXPECT_TRUE(std::filesystem::exists(Output/"Binaries/Win64/Dynamic.dll"));
        EXPECT_TRUE(std::filesystem::exists(Output/"Resources/payload.bin"));
        FJsonDocument Manifest; ASSERT_TRUE(Manifest.LoadFromFile(Output/"Runtime.json"));
        EXPECT_EQ(Manifest.GetRootView().GetView("NativeLibraries").Num(),2u);
        EXPECT_FALSE(std::filesystem::exists(Output/"Binaries/Win64/DurinGame-Stale.dll"));
        const auto Previous=FileSha256(Output/"Runtime.json"); Write(Runtime/"payload.bin","changed");
        EXPECT_NE(ExecuteFresh(),0); EXPECT_EQ(FileSha256(Output/"Runtime.json"),Previous);
    }
    TEST_F(FGameExportTests, ReceiptRejectsWrongIdentityMissingEdgesEditorAndInactiveOwners)
    {
        Write(Output/"previous","keep"); const auto Original=Receipt.ToString();
        const auto Reject=[&] { Write(Root/"build/TargetReceipt.json",Receipt.ToString()); CommitCook(); EXPECT_NE(ExecuteFresh(),0); EXPECT_TRUE(std::filesystem::exists(Output/"previous")); ASSERT_TRUE(Receipt.Parse(Original)); };
        Receipt.GetMutableRoot().GetRef("Identity").SetChildValue("ToolchainABI","wrong"); Reject();
        NativeModule("Core").SetChildValue("Host","Editor"); Reject();
        NativeModule("Core").GetRef("Dynamic").AppendValue("Absent"); Reject();
        NativeModule("Core").GetRef("RuntimeFiles").AppendValue("Missing/data.bin"); Reject();
        AddNativeModule("Inactive","Water"); NativeModule("Core").GetRef("Dynamic").AppendValue("Inactive"); Reject();
        AddNativeProduct("Core",Runtime/"DurinGame-Core.dll","Binaries/Win64/Example.exe","NativeLibrary"); Reject();
        Write(Runtime/"Alternate.dll",EmptyPE()+"different bytes");
        AddNativeProduct("Core",Runtime/"Alternate.dll","Other/DurinGame-Core.dll","NativeLibrary"); Reject();
    }
    TEST_F(FGameExportTests, UndeclaredImportsNeverAuthorizeFilesFoundBesideTheExecutable)
    {
        Write(Runtime/"leaf.dll",EmptyPE()); Write(Runtime/"DurinGame-Core.dll",ImportPE(false)); CommitReceipt();
        Write(Output/"previous","keep"); EXPECT_NE(ExecuteFresh(),0);
        EXPECT_TRUE(std::filesystem::exists(Output/"previous"));
        EXPECT_NE(Events.back().find("Missing declared native dependency"),std::string::npos);
        AddNativeProduct("Core",Runtime/"leaf.dll","Binaries/Win64/leaf.dll","RuntimeData"); CommitReceipt();
        EXPECT_NE(ExecuteFresh(),0); EXPECT_NE(Events.back().find("Missing declared native dependency"),std::string::npos);
    }

    TEST_F(FGameExportTests, WrongPEMachineCannotMasqueradeAsAnX64Receipt)
    {
        auto Binary=EmptyPE(); const uint16 Machine=0x14c; std::memcpy(Binary.data()+68,&Machine,2);
        Write(Runtime/"DurinGame-Core.dll",Binary); CommitReceipt(); Write(Output/"previous","keep");
        EXPECT_NE(ExecuteFresh(),0); EXPECT_TRUE(std::filesystem::exists(Output/"previous"));
        EXPECT_NE(Events.back().find("x64 PE machine"),std::string::npos);
    }

    TEST_F(FGameExportTests, ReceiptProductsCannotReplaceGeneratedConfigOrCookFiles)
    {
        Write(Output/"previous","keep"); Write(Runtime/"data.bin","native data"); const auto Original=Receipt.ToString();
        for(const auto* Destination:{"Config/DurinGame.yaml","Content/Shaders/ShaderLibrary.dslb"})
        {
            AddNativeProduct("Core",Runtime/"data.bin",Destination,"RuntimeData"); CommitReceipt();
            EXPECT_NE(ExecuteFresh(),0); EXPECT_TRUE(std::filesystem::is_regular_file(Output/"previous"));
            EXPECT_NE(Events.back().find("Colliding delivery destination"),std::string::npos);
            ASSERT_TRUE(Receipt.Parse(Original));
        }
    }

}

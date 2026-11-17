#include "Misc/RuntimePaths.h"
#include "Misc/Paths.h"
#include "Json/Json.h"
#include "NativeTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
    namespace
    {
        class FDeploymentBootstrapTests : public testing::Test
        {
        protected:
            std::filesystem::path Root;
            FJsonDocument Manifest;
            void SetUp() override
            {
                Root=Testing::CreateTestFixtureDirectory("DeploymentBootstrap");
                ASSERT_TRUE(Manifest.LoadFromFile(std::filesystem::path(DURIN_EXPORT_FIXTURES)/"runtime/no-plugin.json"));
                auto M=Manifest.GetMutableRoot(); auto Files=M.GetRef("Files");
                for (size_t I=0;I<Files.Num();++I)
                {
                    auto Entry=Files.GetRef(I); const auto Path=ResolveDeploymentPath(Root,Entry.GetView("Path").GetString());
                    std::filesystem::create_directories(Path.parent_path()); std::ofstream(Path,std::ios::binary)<<"fixture";
                    Entry.SetChildValue("Sha256",FileSha256(Path));
                }
                for (const auto* Key:{"Manifest","Snapshot","ShaderLibrary"})
                { auto Entry=M.GetRef("Cook").GetRef(Key); Entry.SetChildValue("Sha256",FileSha256(Root/Entry.GetView("Path").GetString())); }
                std::filesystem::create_directories(Root/"Content/Engine"); std::filesystem::create_directories(Root/"Content/Game");
            }
            void TearDown() override { Testing::RemoveTestWorkDirectory(Root); }
            auto Read() -> std::expected<FRuntimePathContext,std::string>
            { Manifest.SaveToFile((Root/"Runtime.json").generic_string()); return ReadRuntimePathContext(Root/"Runtime.json",Root/"Binaries/Win64/Sandbox.exe",false,Root.parent_path()/"Users"); }
        };
    }
    TEST_F(FDeploymentBootstrapTests, BindsDetachedRootsWithoutEngineSentinel)
    {
        const auto Context=Read(); ASSERT_TRUE(Context) << Context.error();
        EXPECT_EQ(Context->Bindings.at("Root"),std::filesystem::canonical(Root).generic_string()+"/");
        EXPECT_EQ(Context->Bindings.at("Cook"),(Root/"Content").generic_string()+"/");
        EXPECT_FALSE(std::filesystem::exists(Root/"Engine/Content"));
        EXPECT_EQ(GetRuntimePathContext(),nullptr);
    }
    TEST_F(FDeploymentBootstrapTests, MissingCorruptAndOldManifestsFailWithoutSourceFallback)
    {
        EXPECT_FALSE(ReadRuntimePathContext(Root/"missing.json",Root/"Binaries/Win64/Sandbox.exe",false));
        std::ofstream(Root/"corrupt.json")<<"{";
        EXPECT_FALSE(ReadRuntimePathContext(Root/"corrupt.json",Root/"Binaries/Win64/Sandbox.exe",false));
        Manifest.GetMutableRoot().SetChildValue("SchemaVersion",1); EXPECT_FALSE(Read());
        EXPECT_EQ(GetRuntimePathContext(),nullptr);
    }
    TEST_F(FDeploymentBootstrapTests, RejectsChangedRequiredFilesBeforePublishing)
    {
        std::ofstream(Root/"Config/Project.yaml")<<"tampered"; EXPECT_FALSE(Read());
        EXPECT_EQ(GetRuntimePathContext(),nullptr);
    }
    TEST_F(FDeploymentBootstrapTests, RejectsWrongExecutableAndDuplicateFiles)
    {
        ASSERT_TRUE(Read()); EXPECT_FALSE(ReadRuntimePathContext(Root/"Runtime.json",Root/"other.exe",false));
        auto Files=Manifest.GetMutableRoot().GetRef("Files"); auto Item=Files.AppendObject();
        Item.SetChildValue("Path","Config/Project.yaml"); Item.SetChildValue("Sha256",FileSha256(Root/"Config/Project.yaml"));
        EXPECT_FALSE(Read());
    }
    TEST_F(FDeploymentBootstrapTests, PreservesUtf8DeploymentPaths)
    {
        const std::filesystem::path Relative(u8"Content/资源/材质.bin");
        const auto Expected=std::filesystem::weakly_canonical(Root)/Relative;
        EXPECT_EQ(ResolveDeploymentPath(Root,"Content/资源/材质.bin"),Expected);
    }
    TEST_F(FDeploymentBootstrapTests, RejectsLexicalEscapesAndCanonicalAliases)
    {
        for (const auto* Text:{"../outside","/outside","C:outside","Data\\outside","Data/../outside","Data//file","Data/./file","Data/file:stream","Data/nul.txt","Data/file.","Data/file "})
            EXPECT_THROW(ResolveDeploymentPath(Root,Text),std::runtime_error) << Text;
        const auto Outside=Testing::CreateTestFixtureDirectory("OutsideDeployment");
        std::error_code Error; std::filesystem::create_directory_symlink(Outside,Root/"alias",Error);
        if (!Error) EXPECT_THROW(ResolveDeploymentPath(Root,"alias/file"),std::runtime_error);
        Testing::RemoveTestWorkDirectory(Outside);
    }
    TEST_F(FDeploymentBootstrapTests, StableIdentityBindsSavedAndCacheOutsideInstallation)
    {
        const auto Context=Read(); ASSERT_TRUE(Context)<<Context.error();
        const auto User=ResolveRuntimeUserDataRoot("org.durin.Sandbox",Root.parent_path()/"Users");
        EXPECT_EQ(Context->Bindings.at("Saved"),(User/"Saved").generic_string()+"/");
        EXPECT_EQ(Context->Bindings.at("Cache"),(User/"Cache").generic_string()+"/");
        EXPECT_FALSE(std::filesystem::exists(User));
        EXPECT_FALSE(ReadRuntimePathContext(Root/"Runtime.json",Root/"Binaries/Win64/Sandbox.exe",false,Root/"Users"));
        for (const auto* Id:{"../outside","C:bad","bad/name","", ".hidden"})
            EXPECT_THROW(ResolveRuntimeUserDataRoot(Id,Root.parent_path()/"Users"),std::runtime_error);
        Manifest.GetMutableRoot().GetRef("UserData").SetChildValue("Policy","ExecutableSaved");
        EXPECT_FALSE(Read());
    }
    TEST_F(FDeploymentBootstrapTests, CachesOnlyVerifiedNativeLibraries)
    {
        std::ofstream(Root/"Binaries/Win64/Fixture.dll")<<"library";
        auto M=Manifest.GetMutableRoot(); auto File=M.GetRef("Files").AppendObject();
        File.SetChildValue("Path","Binaries/Win64/Fixture.dll"); File.SetChildValue("Sha256",FileSha256(Root/"Binaries/Win64/Fixture.dll"));
        M.AddArray("NativeLibraries").AppendValue("Binaries/Win64/Fixture.dll");
        const auto Context=Read(); ASSERT_TRUE(Context)<<Context.error(); EXPECT_EQ(Context->NativeLibraries.size(),1u);
        M.GetRef("NativeLibraries").AppendValue("Binaries/Win64/Unverified.dll"); EXPECT_FALSE(Read());
    }
    TEST(FDeploymentHashTests, MatchesSha256Golden)
    {
        const std::string Text="abc";
        EXPECT_EQ(BytesSha256(std::as_bytes(std::span(Text.data(),Text.size()))),"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
}

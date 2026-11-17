#include "Misc/RuntimePaths.h"
#include "Misc/Paths.h"
#include "Misc/Build.h"
#include "Misc/FileHelper.h"
#include "Misc/StringHelper.h"
#include "Json/Json.h"
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#include <bcrypt.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace Durin
{
    namespace { std::optional<FRuntimePathContext> Context; }
    auto GetRuntimePathContext() -> const FRuntimePathContext* { return Context ? &*Context : nullptr; }
    auto RuntimePathBinding(std::string_view Key) -> std::string
    {
        if (!Context) return {};
        const auto It=Context->Bindings.find(std::string(Key));
        return It==Context->Bindings.end()?std::string{}:It->second;
    }
    auto PublishRuntimePathContext(FRuntimePathContext Value) -> bool
    {
        if (Context) return false;
        Context=std::move(Value); return true;
    }
    auto ResolveDeploymentPath(const std::filesystem::path& Root,std::string_view Relative,bool bAllowRoot) -> std::filesystem::path
    {
        if (bAllowRoot && Relative==".") return std::filesystem::canonical(Root);
        if (Relative.empty() || Relative.front()=='/' || Relative.back()=='/'
            || Relative.find_first_of("\\:<>\"|?*")!=std::string_view::npos
            || std::ranges::any_of(Relative,[](unsigned char C){return C<32;}))
            throw std::runtime_error("Invalid deployment path: "+std::string(Relative));
        const auto Path=std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));
        if (Path.is_absolute() || Path.has_root_name() || Relative.find("//")!=std::string_view::npos)
            throw std::runtime_error("Invalid deployment path");
        for (const auto& Part:Path)
        {
            const auto Text=Part.generic_string();
            const auto Name=StringUtils::FoldAscii(Text.substr(0,Text.find('.')));
            if (Text=="." || Text==".." || Text.back()=='.' || Text.back()==' '
                || Name=="con" || Name=="prn" || Name=="aux" || Name=="nul"
                || (Name.size()==4 && (Name.starts_with("com") || Name.starts_with("lpt")) && Name[3]>='1' && Name[3]<='9'))
                throw std::runtime_error("Invalid deployment path component: "+Text);
        }
        std::filesystem::path Resolved; std::error_code Error;
        if (!FPaths::TryResolveContainedPath(std::filesystem::weakly_canonical(Root)/Path,
                std::filesystem::weakly_canonical(Root),Resolved,Error))
            throw std::runtime_error("Deployment path escapes root: "+std::string(Relative));
        return Resolved;
    }
    auto BytesSha256(std::span<const std::byte> Bytes) -> std::string
    {
        std::array<unsigned char,32> Digest{};
#if defined(_WIN32)
        if (Bytes.size()>std::numeric_limits<ULONG>::max()
            || BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(Bytes.data())),
                static_cast<ULONG>(Bytes.size()),Digest.data(),static_cast<ULONG>(Digest.size()))<0)
            throw std::runtime_error("Cannot hash runtime file");
#elif defined(__APPLE__)
        if (Bytes.size()>std::numeric_limits<CC_LONG>::max()) throw std::runtime_error("Hash input too large");
        CC_SHA256(Bytes.data(),static_cast<CC_LONG>(Bytes.size()),Digest.data());
#endif
        std::string Result; for (auto Byte:Digest) Result+=std::format("{:02x}",Byte); return Result;
    }
    auto FileSha256(const std::filesystem::path& File) -> std::string
    {
        const auto Bytes=FFileHelper::LoadFileToArray(File);
        if (!Bytes) throw std::runtime_error("Cannot hash required file: "+File.generic_string());
        return BytesSha256(*Bytes);
    }
    auto ResolveRuntimeUserDataRoot(std::string_view ApplicationId,const std::filesystem::path& LocalAppDataOverride) -> std::filesystem::path
    {
        const auto Letter=[](char C){return (C>='A'&&C<='Z') || (C>='a'&&C<='z');};
        if (ApplicationId.empty() || ApplicationId.size()>128 || !Letter(ApplicationId.front())
            || !std::ranges::all_of(ApplicationId,[&](char C){return Letter(C) || (C>='0'&&C<='9') || C=='.' || C=='_' || C=='-';}))
            throw std::runtime_error("Invalid stable application identity");
        auto Base=LocalAppDataOverride;
        if (Base.empty())
        {
#if defined(_WIN32)
            PWSTR Folder=nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DONT_VERIFY,nullptr,&Folder)))
                throw std::runtime_error("Cannot resolve LocalAppData; no writable fallback is permitted");
            Base=Folder; CoTaskMemFree(Folder);
#else
            throw std::runtime_error("LocalAppData deployment policy is available on Win64 only");
#endif
        }
        if (!Base.is_absolute()) throw std::runtime_error("User-data root must be absolute");
        return std::filesystem::weakly_canonical(Base/"Durin"/std::string(ApplicationId));
    }
    auto ReadRuntimePathContext(const std::filesystem::path& Manifest,const std::filesystem::path& Executable, bool bValidateHost,
        const std::filesystem::path& LocalAppDataOverride)
        -> std::expected<FRuntimePathContext,std::string>
    {
        try
        {
            FJsonDocument Document;
            if (!Document.LoadFromFile(Manifest)) throw std::runtime_error("Missing or invalid Runtime.json; restage the Game");
            const auto Root=Document.GetRootView();
            const auto UniqueKeys=[](auto&& Self,FJsonNodeView Node,size_t Depth) -> void {
                if (Depth>32) throw std::runtime_error("Runtime manifest nesting exceeds limit");
                if (Node.IsObject())
                {
                    std::set<std::string> Keys;
                    Node.ForEachObjectMember([&](std::string_view Key,FJsonNodeView Value){
                        if (!Keys.insert(std::string(Key)).second) throw std::runtime_error("Duplicate runtime field: "+std::string(Key));
                        Self(Self,Value,Depth+1);
                    });
                }
                else if (Node.IsArray()) for (size_t I=0;I<Node.Num();++I) Self(Self,Node.GetView(I),Depth+1);
            };
            UniqueKeys(UniqueKeys,Root,0);
            if (!Root.GetView("SchemaVersion").IsInt() || Root.GetView("SchemaVersion").GetInt()!=2)
                throw std::runtime_error("Unsupported Runtime.json schema; recook/restage the Game");
            const auto Text=[](FJsonNodeView Node,std::string_view Key) {
                const auto V=Node.GetView(Key);
                if (!V.IsString() || V.GetString().empty()) throw std::runtime_error("Missing runtime field: "+std::string(Key));
                return V.GetString();
            };
            const auto Identity=Root.GetView("Identity");
            if (Text(Identity,"RuntimeVariant")!="DurinGame" || Text(Identity,"Platform")!="Win64"
                || Text(Identity,"Architecture")!="x64"
                || (Text(Identity,"Configuration")!="Debug" && Text(Identity,"Configuration")!="Release"))
                throw std::runtime_error("Unsupported runtime target");
            if (bValidateHost && (Text(Identity,"RuntimeVariant")!="DurinGame" || Text(Identity,"Platform")!=DURIN_BUILD_PLATFORM_STRING
                || Text(Identity,"Configuration")!=DURIN_BUILD_TYPE_STRING || Text(Identity,"Architecture")!="x64"))
                throw std::runtime_error("Incompatible runtime target identity");
            const auto Base=std::filesystem::canonical(Manifest).parent_path();
            FRuntimePathContext Result; Result.bStaged=true; Result.Manifest=std::filesystem::canonical(Manifest);
            Result.ManifestJson=Document.ToString(); Result.Bindings["Root"]=Base.generic_string()+"/";
            if (Text(Root.GetView("UserData"),"Policy")!="LocalAppData" || Text(Root.GetView("UserData"),"Namespace")!="Durin")
                throw std::runtime_error("Unsupported writable-data policy; recook/restage with a compatible worker");
            const auto User=ResolveRuntimeUserDataRoot(Text(Root,"ApplicationId"),LocalAppDataOverride);
            const auto Relation=User.lexically_relative(Base);
            if (!Relation.empty() && *Relation.begin()!="..") throw std::runtime_error("User data must be outside the immutable deployment");
            Result.Bindings["Saved"]=(User/"Saved").generic_string()+"/";
            Result.Bindings["Cache"]=(User/"Cache").generic_string()+"/";
            const auto Paths=Root.GetView("Paths");
            for (const auto* Key:{"Engine","Project","EngineContent","ProjectContent","Binaries","ThirdParty","Config","Cook"})
                Result.Bindings[Key]=ResolveDeploymentPath(Base,Text(Paths,Key),true).generic_string()+"/";
            if (ResolveDeploymentPath(Base,Text(Root,"Executable"))!=std::filesystem::canonical(Executable))
                throw std::runtime_error("Runtime executable binding differs from this process");
            const auto Files=Root.GetView("Files");
            if (!Files.IsArray() || !Files.Num() || Files.Num()>65536) throw std::runtime_error("Invalid runtime Files");
            std::map<std::string,std::string> Verified;
            for (size_t I=0;I<Files.Num();++I)
            {
                const auto Item=Files.GetView(I); const auto Path=Text(Item,"Path");
                if (!Verified.emplace(StringUtils::FoldAscii(Path),Text(Item,"Sha256")).second
                    || FileSha256(ResolveDeploymentPath(Base,Path))!=Text(Item,"Sha256"))
                    throw std::runtime_error("Missing or modified runtime file: "+Path);
            }
            const auto Libraries=Root.GetView("NativeLibraries");
            if (Libraries.IsValid() && (!Libraries.IsArray() || Libraries.Num()>65536)) throw std::runtime_error("Invalid native library inventory");
            for(size_t I=0;I<Libraries.Num();++I)
            {
                const auto Relative=Libraries.GetView(I).GetString(); const auto Key=StringUtils::FoldAscii(Relative);
                if (!Verified.contains(Key)) throw std::runtime_error("Unverified native library: "+Relative);
                const auto File=ResolveDeploymentPath(Base,Relative);
                if (!Result.NativeLibraries.emplace(StringUtils::FoldAscii(File.generic_string()),Verified.at(Key)).second)
                    throw std::runtime_error("Duplicate native library: "+Relative);
            }
            const auto Required=[&](FJsonNodeView Node,std::string_view Key) {
                const auto Relative=Text(Node,Key);
                if (!Verified.contains(StringUtils::FoldAscii(Relative))) throw std::runtime_error("Unverified runtime binding: "+Relative);
                return ResolveDeploymentPath(Base,Relative).generic_string();
            };
            (void)Required(Root,"Executable");
            const auto Config=Root.GetView("ConfigurationFiles");
            Result.Bindings["ProjectConfig"]=Required(Config,"Project");
            Result.Bindings["AppConfig"]=Required(Config,"Application");
            Result.Bindings["Defaults"]=Required(Config,"Defaults");
            for (const auto* Key:{"Manifest","Snapshot","ShaderLibrary"})
            {
                const auto Binding=Root.GetView("Cook").GetView(Key);
                const auto File=Required(Binding,"Path");
                if (FileSha256(File)!=Text(Binding,"Sha256")) throw std::runtime_error("Cook binding hash mismatch");
                if (std::string_view(Key)=="Manifest" && std::filesystem::path(File)!=std::filesystem::path(Result.Bindings.at("Cook"))/"CookManifest.bin")
                    throw std::runtime_error("Conflicting Cook root/manifest binding");
                if (std::string_view(Key)=="ShaderLibrary" && std::filesystem::path(File)!=std::filesystem::path(Result.Bindings.at("Cook"))/"Shaders/ShaderLibrary.dslb")
                    throw std::runtime_error("Conflicting Cook root/Shader library binding");
            }
            return Result;
        }
        catch (const std::exception& Error) { return std::unexpected(Error.what()); }
    }
}

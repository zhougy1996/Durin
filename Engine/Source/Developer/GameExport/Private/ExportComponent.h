#pragma once
#include "CoreMinimal.h"
#include "Json/Json.h"
namespace Durin {
    auto ReadExportComponent(const std::filesystem::path& File) -> const FJsonDocument&;
}

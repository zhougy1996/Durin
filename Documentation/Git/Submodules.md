
##### Add a submodule to Durin
```bash
git submodule add --name imgui -b docking https://github.com/ocornut/imgui.git Engine/External/Source/imgui
git submodule add --name glm https://github.com/g-truc/glm.git Engine/External/Source/glm
git submodule add --name glfw https://github.com/glfw/glfw.git Engine/External/Source/glfw
git submodule add --name spdlog https://github.com/gabime/spdlog Engine/External/Source/spdlog
git submodule add --name rapidjson https://github.com/Tencent/rapidjson.git Engine/External/Source/rapidjson
# git submodule add --name fmt https://github.com/fmtlib/fmt.git Engine/External/Source/fmt
```

##### Remove a submodule from Durin
```bash
git rm -f Engine/External/Source/imgui
rm -rf .git/modules/Engine/External/Source/imgui
```

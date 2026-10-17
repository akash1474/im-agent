project "ImAgent"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    staticruntime "On"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files
    {
        "include/**.h",
        "src/**.h",
        "src/**.cpp",
        "third_party/**.h"
    }

    includedirs
    {
        "include",
        "src",
        "third_party/stb",
        "../imgui",
        "../glfw/include",
        "../nlohmann/include",
        "../cpp-httplib"
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/MP", "/utf-8" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "On"
        optimize "Off"
        defines
        {
            "AGENT_BRIDGE=1",
            "IMGUI_AGENT_HOOKS",
            "IMGUI_AGENT_TEXT_HOOK"
        }

    filter "configurations:Release or Dist"
        runtime "Release"
        optimize "On"
        symbols "Off"


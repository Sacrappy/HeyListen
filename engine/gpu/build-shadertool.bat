@echo off
rem Build work\gpu\shadertool.exe against the Eglantine clone's shader recompiler (work\eg\build), with the flags its
rem build uses for the recompiler (work\gpu\sr.rsp, from work\eg\build\build.ninja, warnings-as-errors dropped).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "ROOT=%~dp0.."
set "B=%ROOT%\work\eg\build"
cl /nologo @"%ROOT%\work\gpu\sr.rsp" "%~dp0shadertool.cpp" /Fo"%ROOT%\work\gpu\\" /Fe"%ROOT%\work\gpu\shadertool.exe" /link "%B%\src\shader_recompiler\shader_recompiler.lib" "%B%\src\common\common.lib" "%B%\_deps\fmt-build\fmt.lib" "%B%\_deps\sirit-build\src\sirit.lib" ole32.lib shell32.lib || exit /b 1

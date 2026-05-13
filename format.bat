@echo off
powershell -ExecutionPolicy Bypass -Command "Get-ChildItem -Path src -Recurse -Include *.h,*.cpp,*.cu,*.cuh | ForEach-Object { clang-format -i -style=file:.clang-format $_.FullName }"
powershell -ExecutionPolicy Bypass -Command "Get-ChildItem -Path include -Recurse -Include *.h,*.cpp,*.cu,*.cuh | ForEach-Object { clang-format -i -style=file:.clang-format $_.FullName }"
powershell -ExecutionPolicy Bypass -Command "Get-ChildItem -Path tools -Recurse -Include *.h,*.cpp,*.cu,*.cuh | ForEach-Object { clang-format -i -style=file:.clang-format $_.FullName }"
$vcxproj = 'D:\R6Autoupdatingsource\Dominate.vcxproj'
$content = [System.IO.File]::ReadAllText($vcxproj)

# Fix include dirs: replace vcpkg include path with local Dependencies path
$content = $content -replace [regex]::Escape('C:\vcpkg\installed\x64-windows-static\include'), '$(ProjectDir)Dependencies'

# Fix lib dependencies: replace vcpkg lib paths with local paths
$content = $content -replace [regex]::Escape('C:\vcpkg\installed\x64-windows-static\lib\Zydis.lib'), '$(ProjectDir)Dependencies\ZydisLib\Zydis.lib'
$content = $content -replace [regex]::Escape('C:\vcpkg\installed\x64-windows-static\lib\Zycore.lib'), '$(ProjectDir)Dependencies\ZydisLib\Zycore.lib'

[System.IO.File]::WriteAllText($vcxproj, $content)
Write-Host "vcxproj updated"

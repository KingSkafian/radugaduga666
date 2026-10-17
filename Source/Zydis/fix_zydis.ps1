$file = 'D:\R6Autoupdatingsource\Source\Zydis\Zydis.h'
$content = [System.IO.File]::ReadAllText($file)
$content = $content -replace 'g_driver', 'g_backend'
$content = $content -replace 'if \(offsets::CodeCaveThree\)', '//if (offsets::CodeCaveThree)'
$content = $content -replace 'write_buffer\(\(uint64_t\)offsets::CodeCaveThree, zeros, 8\);', '//write_buffer((uint64_t)offsets::CodeCaveThree, zeros, 8);'
$content = $content -replace 'if \(!offsets::WorldPatch \|\| !backup.world_backed_up\) return;', '//if (!offsets::WorldPatch || !backup.world_backed_up) return;'
$content = $content -replace 'write_buffer\(offsets::WorldPatch, backup.world_original, 5\);', '//write_buffer(offsets::WorldPatch, backup.world_original, 5);'
$content = $content -replace 'signature_scanner::FindFunctionStart', 'FindFunctionStart'
$content = $content -replace 'signature_scanner::scan_pattern', 'scan_pattern'
[System.IO.File]::WriteAllText($file, $content)
Write-Host "Zydis.h patched"

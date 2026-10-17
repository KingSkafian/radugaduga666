$file = 'D:\R6Autoupdatingsource\Source\Zydis\Zydis.h'
$lines = [System.IO.File]::ReadAllLines($file)
$lines[1] = '#include "../../Dependencies/Zydis/Zydis.h"'
$lines[2] = '// Bootstrap.h already includes Windows.h and other standard headers'
[System.IO.File]::WriteAllLines($file, $lines)
Write-Host "Done. Line 2 is now: $($lines[1])"

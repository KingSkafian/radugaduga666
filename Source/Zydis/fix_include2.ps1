$file = 'D:\R6Autoupdatingsource\Source\Zydis\Zydis.h'
$content = [System.IO.File]::ReadAllLines($file)
$content[2] = '#include "../Bootstrap.h"'
[System.IO.File]::WriteAllLines($file, $content)
Write-Host "Added Bootstrap.h include"

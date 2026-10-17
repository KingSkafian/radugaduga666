$vcxproj = 'D:\R6Autoupdatingsource\Dominate.vcxproj'
$content = [System.IO.File]::ReadAllText($vcxproj)
$content = $content -replace '<WholeProgramOptimization>true</WholeProgramOptimization>', '<WholeProgramOptimization>false</WholeProgramOptimization>'
[System.IO.File]::WriteAllText($vcxproj, $content)
Write-Host "Disabled WholeProgramOptimization"

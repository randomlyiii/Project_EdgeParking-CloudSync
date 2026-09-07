$ErrorActionPreference = 'Stop'
$p1 = Get-ChildItem 'E:\download\100ASK-MP157' -Directory | Where-Object { $_.Name -like '100ask-mp157*' } | Select-Object -First 1
$p2 = Get-ChildItem $p1.FullName -Directory | Where-Object { $_.Name -like '01_Base_board*' } | Select-Object -First 1
$pdf = (Get-ChildItem $p2.FullName -File -Filter '100ASK_STM32MP157_PRO_V11*.pdf' | Select-Object -First 1).FullName
$bytes = [System.IO.File]::ReadAllBytes($pdf)
$marker = [System.Text.Encoding]::ASCII.GetBytes('stream')
$n = $bytes.Length
$streams = @(); $si = 0
while ($true) {
  $idx = [Array]::IndexOf($bytes, $marker[0], $si); if ($idx -lt 0) { break }
  $ok = $true; for ($j = 0; $j -lt $marker.Length; $j++) { if ($bytes[$idx+$j] -ne $marker[$j]) { $ok = $false; break } }
  if ($ok) {
    $ds = $idx + 6; if ($bytes[$ds] -eq 13) { $ds++ }; if ($bytes[$ds] -eq 10) { $ds++ }
    $mk = [System.Text.Encoding]::ASCII.GetBytes('endstream'); $end = $ds
    for ($q = $ds; $q -lt $n - 8; $q++) { $m = $true; for ($j = 0; $j -lt $mk.Length; $j++) { if ($bytes[$q+$j] -ne $mk[$j]) { $m = $false; break } }; if ($m) { $end = $q; break } }
    $len = $end - $ds
    if ($len -gt 6) {
      $data = New-Object byte[] $len; [Array]::Copy($bytes, $ds, $data, 0, $len)
      $in = $data[2..($len-5)]
      try { $ms = New-Object System.IO.MemoryStream(,$in); $dsz = New-Object System.IO.Compression.DeflateStream($ms, [System.IO.Compression.CompressionMode]::Decompress); $out = New-Object System.IO.MemoryStream; $dsz.CopyTo($out); $streams += ,$out.ToArray(); $dsz.Dispose() } catch {}
    }
    $si = $end + 9
  } else { $si = $idx + 1 }
}
$all = New-Object System.Text.StringBuilder
foreach ($d in $streams) {
  $s = [System.Text.Encoding]::ASCII.GetString($d)
  $m2 = [regex]::Matches($s, '\(((?:[^()\\]|\\.)*)\)\s*Tj')
  foreach ($mm in $m2) { if ($mm.Groups[1].Success) { [void]$all.Append($mm.Groups[1].Value) } [void]$all.Append("`n") }
}
$dst = 'D:\Projects\Project_EdgeParking-CloudSync\shtext2.txt'
[System.IO.File]::WriteAllText($dst, $all.ToString(), [System.Text.Encoding]::UTF8)
'lines: ' + ($all.ToString().Split("`n").Count)

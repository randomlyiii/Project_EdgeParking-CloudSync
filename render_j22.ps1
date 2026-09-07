$ErrorActionPreference = 'Stop'
$p1 = Get-ChildItem 'E:\download\100ASK-MP157' -Directory | Where-Object { $_.Name -like '100ask-mp157*' } | Select-Object -First 1
$p2 = Get-ChildItem $p1.FullName -Directory | Where-Object { $_.Name -like '01_Base_board*' } | Select-Object -First 1
$pdf = (Get-ChildItem $p2.FullName -File -Filter '100ASK_STM32MP157_PRO_V11*.pdf' | Select-Object -First 1).FullName

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Data.Pdf.PdfDocument, Windows.Data.Pdf, ContentType=WindowsRuntime]
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType=WindowsRuntime]
$null = [Windows.Storage.Streams.DataReader, Windows.Storage.Streams, ContentType=WindowsRuntime]
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1)
function Await($op, $type) { $t = $asTaskGeneric.MakeGenericMethod($type).Invoke($null, @($op)); $t.Wait() | Out-Null; return $t.Result }
$sf = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($pdf)) ([Windows.Storage.StorageFile])
$stream = Await ($sf.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
$doc = Await ([Windows.Data.Pdf.PdfDocument]::LoadFromStreamAsync($stream)) ([Windows.Data.Pdf.PdfDocument])
$actTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and -not $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } | Select-Object -First 1)
$pi = 11
$page = $doc.GetPage($pi)
$opts = [Windows.Data.Pdf.PdfPageRenderOptions]::new()
$opts.DestinationWidth = [uint32]($page.Size.Width * 1.6)
$opts.DestinationHeight = [uint32]($page.Size.Height * 1.6)
$ms = [Windows.Storage.Streams.InMemoryRandomAccessStream]::new()
$t = $actTask.Invoke($null, @($page.RenderToStreamAsync($ms, $opts))); $t.Wait() | Out-Null
$ms.Seek(0)
$reader = [Windows.Storage.Streams.DataReader]::new($ms.GetInputStreamAt(0))
$b = New-Object byte[] ([int]$ms.Size)
$loadTask = $asTaskGeneric.MakeGenericMethod([uint32]).Invoke($null, @($reader.LoadAsync([uint32]$ms.Size))); $loadTask.Wait() | Out-Null
$reader.ReadBytes($b)
$png = 'D:\Projects\Project_EdgeParking-CloudSync\j22_full.png'
[System.IO.File]::WriteAllBytes($png, $b)
$reader.Dispose()

Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($png)
$w = $bmp.Width; $h = $bmp.Height
# J22 is on the right side of the CAN section, lower-middle of page 11
$rx=[int]($w*0.44); $ry=[int]($h*0.60); $rw=[int]($w*0.26); $rh=[int]($h*0.16)
$rect = New-Object System.Drawing.Rectangle($rx,$ry,$rw,$rh)
$crop = $bmp.Clone($rect, $bmp.PixelFormat)
$crop.Save('D:\Projects\Project_EdgeParking-CloudSync\j22_crop.png', [System.Drawing.Imaging.ImageFormat]::Png)
'page11 size ' + $w + 'x' + $h + ' ; saved j22_crop.png ' + $crop.Width + 'x' + $crop.Height
$bmp.Dispose(); $crop.Dispose()

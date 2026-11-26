param([string]$Destination,[string]$PreviewDirectory = '')
Add-Type -AssemblyName System.Drawing
function Save-Tga($Bitmap,[string]$Path) {
 $rect = [Drawing.Rectangle]::new(0,0,$Bitmap.Width,$Bitmap.Height)
 $lock = $Bitmap.LockBits($rect,[Drawing.Imaging.ImageLockMode]::ReadOnly,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
 try {
  $pixels = [byte[]]::new($Bitmap.Width*$Bitmap.Height*4)
  [Runtime.InteropServices.Marshal]::Copy($lock.Scan0,$pixels,0,$pixels.Length)
  $header = [byte[]]::new(18);$header[2]=2;$header[12]=$Bitmap.Width-band255;$header[13]=$Bitmap.Width-shr8;$header[14]=$Bitmap.Height-band255;$header[15]=$Bitmap.Height-shr8;$header[16]=32;$header[17]=40
  $stream=[IO.File]::Create($Path);try {$stream.Write($header,0,18);$stream.Write($pixels,0,$pixels.Length)}finally{$stream.Dispose()}
 } finally {$Bitmap.UnlockBits($lock)}
}
function Canvas([int]$Width,[int]$Height) {
 $b=[Drawing.Bitmap]::new($Width,$Height,[Drawing.Imaging.PixelFormat]::Format32bppArgb);$g=[Drawing.Graphics]::FromImage($b);$g.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias;$g.Clear([Drawing.Color]::Transparent);return @($b,$g)
}
$red=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,225,40,46));$shell=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,160,164,169));$dark=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,29,30,34));$ink=[Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255,235,237,240));$edge=[Drawing.Pen]::new([Drawing.Color]::FromArgb(255,88,91,98),3)
$font=[Drawing.Font]::new('Segoe UI',15,[Drawing.FontStyle]::Bold);$small=[Drawing.Font]::new('Segoe UI',10,[Drawing.FontStyle]::Bold)
$canvas=Canvas 640 340;$bitmap=$canvas[0];$graphics=$canvas[1]
$graphics.FillEllipse($shell,18,66,254,252);$graphics.FillEllipse($shell,368,66,254,252);$graphics.FillRectangle($shell,141,68,360,153);$graphics.FillEllipse($shell,140,37,360,130)
$graphics.DrawEllipse($edge,18,66,254,252);$graphics.DrawEllipse($edge,368,66,254,252)
$graphics.FillRectangle($dark,206,72,228,93);$graphics.DrawString('VIRTUAL BOY',$font,$red,230,103)
foreach($point in @(@(94,89),@(432,89))) {
 $x=$point[0];$y=$point[1];$graphics.FillEllipse($dark,$x-20,$y-14,133,133);$graphics.FillRectangle($ink,$x+25,$y,37,104);$graphics.FillRectangle($ink,$x-9,$y+34,105,36)
 $graphics.FillRectangle($dark,$x+34,$y+7,19,90);$graphics.FillRectangle($dark,$x-2,$y+43,91,18)
}
$graphics.FillEllipse($red,456,230,51,51);$graphics.FillEllipse($red,519,199,51,51);$graphics.DrawString('B',$font,$ink,471,239);$graphics.DrawString('A',$font,$ink,534,207)
$graphics.FillRectangle($dark,91,237,62,23);$graphics.FillRectangle($dark,163,237,62,23);$graphics.DrawString('SELECT',$small,$ink,95,240);$graphics.DrawString('START',$small,$ink,170,240)
$graphics.FillRectangle($dark,67,34,100,21);$graphics.FillRectangle($dark,472,34,100,21);$graphics.DrawString('L',$small,$ink,111,35);$graphics.DrawString('R',$small,$ink,516,35)
if ($PreviewDirectory) {$bitmap.Save((Join-Path $PreviewDirectory 'pad-vb-preview.png'),[Drawing.Imaging.ImageFormat]::Png)}
Save-Tga $bitmap (Join-Path $Destination 'pad_vb.tga');$graphics.Dispose();$bitmap.Dispose()
$canvas=Canvas 80 60;$bitmap=$canvas[0];$graphics=$canvas[1];$pen=[Drawing.Pen]::new($red.Color,5)
$graphics.FillRectangle($dark,7,13,66,35);$graphics.DrawRectangle($pen,8,14,27,33);$graphics.DrawRectangle($pen,45,14,27,33);$graphics.DrawLine($pen,34,30,46,30);$graphics.DrawLine($pen,27,8,53,8)
if ($PreviewDirectory) {$bitmap.Save((Join-Path $PreviewDirectory 'brand-vb-preview.png'),[Drawing.Imaging.ImageFormat]::Png)}
Save-Tga $bitmap (Join-Path $Destination 'brand_vb.tga');$graphics.Dispose();$bitmap.Dispose()

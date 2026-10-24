$ErrorActionPreference = 'Stop'
if ($PSEdition -eq 'Core') {
    & "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath
    if ($LASTEXITCODE -ne 0) { throw 'Icon generation failed.' }
    return
}
$taskAssets = Join-Path $PSScriptRoot '../PID_Workbench/assets'
New-Item -ItemType Directory -Path $taskAssets -Force | Out-Null
# Deterministic rendering of assets/workbench.svg; no external design runtime.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
public static class WorkbenchIconAsset {
    static GraphicsPath Rounded(float x,float y,float width,float height,float r) {
        var p=new GraphicsPath();float d=r*2;
        p.AddArc(x,y,d,d,180,90);p.AddArc(x+width-d,y,d,d,270,90);
        p.AddArc(x+width-d,y+height-d,d,d,0,90);p.AddArc(x,y+height-d,d,d,90,90);p.CloseFigure();return p;
    }
    static Bitmap Render(int size) {
        using(var large=new Bitmap(size*4,size*4,PixelFormat.Format32bppArgb)) {
            using(var g=Graphics.FromImage(large)) {
                g.SmoothingMode=SmoothingMode.AntiAlias;g.ScaleTransform(size*4/256f,size*4/256f);
                using(var bg=Rounded(8,8,240,240,55))
                using(var brush=new LinearGradientBrush(new Point(16,16),new Point(240,240),Color.FromArgb(29,52,72),Color.FromArgb(11,19,32)))
                using(var border=new Pen(Color.FromArgb(54,82,101),2)) {g.FillPath(brush,bg);g.DrawPath(border,bg);}
                using(var pin=new Pen(Color.FromArgb(71,216,196),10)) {
                    pin.StartCap=pin.EndCap=LineCap.Round;
                    foreach(float v in new float[]{96,128,160}) {
                        g.DrawLine(pin,v,46,v,64);g.DrawLine(pin,v,192,v,210);
                        g.DrawLine(pin,46,v,64,v);g.DrawLine(pin,192,v,210,v);
                    }
                }
                using(var chip=Rounded(65,65,126,126,25))
                using(var fill=new SolidBrush(Color.FromArgb(18,42,55)))
                using(var edgeBrush=new LinearGradientBrush(new Point(65,65),new Point(191,191),Color.FromArgb(103,240,212),Color.FromArgb(25,186,186)))
                using(var edge=new Pen(edgeBrush,10)) {g.FillPath(fill,chip);g.DrawPath(edge,chip);}
                using(var wave=new Pen(Color.FromArgb(239,255,254),11)) {
                    wave.StartCap=wave.EndCap=LineCap.Round;wave.LineJoin=LineJoin.Round;
                    g.DrawLines(wave,new PointF[]{new PointF(85,143),new PointF(106,143),new PointF(121,96),new PointF(142,160),new PointF(156,128),new PointF(173,128)});
                }
            }
            var small=new Bitmap(size,size,PixelFormat.Format32bppArgb);
            using(var g=Graphics.FromImage(small)) {
                g.CompositingMode=CompositingMode.SourceCopy;g.InterpolationMode=InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode=PixelOffsetMode.HighQuality;g.DrawImage(large,new Rectangle(0,0,size,size),0,0,large.Width,large.Height,GraphicsUnit.Pixel);
            }
            return small;
        }
    }
    public static void Write(string directory) {
        int[] sizes={16,20,24,32,40,48,64,128,256};var data=new byte[sizes.Length][];
        for(int i=0;i<sizes.Length;i++)using(var bmp=Render(sizes[i]))using(var stream=new MemoryStream()) {
            bmp.Save(stream,ImageFormat.Png);data[i]=stream.ToArray();
            if(sizes[i]==256)bmp.Save(Path.Combine(directory,"workbench-icon.png"),ImageFormat.Png);
        }
        using(var w=new BinaryWriter(File.Create(Path.Combine(directory,"workbench.ico")))) {
            w.Write((ushort)0);w.Write((ushort)1);w.Write((ushort)sizes.Length);int offset=6+16*sizes.Length;
            for(int i=0;i<sizes.Length;i++) {
                w.Write((byte)(sizes[i]==256?0:sizes[i]));w.Write((byte)(sizes[i]==256?0:sizes[i]));w.Write((byte)0);w.Write((byte)0);
                w.Write((ushort)1);w.Write((ushort)32);w.Write(data[i].Length);w.Write(offset);offset+=data[i].Length;
            }
            foreach(var bytes in data)w.Write(bytes);
        }
    }
}
'@
[WorkbenchIconAsset]::Write([IO.Path]::GetFullPath($taskAssets))
Write-Output "Created 16-256px icon in $taskAssets"

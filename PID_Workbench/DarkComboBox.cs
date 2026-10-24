using System;
using System.Drawing;
using System.Windows.Forms;
namespace PidWorkbench
{
    // Native keyboard/dropdown behavior, with a dark selection face and a
    // WM_PRINT path so the application's own UI snapshots render real choices.
    internal sealed class DarkComboBox:ComboBox
    {
        protected override void OnFontChanged(EventArgs e)
        {
            base.OnFontChanged(e);ItemHeight=TextRenderer.MeasureText("Ag",Font).Height+6;
        }
        private void Face(Graphics graphics)
        {
            if(Width<4||Height<4)return;
            using(var background=new SolidBrush(MainForm.Field))graphics.FillRectangle(background,ClientRectangle);
            using(var border=new Pen(Focused?MainForm.Accent:Color.FromArgb(62,80,100)))graphics.DrawRectangle(border,0,0,Width-1,Height-1);
            var rect=new Rectangle(7,1,Math.Max(1,Width-34),Height-2);
            TextRenderer.DrawText(graphics,Text,Font,rect,Enabled?MainForm.Ink:MainForm.Muted,TextFormatFlags.Left|TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis|TextFormatFlags.NoPrefix);
            int x=Width-17,y=Height/2;
            using(var brush=new SolidBrush(MainForm.Muted))graphics.FillPolygon(brush,new[]{new Point(x-4,y-2),new Point(x+4,y-2),new Point(x,y+3)});
        }
        protected override void WndProc(ref Message message)
        {
            int kind=message.Msg;base.WndProc(ref message);
            if(kind==0x000F&&IsHandleCreated){using(var graphics=Graphics.FromHwnd(Handle))Face(graphics);}
            else if((kind==0x0317||kind==0x0318)&&message.WParam!=IntPtr.Zero){using(var graphics=Graphics.FromHdc(message.WParam))Face(graphics);}
        }
    }
}

using System;
using System.Drawing;
using System.Windows.Forms;
namespace PidWorkbench
{
    internal sealed class FlatProgressBar:Control
    {
        private int value,maximum=1000;
        internal int Maximum {get{return maximum;}set{maximum=Math.Max(1,value);Invalidate();}}
        internal int Value {get{return value;}set{this.value=Math.Max(0,Math.Min(maximum,value));Invalidate();}}
        internal FlatProgressBar(){SetStyle(ControlStyles.UserPaint|ControlStyles.AllPaintingInWmPaint|ControlStyles.OptimizedDoubleBuffer,true);}
        protected override void OnPaint(PaintEventArgs e)
        {
            using(var brush=new SolidBrush(MainForm.Field))e.Graphics.FillRectangle(brush,ClientRectangle);
            using(var brush=new SolidBrush(MainForm.Accent))e.Graphics.FillRectangle(brush,0,0,(int)((long)Width*value/maximum),Height);
        }
    }
}

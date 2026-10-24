using System;
using System.Runtime.InteropServices;
using System.Windows.Forms;
namespace PidWorkbench
{
    internal static class DpiSupport
    {
        [DllImport("user32.dll")] internal static extern uint GetDpiForWindow(IntPtr window);
        [DllImport("user32.dll")] internal static extern IntPtr GetWindowDpiAwarenessContext(IntPtr window);
        [DllImport("user32.dll")] internal static extern bool AreDpiAwarenessContextsEqual(IntPtr a,IntPtr b);
        [DllImport("user32.dll")] private static extern IntPtr SendMessage(IntPtr window,uint message,IntPtr wParam,IntPtr lParam);
        [StructLayout(LayoutKind.Sequential)] private struct Rect {public int Left,Top,Right,Bottom;}
        internal static void MatchInitialMonitor(Form form)
        {
            uint native;try{native=GetDpiForWindow(form.Handle);}catch(EntryPointNotFoundException){return;}
            int dpi=(int)native;if(dpi<=0||dpi==form.DeviceDpi)return;
            double scale=(double)dpi/form.DeviceDpi;
            var bounds=new Rect {Left=form.Left,Top=form.Top,Right=form.Left+(int)Math.Round(form.Width*scale),Bottom=form.Top+(int)Math.Round(form.Height*scale)};
            IntPtr memory=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Rect)));
            try{Marshal.StructureToPtr(bounds,memory,false);SendMessage(form.Handle,0x02E0,new IntPtr(dpi|(dpi<<16)),memory);}finally{Marshal.FreeHGlobal(memory);}
        }
        internal static void SimulateForTest(Form form,int dpi)
        {
            double scale=(double)dpi/form.DeviceDpi;
            var bounds=new Rect {Left=form.Left,Top=form.Top,Right=form.Left+(int)Math.Round(form.Width*scale),Bottom=form.Top+(int)Math.Round(form.Height*scale)};
            IntPtr memory=Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Rect)));
            try{Marshal.StructureToPtr(bounds,memory,false);SendMessage(form.Handle,0x02E0,new IntPtr(dpi|(dpi<<16)),memory);}finally{Marshal.FreeHGlobal(memory);}
        }
    }
}

using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PidWorkbench
{
    internal static class SerialLiveValidation
    {
        internal static int Run(string directory)
        {
            Directory.CreateDirectory(directory);int code=1;
            using(var form=new MainForm()) {
                form.Opacity=0;form.ShowInTaskbar=false;
                form.Shown+=async delegate {
                    try {
                        DateTime end=DateTime.UtcNow.AddSeconds(20);
                        while(!form.Ready&&form.StartupError==null&&DateTime.UtcNow<end)await Task.Delay(50);
                        if(!form.Ready)throw new Exception("Startup failed: "+form.StartupError);
                        form.ShowPage("serial");await form.Terminal.StartWhenAvailable();
                        if(!form.Terminal.IsReceiving)throw new Exception("Receiver did not start");
                        await Task.Delay(3000);
                        string text=form.Terminal.DisplayedText;
                        File.WriteAllText(Path.Combine(directory,"actual_gui_log.txt"),text,new UTF8Encoding(false));
                        using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(Point.Empty,form.Size));bitmap.Save(Path.Combine(directory,"actual_gui.png"),ImageFormat.Png);}
                        if(!text.Contains("T=50,50 actual=")||!text.Contains("PWM=0,0"))throw new Exception("No actual F407 log line in GUI");
                        await form.Terminal.Stop();
                        // Also close the window with reception active, exercising
                        // the asynchronous close/release path without blocking reads.
                        await form.Terminal.StartWhenAvailable();await Task.Delay(250);code=0;
                    }catch(Exception error){File.WriteAllText(Path.Combine(directory,"error.txt"),error.ToString());}
                    finally{form.Close();}
                };
                // Use the real desktop message loop for UI continuations/timers.
                Application.Run(form);
                if(code==0&&form.Terminal.IsReceiving){code=1;File.WriteAllText(Path.Combine(directory,"error.txt"),"Receiver still active after window closed");}
            }
            if(code==0)File.WriteAllText(Path.Combine(directory,"result.json"),"{\"passed\":true,\"manual_stop\":true,\"window_close_releases_receiver\":true,\"source\":\"physical STM32 UART via slave H1/ESP-NOW/host USB/API\"}");
            return code;
        }
    }
}

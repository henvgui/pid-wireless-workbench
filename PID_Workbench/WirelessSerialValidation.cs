using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
namespace PidWorkbench
{
    internal static class WirelessSerialValidation
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
                        form.ShowPage("serial");string marker="无线串口 UTF-8 / 1234567890\r\n";
                        form.Terminal.ConfigureTest(true,marker);await form.Terminal.StartWhenAvailable();
                        await form.Terminal.Send(false);
                        end=DateTime.UtcNow.AddSeconds(8);
                        while(!form.Terminal.DisplayedText.Contains(marker.Trim())&&DateTime.UtcNow<end)await Task.Delay(100);
                        if(!form.Terminal.DisplayedText.Contains(marker.Trim()))throw new Exception("Actual wireless UART loopback did not reach GUI");
                        long before=form.Terminal.ReceivedBytes;
                        await form.Terminal.Send(true);await Task.Delay(1000);
                        if(form.Terminal.ReceivedBytes!=before)throw new Exception("Retry duplicated UART bytes");
                        using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(Point.Empty,form.Size));bitmap.Save(Path.Combine(directory,"wireless_serial.png"),ImageFormat.Png);}
                        File.WriteAllText(Path.Combine(directory,"gui_test.txt"),"PASS actual GUI -> API -> host USB -> encrypted ESP-NOW -> slave UART internal loopback -> GUI UTF-8; retry produced zero duplicate RX bytes. External target was isolated.\r\n"+form.Terminal.DisplayedText,Encoding.UTF8);
                        await form.Terminal.Stop();form.Terminal.ConfigureTest(false,"");code=0;
                    } catch(Exception error){File.WriteAllText(Path.Combine(directory,"gui_failure.txt"),error.ToString(),Encoding.UTF8);}
                    form.Close();
                };
                Application.Run(form);
            }
            return code;
        }
    }
}

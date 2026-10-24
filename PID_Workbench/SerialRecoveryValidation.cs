using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PidWorkbench
{
    internal static class SerialRecoveryValidation
    {
        internal static int Run(string directory)
        {
            Directory.CreateDirectory(directory);int code=1;string report="";
            using(var form=new MainForm()) {
                form.Opacity=0;form.ShowInTaskbar=false;
                form.Shown+=async delegate {
                    try {
                        DateTime until=DateTime.UtcNow.AddSeconds(20);
                        while(!form.Ready&&form.StartupError==null&&DateTime.UtcNow<until)await Task.Delay(50);
                        if(!form.Ready)throw new Exception("Startup failed: "+form.StartupError);
                        form.ShowPage("serial");await form.Terminal.StartWhenAvailable();await Task.Delay(2200);
                        string before=form.Terminal.DisplayedText;
                        if(!form.Terminal.IsReceiving)throw new Exception("Receiver did not start");
                        bool uartBefore=before.Contains("T=")&&before.Contains("PWM=");
                        string prior=form.Terminal.SessionId;long bytes=form.Terminal.ReceivedBytes;
                        // Inject the exact typed error raised by a lost slave UART
                        // session. UART/radio/USB data before and after are real.
                        form.Terminal.Recover(new BackendFailure("serial_remote_error","test: lost slave log session","wrong_transfer"));
                        await Task.Delay(3500);
                        string after=form.Terminal.DisplayedText;
                        int marker=after.IndexOf("[日志会话已重建");
                        if(form.Terminal.SessionId==prior||marker<0)
                            throw new Exception("Automatic session rebuild failed");
                        bool uartAfter=after.Substring(marker).Contains("T=")&&form.Terminal.ReceivedBytes>bytes;
                        if(before.Length>0&&!after.Contains(before.Substring(0,Math.Min(60,before.Length))))throw new Exception("Recovery cleared existing displayed log");
                        File.WriteAllText(Path.Combine(directory,"actual_uart_gui.txt"),after,new UTF8Encoding(false));
                        using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(Point.Empty,form.Size));bitmap.Save(Path.Combine(directory,"recovered_gui.png"),ImageFormat.Png);}
                        form.Terminal.Recover(new BackendFailure("serial_remote_error","test: queued retry","wrong_transfer"));
                        await form.Terminal.Stop();await Task.Delay(1100);
                        if(form.Terminal.IsReceiving)throw new Exception("User STOP restarted reception");
                        var api=new BackendClient();await api.Start();var status=Json.Object(Json.Value(await api.Call("/v1/serial/status"),"data"));
                        if(!Json.Bool(status,"closed"))throw new Exception("Remote UART remained open after STOP");
                        report=Json.Encode(new{session_recovery_passed=true,end_to_end_uart_passed=uartBefore&&uartAfter,actual_bytes=form.Terminal.ReceivedBytes,
                            simulated_error="wrong_transfer",actual_uart_resumed=uartAfter,previous_log_retained=before.Length>0?(bool?)true:null,
                            stop_cancels_retry=true,physical_board_reset=false});code=0;
                    }catch(Exception error){
                        File.WriteAllText(Path.Combine(directory,"error.txt"),error.ToString());
                        File.WriteAllText(Path.Combine(directory,"failed_gui_log.txt"),form.Terminal.DisplayedText,new UTF8Encoding(false));
                        File.WriteAllText(Path.Combine(directory,"failed_state.json"),Json.Encode(new{receiving=form.Terminal.IsReceiving,bytes=form.Terminal.ReceivedBytes,session=form.Terminal.SessionId}));
                    }
                    finally{form.Close();}
                };
                Application.Run(form);
            }
            if(code==0)File.WriteAllText(Path.Combine(directory,"result.json"),report,Encoding.UTF8);
            return code;
        }
    }
}

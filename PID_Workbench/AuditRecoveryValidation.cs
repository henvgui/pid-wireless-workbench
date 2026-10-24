using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
namespace PidWorkbench
{
    internal static class AuditRecoveryValidation
    {
        private static async Task KillIdleBackend()
        {
            await Task.Run(delegate {
                var info=new ProcessStartInfo(BackendClient.Python,BackendClient.Quote(Path.Combine(BackendClient.AppDirectory,"validation/project_audit_hardware_20261024.py"))+" stop_backend") {UseShellExecute=false,CreateNoWindow=true};
                using(var process=Process.Start(info)){process.WaitForExit();if(process.ExitCode!=0)throw new Exception("Could not stop checked idle backend");}
            });
        }
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
                        form.ShowPage("serial");form.Terminal.ConfigureTest(true,"before service outage\r\n");
                        await form.Terminal.StartWhenAvailable();await form.Terminal.Send(false);
                        end=DateTime.UtcNow.AddSeconds(6);
                        while(form.Terminal.ReceivedBytes==0&&DateTime.UtcNow<end)await Task.Delay(50);
                        if(form.Terminal.ReceivedBytes==0)throw new Exception("Initial loopback missing");
                        string previous=form.Terminal.SessionId;long priorBytes=form.Terminal.ReceivedBytes;
                        await KillIdleBackend();end=DateTime.UtcNow.AddSeconds(20);
                        while(form.Terminal.SessionId==previous&&DateTime.UtcNow<end)await Task.Delay(100);
                        if(form.Terminal.SessionId==previous||!form.Terminal.IsReceiving)throw new Exception("Service did not restart and rebuild receiver");
                        if(!form.Terminal.DisplayedText.Contains("before service outage"))throw new Exception("Recovery erased retained log");
                        form.Terminal.ConfigureTest(true,"after service outage\r\n");
                        await form.Terminal.Send(false);end=DateTime.UtcNow.AddSeconds(6);
                        while(form.Terminal.ReceivedBytes==priorBytes&&DateTime.UtcNow<end)await Task.Delay(50);
                        if(form.Terminal.ReceivedBytes==priorBytes||!form.Terminal.DisplayedText.Contains("after service outage"))throw new Exception("UART did not resume after service restart");
                        await KillIdleBackend();await form.Terminal.Stop();await Task.Delay(1500);
                        if(form.Terminal.IsReceiving)throw new Exception("User STOP reopened reception");
                        var client=new BackendClient();await client.Start();var status=Json.Object(Json.Value(await client.Call("/v1/serial/status"),"data"));
                        if(!Json.Bool(status,"closed"))throw new Exception("Disconnected STOP opened a new remote receiver");
                        File.WriteAllText(Path.Combine(directory,"recovery_test.txt"),"PASS real backend termination/restart, new receiver session, retained log, resumed wireless UART, and explicit STOP while disconnected. No STM32 erase/RUN.\r\n"+form.Terminal.DisplayedText,Encoding.UTF8);
                        form.Terminal.ConfigureTest(false,"");code=0;
                    }catch(Exception error){File.WriteAllText(Path.Combine(directory,"recovery_failure.txt"),error.ToString(),Encoding.UTF8);}
                    form.Close();
                };
                Application.Run(form);
            }
            return code;
        }
    }
}

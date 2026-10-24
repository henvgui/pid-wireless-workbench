using System;
using System.Threading;
using System.Windows.Forms;

[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8")]
namespace PidWorkbench
{
    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            if(args.Length>0&&args[0]=="--ui-test")return UiValidation.Run(args.Length>1?args[1]:"validation");
            if(args.Length>0&&args[0]=="--serial-live-test")return SerialLiveValidation.Run(args[1]);
            if(args.Length>0&&args[0]=="--serial-recovery-test")return SerialRecoveryValidation.Run(args[1]);
            if(args.Length>0&&args[0]=="--wireless-serial-test")return WirelessSerialValidation.Run(args[1]);
            if(args.Length>0&&args[0]=="--audit-recovery-test")return AuditRecoveryValidation.Run(args[1]);
            bool created;
            using(var mutex=new Mutex(true,@"Local\PID_Workbench_UI",out created)) {
                if(!created)return 0;
                Application.ThreadException+=delegate(object sender,ThreadExceptionEventArgs e){MessageBox.Show(e.Exception.Message,"PID 工作台",MessageBoxButtons.OK,MessageBoxIcon.Error);};
                var form=new MainForm();
                if(Array.IndexOf(args,"--serial-log")>=0)form.Shown+=async delegate {
                    form.ShowPage("serial");
                    try{await form.Terminal.StartWhenAvailable();}catch(Exception error){MessageBox.Show(error.Message,"串口日志",MessageBoxButtons.OK,MessageBoxIcon.Error);}
                };
                Application.Run(form);
            }
            return 0;
        }
    }
}

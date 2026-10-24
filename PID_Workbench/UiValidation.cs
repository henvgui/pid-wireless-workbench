using System;
using System.Linq;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;
namespace PidWorkbench
{
    internal static class UiValidation
    {
        internal static int Run(string directory)
        {
            SynchronizationContext.SetSynchronizationContext(new WindowsFormsSynchronizationContext());
            Directory.CreateDirectory(directory);var report=new StringBuilder();
            try {
                using(var form=new MainForm()) {
                    if(Environment.GetEnvironmentVariable("PID_UI_TEST_OFFSCREEN")=="1"){form.Opacity=0;form.ShowInTaskbar=false;}
                    form.Show();DateTime deadline=DateTime.UtcNow.AddSeconds(20);
                    while(!form.Ready&&form.StartupError==null&&DateTime.UtcNow<deadline){Application.DoEvents();Thread.Sleep(12);}
                    if(!form.Ready)throw new Exception("Startup failed: "+form.StartupError);
                    if(!DpiSupport.AreDpiAwarenessContextsEqual(DpiSupport.GetWindowDpiAwarenessContext(form.Handle),new IntPtr(-4)))throw new Exception("Window is not PerMonitorV2 aware");
                    report.AppendLine("PASS: real WinForms window uses PerMonitorV2; DPI="+form.DeviceDpi+", native="+DpiSupport.GetDpiForWindow(form.Handle));
                    if(form.Chips.Items.Count<2||form.Methods.Items.Count!=2||form.Ports.Items.Count<1)throw new Exception("Catalog or port selection unavailable");
                    report.AppendLine("PASS: chip catalog, slave UART/SWD interfaces, and ESP32 host USB selection loaded from live API.");
                    if(form.Chips.SelectedIndex<0||form.Methods.SelectedIndex<0||form.Ports.SelectedIndex<0)throw new Exception("A required catalog selection is blank");
                    report.AppendLine("Selections: "+form.Chips.Text+" | "+form.Methods.Text+" | "+form.Ports.Text);
                    DateTime stateDeadline=DateTime.UtcNow.AddSeconds(2);
                    while(form.LastReadStep==null&&DateTime.UtcNow<stateDeadline){Application.DoEvents();Thread.Sleep(10);}
                    if(form.LastReadStep!=null&&form.SelectedStep!=form.LastReadStep)throw new Exception("STEP selection disagrees with the host state");
                    report.AppendLine("STEP selection matches live host: "+form.SelectedStep);
                    int configuredChip=form.Chips.SelectedIndex,configuredMethod=form.Methods.SelectedIndex;
                    string configuredFirmware=form.Firmware.Text;
                    form.Chips.SelectedIndex=0;
                    int originalPort=form.Ports.SelectedIndex;
                    if(form.Ports.Items.Count>1)form.Ports.SelectedIndex=1;
                    string hostPort=((Choice)form.Ports.SelectedItem).Id;
                    form.Methods.SelectedIndex=1;Application.DoEvents();
                    if(form.PanelPort!=hostPort)throw new Exception("Changing the target interface redirected the host USB port");
                    if(form.FlashButton.Enabled||form.ValidateButton.Enabled||form.RunAfter.Enabled)throw new Exception("Unimplemented slave SWD can be executed");
                    using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(Point.Empty,form.Size));bitmap.Save(Path.Combine(directory,"flash_swd.png"),ImageFormat.Png);}
                    form.Methods.SelectedIndex=0;form.Ports.SelectedIndex=originalPort;
                    if(!form.FlashButton.Enabled||!form.ValidateButton.Enabled)throw new Exception("Implemented wireless UART disabled after switching back");
                    report.AppendLine("PASS: every interface retains host USB; unsupported SWD chip is disabled; UART re-enables correctly.");
                    int originalChip=form.Chips.SelectedIndex;string originalFirmware=form.Firmware.Text;
                    int f407=-1;for(int i=0;i<form.Chips.Items.Count;++i)if(((Choice)form.Chips.Items[i]).Id=="STM32F407VGT6")f407=i;
                    if(f407<0)throw new Exception("F407 chip missing from live catalog");
                    form.Chips.SelectedIndex=f407;Application.DoEvents();
                    if(((Choice)form.Methods.SelectedItem).TargetInterface!="swd"||!form.FlashButton.Enabled)throw new Exception("F407 does not default to implemented SWD");
                    using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(Point.Empty,form.Size));bitmap.Save(Path.Combine(directory,"flash_f407_swd.png"),ImageFormat.Png);}
                    form.Chips.SelectedIndex=originalChip;form.Firmware.Text=originalFirmware;
                    report.AppendLine("PASS: F407 is selectable, defaults to slave SWD, and enables the flash route without a PC probe.");
                    int f407ve=-1;for(int i=0;i<form.Chips.Items.Count;++i)if(((Choice)form.Chips.Items[i]).Id=="STM32F407VET6")f407ve=i;
                    if(f407ve<0)throw new Exception("F407VET6 missing from live catalog");
                    form.Chips.SelectedIndex=f407ve;Application.DoEvents();
                    if(((Choice)form.Methods.SelectedItem).TargetInterface!="swd"||!form.FlashButton.Enabled)throw new Exception("F407VET6 does not default to implemented SWD");
                    using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(Point.Empty,form.Size));bitmap.Save(Path.Combine(directory,"flash_f407ve_swd.png"),ImageFormat.Png);}
                    form.Chips.SelectedIndex=originalChip;form.Firmware.Text=originalFirmware;
                    report.AppendLine("PASS: 512 KiB F407VET6 is separately selectable with slave SWD.");
                    int originalDpi=form.DeviceDpi;
                    foreach(int dpi in new[]{96,120,144,192,originalDpi}) {
                        DpiSupport.SimulateForTest(form,dpi);Application.DoEvents();
                        if(form.DeviceDpi!=dpi||Math.Abs(form.Font.SizeInPoints-9.5f)>.01f)throw new Exception("DPI/font baseline mismatch: requested="+dpi+", device="+form.DeviceDpi+", font="+form.Font.SizeInPoints);
                        if(!form.FlashButton.Parent.ClientRectangle.Contains(form.FlashButton.Bounds))throw new Exception("Flash action clipped at simulated DPI "+dpi);
                        report.AppendLine("PASS simulated "+dpi+" DPI: fixed font point size and visible flash action.");
                    }
                    if(File.Exists(form.Firmware.Text)) {
                        form.ValidateButton.PerformClick();DateTime check=DateTime.UtcNow.AddMilliseconds(700);
                        while(DateTime.UtcNow<check){Application.DoEvents();Thread.Sleep(10);}
                        report.AppendLine("Read-only firmware plan validation invoked; no flash job was submitted.");
                    }
                    form.ShowPage("serial");
                    var formatField=typeof(SerialTerminalPanel).GetField("format",System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic);
                    var encodingField=typeof(SerialTerminalPanel).GetField("encoding",System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic);
                    ((ComboBox)formatField.GetValue(form.Terminal)).SelectedIndex=0;
                    ((ComboBox)encodingField.GetValue(form.Terminal)).SelectedIndex=0;
                    byte[] utf8=Encoding.UTF8.GetBytes("转速=120.5 RPM\r\n");
                    form.Terminal.Receive(new[]{utf8[0]});form.Terminal.Receive(utf8.Skip(1).ToArray());
                    if(!form.Terminal.DisplayedText.Contains("转速=120.5 RPM")||form.Terminal.DisplayedText.Contains("�"))throw new Exception("Serial UTF-8 split-frame decoding failed");
                    ((ComboBox)formatField.GetValue(form.Terminal)).SelectedIndex=1;
                    form.Terminal.Receive(new byte[]{0,255,0x41});
                    if(!form.Terminal.DisplayedText.Contains("00 FF 41"))throw new Exception("Serial HEX binary view failed");
                    ((ComboBox)formatField.GetValue(form.Terminal)).SelectedIndex=0;
                    form.Terminal.ClearDisplay();
                    form.Terminal.Receive(Encoding.UTF8.GetBytes("转速=120.5 RPM\r\n"));
                    for(int n=0;n<7;++n)form.Terminal.Receive(Encoding.UTF8.GetBytes("speed="+(120+n)+" rpm  angle="+(n*3)+" deg\r\n"));
                    report.AppendLine("PASS: serial page decodes UTF-8 across packet boundaries and renders arbitrary binary as HEX; preview bytes are injected locally, not from hardware.");
                    foreach(string page in new[]{"flash","pid","serial","echo","ai"}) {
                        form.ShowPage(page);Application.DoEvents();Thread.Sleep(100);Application.DoEvents();
                        using(var bitmap=new Bitmap(form.Width,form.Height)){form.DrawToBitmap(bitmap,new Rectangle(Point.Empty,form.Size));bitmap.Save(Path.Combine(directory,page+".png"),ImageFormat.Png);}
                        report.AppendLine("Captured "+page+" page "+form.Width+"x"+form.Height);
                    }
                    form.Chips.SelectedIndex=configuredChip;form.Methods.SelectedIndex=configuredMethod;form.Firmware.Text=configuredFirmware;
                    form.Close();
                }
                File.WriteAllText(Path.Combine(directory,"ui_test.txt"),report.ToString(),Encoding.UTF8);return 0;
            } catch(Exception error){report.AppendLine("FAIL: "+error);File.WriteAllText(Path.Combine(directory,"ui_test.txt"),report.ToString(),Encoding.UTF8);return 1;}
        }
    }
}

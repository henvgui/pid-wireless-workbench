using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PidWorkbench
{
    internal sealed class Choice
    {
        internal string Id,Label,TargetInterface;
        internal bool Available;
        internal string DefaultInterface;
        internal HashSet<string> Interfaces;
        internal Choice(string id,string label,bool available=true,string targetInterface=null){Id=id;Label=label;Available=available;TargetInterface=targetInterface;}
        public override string ToString(){return Label;}
    }
    internal sealed class MainForm:Form
    {
        internal static readonly Color Background=Color.FromArgb(13,19,29),Surface=Color.FromArgb(22,31,45),Field=Color.FromArgb(15,23,35);
        internal static readonly Color Ink=Color.FromArgb(229,237,248),Muted=Color.FromArgb(146,164,188),Accent=Color.FromArgb(50,211,181);
        private readonly BackendClient api=new BackendClient();
        private readonly Dictionary<string,Panel> pages=new Dictionary<string,Panel>();
        private readonly Dictionary<string,Button> navigation=new Dictionary<string,Button>();
        private readonly List<Control> actions=new List<Control>();
        private readonly Label statusLabel=new Label(),deviceLabel=new Label(),wirelessLabel=new Label(),hint=new Label(),manifest=new Label(),jobTitle=new Label(),phaseLabel=new Label();
        internal readonly ComboBox Chips=new DarkComboBox(),Methods=new DarkComboBox(),Ports=new DarkComboBox();
        internal readonly TextBox Firmware=new TextBox(),EchoText=new TextBox(),EchoFile=new TextBox();
        internal readonly CheckBox RunAfter=new CheckBox();
        internal readonly Button ValidateButton=new Button(),FlashButton=new Button();
        private readonly FlatProgressBar progress=new FlatProgressBar();
        private readonly Dictionary<Control,Font> baselineFonts=new Dictionary<Control,Font>();
        private readonly RichTextBox logs=new RichTextBox(),results=new RichTextBox(),aiHelp=new RichTextBox();
        private readonly NumericUpDown pNumber=new NumericUpDown(),iNumber=new NumericUpDown(),dNumber=new NumericUpDown();
        private readonly ComboBox stepBox=new DarkComboBox(),echoMode=new DarkComboBox();
        private readonly Label panelState=new Label(),echoHint=new Label();
        private readonly System.Windows.Forms.Timer pollTimer=new System.Windows.Forms.Timer();
        private string activeJob,lastJob;
        private bool polling,closing,submitting,devicePolling,actionsEnabled;
        private DateTime lastDevice=DateTime.MinValue;
        private DateTime serviceRetryAfter=DateTime.MinValue;
        internal bool Ready;
        internal string StartupError;
        internal string LastReadStep;
        internal string PanelPort {get{return HostPort();}}
        internal string SelectedStep {get{return stepBox.Text;}}
        private readonly Panel content=new Panel();
        internal SerialTerminalPanel Terminal;
        internal MainForm()
        {
            SuspendLayout();
            Text="PID 调试工作台 · 烧录与 AI 自动化";Font=new Font("Microsoft YaHei UI",9.5f);
            Icon=Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            AutoScaleDimensions=new SizeF(96,96);AutoScaleMode=AutoScaleMode.Dpi;
            BackColor=Background;ForeColor=Ink;ClientSize=new Size(1220,830);MinimumSize=new Size(960,650);StartPosition=FormStartPosition.CenterScreen;
            Build();
            EnableActions(false);
            RememberFonts(this);
            AutoScaleDimensions=new SizeF(96,96);ResumeLayout(false);PerformAutoScale();
            pollTimer.Interval=700;pollTimer.Tick+=async delegate{await Poll();};
            Shown+=async delegate {ClampToScreen();DpiSupport.MatchInitialMonitor(this);RestoreFonts();ClampToScreen();await Startup();};
            FormClosing+=async delegate(object sender,FormClosingEventArgs e){
                if(closing)return;
                SaveSettings();pollTimer.Stop();if(Terminal!=null)Terminal.Shutdown();
                if(Terminal!=null&&Terminal.IsReceiving) {
                    e.Cancel=true;
                    try{await Terminal.Stop();}catch(Exception){/* A flash job keeps running if it owns the hardware gate. */}
                    closing=true;if(!IsDisposed)BeginInvoke(new Action(Close));
                } else closing=true;
            };
            Disposed+=delegate{foreach(var font in baselineFonts.Values)font.Dispose();};
        }
        protected override void WndProc(ref Message message)
        {
            int kind=message.Msg;base.WndProc(ref message);
            // WinForms finishes scaling after raising DpiChanged. Restore point
            // sizes after the whole native message, including child fonts.
            if(kind==0x02E0&&baselineFonts!=null&&baselineFonts.Count>0)RestoreFonts();
        }
        private void RememberFonts(Control parent)
        {
            baselineFonts[parent]=new Font(parent.Font,parent.Font.Style);
            foreach(Control child in parent.Controls)RememberFonts(child);
        }
        private void RestoreFonts(){foreach(var item in baselineFonts)if(!item.Key.IsDisposed)item.Key.Font=item.Value;}
        private void SaveSettings()
        {
            if(!Ready)return;
            try{Directory.CreateDirectory(BackendClient.DataDirectory);File.WriteAllText(Path.Combine(BackendClient.DataDirectory,"ui_settings.json"),Json.Encode(FlashSpec()),System.Text.Encoding.UTF8);}catch(IOException){}
        }
        private void LoadSettings()
        {
            string path=Path.Combine(BackendClient.DataDirectory,"ui_settings.json");if(!File.Exists(path))return;
            try{
                var settings=Json.Decode(File.ReadAllText(path));
                foreach(var pair in new[]{new KeyValuePair<ComboBox,string>(Chips,"chip"),new KeyValuePair<ComboBox,string>(Methods,"method"),new KeyValuePair<ComboBox,string>(Ports,"port")}) {
                    string selection=Json.Text(settings,pair.Value,pair.Value=="method"?"wireless_"+Json.Text(settings,"target_interface","uart"):"");
                    for(int i=0;i<pair.Key.Items.Count;++i)if(((Choice)pair.Key.Items[i]).Id==selection)pair.Key.SelectedIndex=i;
                }
                Firmware.Text=Json.Text(settings,"firmware",Firmware.Text);RunAfter.Checked=Json.Bool(settings,"run");
            }catch(Exception){}
        }
        private void ClampToScreen()
        {
            Rectangle area=Screen.FromHandle(Handle).WorkingArea;
            MinimumSize=new Size(Math.Min(MinimumSize.Width,area.Width-30),Math.Min(MinimumSize.Height,area.Height-30));
            Size=new Size(Math.Min(Width,area.Width-30),Math.Min(Height,area.Height-30));
            Location=new Point(area.Left+Math.Max(0,(area.Width-Width)/2),area.Top+Math.Max(0,(area.Height-Height)/2));
        }
        private static Label Label(string text,Color? color=null,int height=28)
        {
            return new Label {Text=text,ForeColor=color??Ink,AutoSize=false,Dock=DockStyle.Top,Height=height,TextAlign=ContentAlignment.MiddleLeft,Margin=new Padding(0,4,0,4)};
        }
        private Button Button(string text,Func<Task> action,bool primary=false,bool jobAction=false)
        {
            var button=new Button {Text=text,AutoSize=true,Height=36,MinimumSize=new Size(90,36),FlatStyle=FlatStyle.Flat,
                BackColor=primary?Accent:Color.FromArgb(34,47,65),ForeColor=primary?Background:Ink,Margin=new Padding(0,0,8,7),Padding=new Padding(10,4,10,4),Cursor=Cursors.Hand};
            button.FlatAppearance.BorderSize=0;
            button.Click+=async delegate {try{await action();}catch(Exception ex){Notice(ex.Message,true);}};
            if(jobAction)actions.Add(button);
            return button;
        }
        private static void Input(Control control)
        {
            control.BackColor=Field;control.ForeColor=Ink;control.Dock=DockStyle.Top;control.Height=32;control.Margin=new Padding(0,0,0,10);
            var box=control as TextBox;if(box!=null)box.BorderStyle=BorderStyle.FixedSingle;
            var combo=control as ComboBox;if(combo!=null){
                combo.FlatStyle=FlatStyle.Flat;combo.DropDownStyle=ComboBoxStyle.DropDownList;combo.DrawMode=DrawMode.OwnerDrawFixed;
                combo.ItemHeight=TextRenderer.MeasureText("Ag",combo.Font).Height+6;
                combo.DrawItem+=delegate(object sender,DrawItemEventArgs e){
                    using(var brush=new SolidBrush((e.State&DrawItemState.Selected)!=0?Color.FromArgb(28,65,65):Field))e.Graphics.FillRectangle(brush,e.Bounds);
                    string text=e.Index>=0?combo.GetItemText(combo.Items[e.Index]):combo.Text;
                    TextRenderer.DrawText(e.Graphics,text,e.Font,e.Bounds,Ink,TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis|TextFormatFlags.Left);
                };
            }
        }
        private static Panel Card(string title)
        {
            var card=new Panel {Dock=DockStyle.Fill,BackColor=Surface,Padding=new Padding(18),Margin=new Padding(0,0,12,12),AutoScroll=true};
            return card;
        }
        private static TableLayoutPanel Stack(int rows)
        {
            var stack=new TableLayoutPanel {Dock=DockStyle.Top,AutoSize=true,ColumnCount=1,RowCount=rows,BackColor=Color.Transparent,Padding=new Padding(0)};
            stack.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));return stack;
        }
        private static Label AddField(TableLayoutPanel stack,string title,Control control)
        {
            Input(control);var label=Label(title,Muted);stack.Controls.Add(label);stack.Controls.Add(control);return label;
        }
        private static void StyleLog(RichTextBox box)
        {
            box.Dock=DockStyle.Fill;box.BackColor=Field;box.ForeColor=Ink;box.BorderStyle=BorderStyle.None;box.ReadOnly=true;
            box.Font=new Font("Consolas",9.5f);box.WordWrap=true;box.DetectUrls=false;
        }
        private void Build()
        {
            var root=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=2,RowCount=3,Padding=new Padding(20)};
            root.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,172));root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute,68));root.RowStyles.Add(new RowStyle(SizeType.Percent,100));root.RowStyles.Add(new RowStyle(SizeType.Absolute,28));Controls.Add(root);
            var brand=Label("PID\nWORKBENCH",Accent,60);brand.Font=new Font("Segoe UI",16,FontStyle.Bold);root.Controls.Add(brand,0,0);
            var header=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=2,RowCount=2};
            header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,60));header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,40));
            header.RowStyles.Add(new RowStyle(SizeType.Percent,60));header.RowStyles.Add(new RowStyle(SizeType.Percent,40));
            var title=Label("设备调试与固件烧录",Ink,34);title.Dock=DockStyle.Fill;title.Margin=new Padding(0);title.Font=new Font(Font.FontFamily,16,FontStyle.Bold);header.Controls.Add(title,0,0);
            var subtitle=Label("主机 USB → ESP-NOW → 从机 → 目标板",Muted,25);subtitle.Dock=DockStyle.Fill;subtitle.Margin=new Padding(0);header.Controls.Add(subtitle,0,1);
            deviceLabel.Dock=DockStyle.Fill;deviceLabel.TextAlign=ContentAlignment.MiddleRight;deviceLabel.Text="正在启动本地服务…";deviceLabel.ForeColor=Muted;header.Controls.Add(deviceLabel,1,0);
            wirelessLabel.Dock=DockStyle.Fill;wirelessLabel.TextAlign=ContentAlignment.MiddleRight;wirelessLabel.ForeColor=Accent;header.Controls.Add(wirelessLabel,1,1);root.Controls.Add(header,1,0);
            var sidebar=new FlowLayoutPanel {Dock=DockStyle.Fill,FlowDirection=FlowDirection.TopDown,WrapContents=false,Padding=new Padding(0,20,18,0)};root.Controls.Add(sidebar,0,1);
            string[] keys={"flash","pid","serial","echo","ai"},labels={"01   固件烧录","02   PID 参数","03   无线串口","04   文本回传","05   AI 接口"};
            for(int i=0;i<keys.Length;++i){string key=keys[i];var button=Button(labels[i],delegate{ShowPage(key);return Task.FromResult(0);});button.Width=145;button.Height=43;button.AutoSize=false;button.TextAlign=ContentAlignment.MiddleLeft;navigation[key]=button;sidebar.Controls.Add(button);}
            sidebar.Controls.Add(Label("LOCAL / v2026.10\nESP-NOW 硬件工作台",Muted,70));
            content.Dock=DockStyle.Fill;root.Controls.Add(content,1,1);
            statusLabel.Dock=DockStyle.Fill;statusLabel.ForeColor=Muted;statusLabel.Text="服务仅监听本机 127.0.0.1";root.Controls.Add(statusLabel,1,2);
            BuildFlash();BuildPid();BuildEcho();BuildAi();
            Terminal=new SerialTerminalPanel(api,HostPort,delegate{return !Ready||activeJob!=null||submitting||polling||devicePolling;});
            NewPage("serial").Controls.Add(Terminal);ShowPage("flash");
        }
        private Panel NewPage(string key)
        {
            var page=new Panel {Dock=DockStyle.Fill,Visible=false};pages[key]=page;content.Controls.Add(page);return page;
        }
        internal void ShowPage(string key)
        {
            foreach(var item in pages)item.Value.Visible=item.Key==key;
            pages[key].BringToFront();
            foreach(var item in navigation){item.Value.BackColor=item.Key==key?Color.FromArgb(27,66,64):Background;item.Value.ForeColor=item.Key==key?Accent:Muted;}
            if(key=="ai")UpdateAi();
        }
        private void BuildFlash()
        {
            var page=NewPage("flash");var grid=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=2,RowCount=1};grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,44));grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,56));page.Controls.Add(grid);
            var left=Card("");left.AutoScroll=false;grid.Controls.Add(left,0,0);
            var leftLayout=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=1,RowCount=2,Padding=new Padding(0)};
            leftLayout.RowStyles.Add(new RowStyle(SizeType.Percent,100));leftLayout.RowStyles.Add(new RowStyle(SizeType.AutoSize));left.Controls.Add(leftLayout);
            var configScroll=new Panel {Dock=DockStyle.Fill,AutoScroll=true,Margin=new Padding(0)};leftLayout.Controls.Add(configScroll,0,0);
            var stack=Stack(18);configScroll.Controls.Add(stack);
            var title=Label("烧录配置",Ink,36);title.Font=new Font(Font,FontStyle.Bold);stack.Controls.Add(title);
            stack.Controls.Add(Label("固定链路：电脑 → 主机 USB → ESP-NOW\n从机 → 所选接口 → 目标 MCU",Accent,56));
            AddField(stack,"目标芯片",Chips);AddField(stack,"从机 → 目标板接口",Methods);AddField(stack,"ESP32 主机 USB 端口",Ports);
            stack.Controls.Add(Button("刷新串口",async delegate{await RefreshPorts();Notice("串口列表已更新。");}));
            var pathRow=new TableLayoutPanel {Dock=DockStyle.Top,Height=40,ColumnCount=2};pathRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));pathRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,78));
            Input(Firmware);Firmware.Dock=DockStyle.Fill;pathRow.Controls.Add(Firmware,0,0);
            var browse=Button("浏览",delegate{using(var dialog=new OpenFileDialog {Filter="STM32 固件 (*.bin)|*.bin",CheckFileExists=true}){if(dialog.ShowDialog(this)==DialogResult.OK){Firmware.Text=dialog.FileName;manifest.Text="固件已选择，点击检查镜像查看 SHA256。";}}return Task.FromResult(0);});browse.MinimumSize=new Size(66,32);browse.Padding=new Padding(4);pathRow.Controls.Add(browse,1,0);
            stack.Controls.Add(Label("固件文件 · BIN / 0x08000000",Muted));stack.Controls.Add(pathRow);
            RunAfter.Text="校验通过后启动";RunAfter.Checked=true;RunAfter.AutoSize=true;RunAfter.Margin=new Padding(0,12,0,10);
            hint.ForeColor=Muted;hint.Dock=DockStyle.Top;hint.Height=100;hint.Padding=new Padding(0,5,0,5);stack.Controls.Add(hint);
            var buttons=new FlowLayoutPanel {Dock=DockStyle.Top,AutoSize=true,WrapContents=true};
            var validate=Button("检查镜像",async delegate{await ValidateImage();},false,true);CopyButton(validate,ValidateButton);buttons.Controls.Add(ValidateButton);
            var flash=Button("开始烧录",async delegate{await Flash();},true,true);CopyButton(flash,FlashButton);buttons.Controls.Add(FlashButton);
            var footerStack=Stack(2);footerStack.Controls.Add(RunAfter);footerStack.Controls.Add(buttons);leftLayout.Controls.Add(footerStack,0,1);
            manifest.ForeColor=Muted;manifest.Font=new Font("Consolas",9);manifest.Dock=DockStyle.Top;manifest.Height=130;manifest.Text="选择芯片、从机接口与固件后开始。\n启动地址：0x08000000\n将核对向量表、容量与完整读回。";stack.Controls.Add(manifest);
            Methods.SelectedIndexChanged+=delegate{UpdateMethod();};Chips.SelectedIndexChanged+=delegate{manifest.Text="芯片已变更，请选择对应固件并重新检查镜像。";UpdateChip();};
            var right=Card("");grid.Controls.Add(right,1,0);
            var detail=new TableLayoutPanel {Dock=DockStyle.Fill,RowCount=6,ColumnCount=1};detail.RowStyles.Add(new RowStyle(SizeType.Absolute,34));detail.RowStyles.Add(new RowStyle(SizeType.Absolute,32));detail.RowStyles.Add(new RowStyle(SizeType.Absolute,14));detail.RowStyles.Add(new RowStyle(SizeType.Absolute,36));detail.RowStyles.Add(new RowStyle(SizeType.Percent,100));detail.RowStyles.Add(new RowStyle(SizeType.Absolute,44));right.Controls.Add(detail);
            jobTitle.Text="当前任务";jobTitle.Dock=DockStyle.Fill;jobTitle.Font=new Font(Font,FontStyle.Bold);detail.Controls.Add(jobTitle,0,0);
            phaseLabel.Text="等待任务 · 每次只操作一个目标";phaseLabel.Dock=DockStyle.Fill;phaseLabel.ForeColor=Muted;detail.Controls.Add(phaseLabel,0,1);
            progress.Dock=DockStyle.Fill;progress.Maximum=1000;detail.Controls.Add(progress,0,2);
            detail.Controls.Add(Label("检查请求  →  执行操作  →  验证结果",Muted),0,3);
            var logTabs=new TabControl {Dock=DockStyle.Fill};var logPage=new TabPage("运行日志");var jsonPage=new TabPage("结构化结果");StyleLog(logs);StyleLog(results);logPage.Controls.Add(logs);jsonPage.Controls.Add(results);logTabs.TabPages.Add(logPage);logTabs.TabPages.Add(jsonPage);detail.Controls.Add(logTabs,0,4);
            logs.Text="这里显示后台任务日志。\n\nAI 和桌面界面使用同一串口调度，避免互相占用。\n同一个任务键重试时返回原任务，不重复擦写。";
            var footer=new FlowLayoutPanel {Dock=DockStyle.Fill,AutoSize=true};footer.Controls.Add(Button("打开任务目录",delegate{OpenJobDirectory();return Task.FromResult(0);}));footer.Controls.Add(Button("复制结果 JSON",delegate{Clipboard.SetText(results.Text.Length>0?results.Text:"{}");return Task.FromResult(0);}));detail.Controls.Add(footer,0,5);
        }
        private void CopyButton(Button source,Button target)
        {
            target.Text=source.Text;target.AutoSize=source.AutoSize;target.Height=source.Height;target.MinimumSize=source.MinimumSize;target.FlatStyle=source.FlatStyle;target.FlatAppearance.BorderSize=0;target.BackColor=source.BackColor;target.ForeColor=source.ForeColor;target.Padding=source.Padding;target.Margin=source.Margin;target.Cursor=source.Cursor;
            actions.Remove(source);actions.Add(target);
            if(target==ValidateButton)target.Click+=async delegate{try{await ValidateImage();}catch(Exception ex){Notice(ex.Message,true);}};
            else target.Click+=async delegate{try{await Flash();}catch(Exception ex){Notice(ex.Message,true);}};
            source.Dispose();
        }
        private void BuildPid()
        {
            var page=NewPage("pid");var card=Card("");page.Controls.Add(card);var stack=Stack(16);card.Controls.Add(stack);
            var heading=Label("本地与无线 PID 参数",Ink,44);heading.Font=new Font(Font.FontFamily,17,FontStyle.Bold);stack.Controls.Add(heading);
            stack.Controls.Add(Label("读取主机参数，调节 P / I / D 与 STEP，并等待从机同步确认。",Muted,38));
            foreach(var number in new[]{pNumber,iNumber,dNumber}){number.DecimalPlaces=2;number.Maximum=9999.99m;number.Increment=.01m;number.ThousandsSeparator=false;}
            AddField(stack,"P · 比例",pNumber);AddField(stack,"I · 积分",iNumber);AddField(stack,"D · 微分",dNumber);
            stepBox.Items.AddRange(new object[]{"0.01","0.1","1","10"});stepBox.SelectedIndex=2;AddField(stack,"STEP · 旋钮步长",stepBox);
            var buttons=new FlowLayoutPanel {Dock=DockStyle.Top,AutoSize=true,WrapContents=true};
            buttons.Controls.Add(Button("读取参数",async delegate{await ReadDevice();}));
            buttons.Controls.Add(Button("应用参数",async delegate {await Submit(new Dictionary<string,object>{{"action","pid"},{"port",HostPort()},{"parameters",new Dictionary<string,object>{{"p",pNumber.Value},{"i",iNumber.Value},{"d",dNumber.Value},{"step",stepBox.Text}}}});},true,true));
            buttons.Controls.Add(Button("RUN",async delegate{await PanelAction("run");},false,true));buttons.Controls.Add(Button("STOP",async delegate{await PanelAction("stop");},false,true));
            buttons.Controls.Add(Button("保存到主机",async delegate{await PanelAction("save");},false,true));buttons.Controls.Add(Button("载入已保存",async delegate{await PanelAction("load");},false,true));stack.Controls.Add(buttons);
            panelState.Dock=DockStyle.Top;panelState.Height=90;panelState.ForeColor=Accent;panelState.Text="等待读取主机状态。";stack.Controls.Add(panelState);
            stack.Controls.Add(Label("当前确认来自从机。目标 MCU 的 PID 参数/执行反馈协议待接入，RUN ACK 不代表电机已运行。",Muted,65));
        }
        private void BuildEcho()
        {
            var page=NewPage("echo");var card=Card("");page.Controls.Add(card);var stack=Stack(12);card.Controls.Add(stack);
            var heading=Label("STM32 文本回传与完整性检查",Ink,44);heading.Font=new Font(Font.FontFamily,17,FontStyle.Bold);stack.Controls.Add(heading);
            stack.Controls.Add(Label("电脑 → 主机 → ESP-NOW → 从机 UART → F103，再沿原路返回。",Muted,34));
            echoMode.Items.AddRange(new object[]{"输入文字","选择文件","自动生成 128 KiB 文本"});echoMode.SelectedIndex=0;AddField(stack,"测试数据来源",echoMode);
            EchoText.Multiline=true;EchoText.Height=190;EchoText.ScrollBars=ScrollBars.Vertical;EchoText.Text="你好，STM32！\r\nP=1.25 I=0.12 D=0.15\r\n检查中文、换行和 ABC123 是否原样返回。";AddField(stack,"发送文本",EchoText);EchoText.Height=190;
            AddField(stack,"文件绝对路径（文件模式）",EchoFile);
            var row=new FlowLayoutPanel {Dock=DockStyle.Top,AutoSize=true};row.Controls.Add(Button("选择文件",delegate{using(var dialog=new OpenFileDialog {Filter="文本或二进制文件|*.*"})if(dialog.ShowDialog(this)==DialogResult.OK){EchoFile.Text=dialog.FileName;echoMode.SelectedIndex=1;}return Task.FromResult(0);}));
            row.Controls.Add(Button("开始回传测试",async delegate{var spec=new Dictionary<string,object>{{"action","echo"},{"port",HostPort()}};if(echoMode.SelectedIndex==0)spec["text"]=EchoText.Text;else if(echoMode.SelectedIndex==1)spec["file"]=EchoFile.Text;else spec["bytes"]=131072;await Submit(spec);},true,true));stack.Controls.Add(row);
            echoHint.Dock=DockStyle.Top;echoHint.Height=90;echoHint.ForeColor=Muted;echoHint.Text="需要 F103 运行 UART_ECHO 测试固件。逐块比较、总长度、唯一块数、CRC32 与完整 SHA256 全部一致才算通过。\n原文、回传文本和 JSON 报告会保存在任务目录。";stack.Controls.Add(echoHint);
        }
        private void BuildAi()
        {
            var page=NewPage("ai");var card=Card("");page.Controls.Add(card);
            var grid=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=1,RowCount=3};grid.RowStyles.Add(new RowStyle(SizeType.Absolute,48));grid.RowStyles.Add(new RowStyle(SizeType.Percent,100));grid.RowStyles.Add(new RowStyle(SizeType.Absolute,46));card.Controls.Add(grid);
            var heading=Label("AI 自动化接口",Ink,40);heading.Font=new Font(Font.FontFamily,17,FontStyle.Bold);grid.Controls.Add(heading,0,0);StyleLog(aiHelp);grid.Controls.Add(aiHelp,0,1);
            var row=new FlowLayoutPanel {Dock=DockStyle.Fill};row.Controls.Add(Button("复制当前烧录请求",delegate{Clipboard.SetText(Json.Pretty(FlashSpec()));return Task.FromResult(0);}));row.Controls.Add(Button("打开 AI 接入文档",delegate{Process.Start(new ProcessStartInfo(Path.Combine(BackendClient.AppDirectory,"AI_USAGE.md")){UseShellExecute=true});return Task.FromResult(0);}));row.Controls.Add(Button("打开任务记录",delegate{Directory.CreateDirectory(BackendClient.DataDirectory);Process.Start("explorer.exe",BackendClient.Quote(BackendClient.DataDirectory));return Task.FromResult(0);}));grid.Controls.Add(row,0,2);
        }
        private string ChoiceId(ComboBox combo,string fallback){var item=combo.SelectedItem as Choice;return item==null?fallback:item.Id;}
        private string SelectedPort(){return ChoiceId(Ports,"auto");}
        private string HostPort(){return SelectedPort();}
        private Dictionary<string,object> FlashSpec()
        {
            var method=Methods.SelectedItem as Choice;
            return new Dictionary<string,object>{{"action","flash"},{"chip",ChoiceId(Chips,"STM32F103C8T6")},{"transport","esp_now"},{"target_interface",method==null?"uart":method.TargetInterface},
                {"firmware",Firmware.Text},{"port",SelectedPort()},{"run",RunAfter.Checked},{"idempotency_key","ui-"+Guid.NewGuid().ToString("N")}};
        }
        private async Task ValidateImage()
        {
            var data=await api.Call("/v1/plan",FlashSpec());var plan=Json.Object(Json.Value(data,"data"));
            manifest.Text="镜像检查通过\n"+Json.Text(plan,"size")+" 字节 / "+Json.Text(plan,"flash_kb")+" KiB Flash\nSHA256\n"+Json.Text(plan,"sha256");Notice("镜像检查通过；目标尚未写入。");
        }
        private async Task Flash()
        {
            var spec=FlashSpec();var data=await api.Call("/v1/plan",spec);var plan=Json.Object(Json.Value(data,"data"));spec["expected_sha256"]=Json.Text(plan,"sha256");await Submit(spec);
        }
        private async Task PanelAction(string action){await Submit(new Dictionary<string,object>{{"action",action},{"port",HostPort()}});}
        private async Task Submit(Dictionary<string,object> spec)
        {
            if(submitting||activeJob!=null)throw new Exception("当前已有任务，请等待完成。");
            submitting=true;EnableActions(false);
            try {
                if(!spec.ContainsKey("idempotency_key"))spec["idempotency_key"]="ui-"+Guid.NewGuid().ToString("N");
                var value=await api.Call("/v1/jobs",spec);var job=Json.Object(Json.Value(value,"data"));activeJob=Json.Text(job,"id");lastJob=activeJob;ShowPage("flash");DisplayJob(job);Notice("任务已提交："+activeJob);
            } finally {submitting=false;if(activeJob==null)EnableActions(true);}
        }
        private async Task Startup()
        {
            try {
                await api.Start();var caps=await api.Call("/v1/capabilities");var data=Json.Object(Json.Value(caps,"data"));
                foreach(object item in (IEnumerable)Json.Value(data,"chips")){
                    var chip=Json.Object(item);var choice=new Choice(Json.Text(chip,"id"),Json.Text(chip,"label"));
                    choice.DefaultInterface=Json.Text(chip,"default_interface","uart");
                    choice.Interfaces=new HashSet<string>(((IEnumerable)Json.Value(chip,"interfaces")).Cast<object>().Select(Convert.ToString));Chips.Items.Add(choice);
                }
                foreach(object item in (IEnumerable)Json.Value(data,"methods")){var method=Json.Object(item);Methods.Items.Add(new Choice(Json.Text(method,"id"),Json.Text(method,"label"),Json.Bool(method,"available"),Json.Text(method,"target_interface")));}
                Chips.SelectedIndex=0;Methods.SelectedIndex=0;await RefreshPorts();
                string candidate=Path.GetFullPath(Path.Combine(BackendClient.AppDirectory,"..","STM32F103_UART_ECHO","uart_echo.bin"));if(File.Exists(candidate))Firmware.Text=candidate;
                LoadSettings();
                Ready=true;EnableActions(true);Notice("本地服务已就绪 · GUI 与 AI 共用任务队列");pollTimer.Start();
                try{await ReadDevice();}catch(Exception){deviceLabel.Text="未连接 ESP32 主机";wirelessLabel.Text="本地服务已就绪";}
            } catch(Exception error){StartupError=error.Message;Notice(error.Message,true);deviceLabel.Text="服务未就绪";}
        }
        private async Task RefreshPorts()
        {
            var envelope=await api.Call("/v1/ports");string selected=SelectedPort();Ports.Items.Clear();Ports.Items.Add(new Choice("auto","自动识别 ESP32 主机"));
            foreach(object value in (IEnumerable)Json.Value(envelope,"data")){var p=Json.Object(value);if(Json.Text(p,"role")=="host")Ports.Items.Add(new Choice(Json.Text(p,"port"),Json.Text(p,"port")+"  ESP32 主机 USB"));}
            Ports.SelectedIndex=0;for(int i=0;i<Ports.Items.Count;++i)if(((Choice)Ports.Items[i]).Id==selected)Ports.SelectedIndex=i;
        }
        private void UpdateMethod()
        {
            string method=ChoiceId(Methods,"wireless_uart");
            if(method=="wireless_uart")hint.Text="ESP-NOW → 从机 UART → 目标 ROM。\n需要 TX/RX、BOOT0、NRST、VTREF、GND。F103 实板通过；G431/F407 待实板验收。";
            else if(ChoiceId(Chips,"").StartsWith("STM32F407V"))hint.Text="PA13/SWDIO、PA14/SWCLK、NRST、3V3、GND\n无需 BOOT0；VET6 512KB，VGT6 1MB。";
            else hint.Text="从机 SWD 当前适配 STM32F407VGT6/VET6。\n所选芯片请使用 UART；后续可继续扩展 SWD 芯片算法。";
            UpdateFlashAvailability();
        }
        private void UpdateChip()
        {
            var chip=Chips.SelectedItem as Choice;
            if(chip!=null)for(int i=0;i<Methods.Items.Count;++i)if(((Choice)Methods.Items[i]).TargetInterface==chip.DefaultInterface){Methods.SelectedIndex=i;break;}
            if(chip!=null&&chip.Id!="STM32F103C8T6"&&Firmware.Text.Replace('\\','/').EndsWith("STM32F103_UART_ECHO/uart_echo.bin",StringComparison.OrdinalIgnoreCase))Firmware.Text="";
            UpdateMethod();
        }
        private async Task ReadDevice()
        {
            if(devicePolling||closing)return;devicePolling=true;
            try {
                var envelope=await api.Call("/v1/device?port="+Uri.EscapeDataString(HostPort()));var data=Json.Object(Json.Value(envelope,"data"));var state=Json.Object(Json.Value(data,"state"));
                if(state.Count==0)return;
                if(!pNumber.Focused&&!iNumber.Focused&&!dNumber.Focused&&!stepBox.Focused){SetNumber(pNumber,state,"p");SetNumber(iNumber,state,"i");SetNumber(dNumber,state,"d");decimal step;if(Decimal.TryParse(Json.Text(state,"step"),System.Globalization.NumberStyles.Any,System.Globalization.CultureInfo.InvariantCulture,out step)){LastReadStep=step.ToString("0.##",System.Globalization.CultureInfo.InvariantCulture);stepBox.SelectedItem=LastReadStep;}}
                deviceLabel.Text="主机 "+Json.Text(data,"port",SelectedPort());deviceLabel.ForeColor=Ink;
                wirelessLabel.Text=Json.Bool(state,"link_online")?(Json.Bool(state,"synced")?"无线已连接 · 参数已同步":"无线已连接 · 同步中"):"无线未连接";wirelessLabel.ForeColor=Json.Bool(state,"link_online")?Accent:Color.Goldenrod;
                panelState.Text="主机请求："+(Json.Bool(state,"run_request")?"RUN":"STOP")+"    从机确认："+(Json.Bool(state,"ack")?"已收到":"等待")+"\n参数存储："+(Json.Bool(state,"dirty")?"有未保存变更":"已保存")+"    确认范围：从机";
            } finally {devicePolling=false;lastDevice=DateTime.UtcNow;}
        }
        private static void SetNumber(NumericUpDown number,Dictionary<string,object> state,string key){decimal value;if(Decimal.TryParse(Json.Text(state,key),System.Globalization.NumberStyles.Any,System.Globalization.CultureInfo.InvariantCulture,out value))number.Value=Math.Max(number.Minimum,Math.Min(number.Maximum,value));}
        private async Task Poll()
        {
            if(polling||closing)return;polling=true;
            bool restartService=false;
            try {
              try {
                var envelope=await api.Call("/v1/jobs");var list=((IEnumerable)Json.Value(envelope,"data")).Cast<object>().Select(Json.Object).ToList();
                var running=list.FirstOrDefault(j=>Json.Text(j,"state")=="running"||Json.Text(j,"state")=="queued");
                if(running!=null){activeJob=Json.Text(running,"id");lastJob=activeJob;EnableActions(false);}
                else if(list.Count>0&&lastJob!=Json.Text(list[0],"id"))lastJob=Json.Text(list[0],"id");
                string tracked=activeJob??lastJob;
                if(tracked!=null){var detail=await api.Call("/v1/jobs/"+tracked);var job=Json.Object(Json.Value(detail,"data"));DisplayJob(job);string state=Json.Text(job,"state");if(state!="running"&&state!="queued"){activeJob=null;EnableActions(true);}}
                if(activeJob==null&&DateTime.UtcNow-lastDevice>TimeSpan.FromSeconds(4))await ReadDevice();
            } catch(Exception error){
                if(!closing){
                    Notice(error.Message,true);wirelessLabel.Text="连接状态待确认";wirelessLabel.ForeColor=Color.Goldenrod;
                    var failure=error as BackendFailure;
                    if(failure!=null&&failure.Code=="network_unavailable"&&DateTime.UtcNow>=serviceRetryAfter){
                        serviceRetryAfter=DateTime.UtcNow.AddSeconds(5);
                        restartService=true;
                    }
                }
              }
              if(restartService){
                  try{await api.Start();if(!closing)Notice("本地服务已恢复，正在重新读取设备和任务；未重放写入请求。");}
                  catch(Exception startError){if(!closing)Notice("后台服务恢复失败："+startError.Message,true);}
              }
            }finally{polling=false;}
        }
        private void DisplayJob(Dictionary<string,object> job)
        {
            if(closing)return;var spec=Json.Object(Json.Value(job,"spec"));string state=Json.Text(job,"state");
            jobTitle.Text=Json.Text(spec,"action").ToUpperInvariant()+"  "+Json.Text(job,"id");
            var phases=new Dictionary<string,string>{{"queued","排队"},{"validate","检查镜像"},{"probe","探测芯片"},{"transfer","无线传输"},{"erase","擦除"},{"write","写入"},{"verify","完整读回"},{"start","请求启动"},{"echo","文本回传"},{"done","完成"},{"failed","失败"}};
            string phase=Json.Text(job,"phase"),label;phases.TryGetValue(phase,out label);phaseLabel.Text=(label??phase)+" · "+state;
            phaseLabel.ForeColor=state=="failed"||state=="interrupted"?Color.Salmon:Accent;
            double value;Double.TryParse(Json.Text(job,"progress"),System.Globalization.NumberStyles.Any,System.Globalization.CultureInfo.InvariantCulture,out value);progress.Value=Math.Max(0,Math.Min(1000,(int)(value*1000)));
            string log=Json.Text(job,"log");if(logs.Text!=log){logs.Text=log;logs.SelectionStart=logs.TextLength;logs.ScrollToCaret();}
            results.Text=Json.Pretty(job);
            if(state=="succeeded")statusLabel.Text="任务完成，结果已保存；关闭窗口不会清除记录。";
            if(state=="failed"||state=="interrupted")statusLabel.Text=Json.Text(Json.Object(Json.Value(job,"error")),"message","任务未完成，查看日志。");
        }
        private void EnableActions(bool enabled){actionsEnabled=enabled;foreach(var action in actions)if(!action.IsDisposed)action.Enabled=enabled;UpdateFlashAvailability();}
        private void UpdateFlashAvailability()
        {
            var method=Methods.SelectedItem as Choice;var chip=Chips.SelectedItem as Choice;
            bool implemented=method!=null&&method.Available&&chip!=null&&chip.Interfaces!=null&&chip.Interfaces.Contains(method.TargetInterface);
            bool available=actionsEnabled&&implemented;FlashButton.Enabled=available;ValidateButton.Enabled=available;RunAfter.Enabled=available;
            FlashButton.Text=method!=null&&!implemented?"接口未适配":"开始烧录";
            FlashButton.BackColor=method!=null&&!implemented?Color.FromArgb(34,47,65):Accent;
        }
        private void Notice(string message,bool error=false){if(closing)return;statusLabel.Text=message.Replace("\r"," ").Replace("\n"," ");statusLabel.ForeColor=error?Color.Salmon:Muted;}
        private void OpenJobDirectory(){string path=lastJob==null?BackendClient.DataDirectory:Path.Combine(BackendClient.DataDirectory,"jobs",lastJob);Directory.CreateDirectory(path);Process.Start("explorer.exe",BackendClient.Quote(path));}
        private void UpdateAi()
        {
            aiHelp.Text="本地接口  "+BackendClient.BaseUrl+"\n\n统一硬件路径\n电脑 → 主机 USB → ESP-NOW → 从机 → UART / SWD → 目标板\nUART 支持 F103/G431/F407；SWD 当前适配 F407，需更新两端固件并做实板验收。\n\nAI 与界面共用任务引擎；不需要点击鼠标。\n\nCLI\n  pidctl.cmd capabilities\n  pidctl.cmd ports\n  pidctl.cmd device\n  pidctl.cmd flash --chip STM32F407VGT6 --interface swd\n      --firmware C:\\firmware\\f407.bin --idempotency-key my-build-001 --wait\n\nHTTP API\n  GET  /v1/capabilities\n  GET  /v1/ports\n  GET  /v1/device\n  POST /v1/plan\n  POST /v1/jobs\n  GET  /v1/jobs/{id}\n  GET  /v1/openapi.json\n\n认证文件（仅本机）\n  "+Path.Combine(BackendClient.DataDirectory,"api_token.txt")+"\n  Authorization: Bearer <token>\n\n同一个任务键 + 同一请求会返回原任务；更换固件或参数请使用新任务键。\n任务进行时独占串口，AI 和界面不会互相抢占。\n\n当前烧录请求示例\n"+Json.Pretty(FlashSpec());
            aiHelp.Text+="\n\n双向无线串口（H2）\n  pidctl.cmd serial configure --session 12345678 --channel h2 --baud 115200\n  pidctl.cmd serial send --session 12345678 --text status --ending crlf --idempotency-key status-001\n  pidctl.cmd serial read --session 12345678 --cursor 0\n  POST /v1/serial/send\n发送结果不确定时保持 session、编号及内容重试；旧会话发送不自动重放。详见 docs/WIRELESS_SERIAL.md。";
        }
    }
}

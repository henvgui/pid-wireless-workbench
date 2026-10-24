using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PidWorkbench
{
    internal sealed class SerialTerminalPanel : UserControl
    {
        private readonly BackendClient api;
        private readonly Func<string> port;
        private readonly Func<bool> busy;
        private readonly ComboBox baud=new DarkComboBox(),bits=new DarkComboBox(),parity=new DarkComboBox(),stop=new DarkComboBox();
        private readonly ComboBox format=new DarkComboBox(),encoding=new DarkComboBox();
        private readonly ComboBox channel=new DarkComboBox(),sendFormat=new DarkComboBox(),ending=new DarkComboBox();
        private readonly TextBox sendText=new TextBox();
        private readonly Button send=new Button(),retrySend=new Button();
        private bool sending,sendUncertain;
        private long transmitted;
        private Dictionary<string,object> lastSend;
        private readonly RichTextBox output=new RichTextBox();
        private readonly Label state=new Label(),counts=new Label();
        private readonly CheckBox timestamps=new CheckBox(),scroll=new CheckBox();
        private readonly Button open=new Button(),close=new Button(),pause=new Button();
        private readonly System.Windows.Forms.Timer timer=new System.Windows.Forms.Timer();
        private readonly Queue<byte[]> raw=new Queue<byte[]>();
        private Decoder decoder;
        private string session;
        private uint cursor;
        private long received;
        private int retained;
        private bool reading,receiving,paused,lineStart=true;
        private bool stopping,reopenPending,servicePending;
        private int retryAttempt;
        private DateTime retryAfter=DateTime.MinValue;
        private DateTime lastByte=DateTime.MinValue;
        private string connectedPort;
        private string recoverySession;
        private const int RetainedLimit=2*1024*1024,DisplayLimit=250000;
        internal SerialTerminalPanel(BackendClient client,Func<string> hostPort,Func<bool> isBusy)
        {
            api=client;port=hostPort;busy=isBusy;Dock=DockStyle.Fill;BackColor=MainForm.Surface;ForeColor=MainForm.Ink;Padding=new Padding(16);
            var root=new TableLayoutPanel {Dock=DockStyle.Fill,ColumnCount=1,RowCount=7};
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute,30));root.RowStyles.Add(new RowStyle(SizeType.Percent,100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute,68));root.RowStyles.Add(new RowStyle(SizeType.AutoSize));root.RowStyles.Add(new RowStyle(SizeType.Absolute,42));Controls.Add(root);
            var config=new FlowLayoutPanel {Dock=DockStyle.Top,AutoSize=true,WrapContents=true,Margin=new Padding(0)};
            foreach(string value in new[]{"1200","2400","4800","9600","19200","38400","57600","115200","230400","460800","921600"})baud.Items.Add(value);
            foreach(string value in new[]{"5","6","7","8"})bits.Items.Add(value);
            foreach(string value in new[]{"N 无校验","O 奇校验","E 偶校验"})parity.Items.Add(value);
            stop.Items.AddRange(new object[]{"1","2"});format.Items.AddRange(new object[]{"文本","HEX"});encoding.Items.AddRange(new object[]{"UTF-8","GB18030","ASCII"});
            channel.Items.AddRange(new object[]{"H2 双向串口","H1 单向日志","从机内部自检"});channel.SelectedIndex=0;
            AddChoice(config,"串口通道",channel,156);AddChoice(config,"从机 UART 波特率",baud,138);AddChoice(config,"数据位",bits,78);AddChoice(config,"校验位",parity,126);AddChoice(config,"停止位",stop,78);AddChoice(config,"显示",format,90);AddChoice(config,"文本编码",encoding,112);
            baud.Text="115200";bits.Text="8";parity.SelectedIndex=0;stop.SelectedIndex=0;format.SelectedIndex=0;encoding.SelectedIndex=0;root.Controls.Add(config,0,0);
            var tools=new FlowLayoutPanel {Dock=DockStyle.Top,AutoSize=true,WrapContents=true,Margin=new Padding(0,8,0,0)};
            StyleButton(open,"连接串口");StyleButton(close,"关闭串口");StyleButton(pause,"暂停显示");
            var clear=new Button();StyleButton(clear,"清空显示");var save=new Button();StyleButton(save,"保存日志");
            tools.Controls.AddRange(new Control[]{open,close,pause,clear,save});
            timestamps.Text="时间戳";timestamps.Checked=true;timestamps.AutoSize=true;timestamps.Margin=new Padding(8,9,8,0);
            scroll.Text="自动滚动";scroll.Checked=true;scroll.AutoSize=true;scroll.Margin=new Padding(0,9,0,0);tools.Controls.Add(timestamps);tools.Controls.Add(scroll);root.Controls.Add(tools,0,1);
            state.Text="H2：TX/GPIO17 → STM32 RX，RX/GPIO18 ← STM32 TX；GND 共地";state.Dock=DockStyle.Fill;state.ForeColor=MainForm.Muted;state.TextAlign=ContentAlignment.MiddleLeft;root.Controls.Add(state,0,2);
            output.Dock=DockStyle.Fill;output.ReadOnly=true;output.BackColor=MainForm.Field;output.ForeColor=MainForm.Ink;output.BorderStyle=BorderStyle.None;output.Font=new Font("Consolas",10);output.DetectUrls=false;output.WordWrap=true;root.Controls.Add(output,0,3);
            sendText.Multiline=true;sendText.Dock=DockStyle.Fill;sendText.ScrollBars=ScrollBars.Vertical;sendText.BackColor=MainForm.Field;sendText.ForeColor=MainForm.Ink;sendText.Font=new Font("Consolas",10);sendText.BorderStyle=BorderStyle.FixedSingle;sendText.MaxLength=4096;root.Controls.Add(sendText,0,4);
            var sendTools=new FlowLayoutPanel {Dock=DockStyle.Top,AutoSize=true,WrapContents=true,Margin=new Padding(0,5,0,0)};
            sendFormat.Items.AddRange(new object[]{"文本","HEX"});sendFormat.SelectedIndex=0;ending.Items.AddRange(new object[]{"无结尾","CR","LF","CRLF"});ending.SelectedIndex=3;
            AddChoice(sendTools,"发送格式",sendFormat,100);AddChoice(sendTools,"文本结尾",ending,100);
            StyleButton(send,"发送");StyleButton(retrySend,"重试上次");send.Margin=retrySend.Margin=new Padding(0,27,7,0);sendTools.Controls.Add(send);sendTools.Controls.Add(retrySend);
            var note=new Label {Text="单次最多 1024 字节；发送成功表示从机 UART 已接收，目标应答显示在接收区。",AutoSize=true,ForeColor=MainForm.Muted,Margin=new Padding(4,35,0,0)};sendTools.Controls.Add(note);root.Controls.Add(sendTools,0,5);
            counts.Dock=DockStyle.Fill;counts.ForeColor=MainForm.Muted;counts.TextAlign=ContentAlignment.MiddleLeft;counts.Text="接收 0 B · 发送 0 B · 原始接收数据最多保留 2 MiB";root.Controls.Add(counts,0,6);
            send.Enabled=retrySend.Enabled=false;
            close.Enabled=false;pause.Enabled=false;
            open.Click+=async delegate {try{await Start();}catch(Exception error){Error(error.Message);}};
            close.Click+=async delegate {try{await Stop();}catch(Exception error){Error(error.Message);}};
            send.Click+=async delegate {try{await Send(false);}catch(Exception error){Error(error.Message);}};
            retrySend.Click+=async delegate {try{await Send(true);}catch(Exception error){Error(error.Message);}};
            pause.Click+=delegate {paused=!paused;pause.Text=paused?"恢复显示":"暂停显示";if(!paused)Rebuild();};
            clear.Click+=delegate {ClearDisplay();};
            save.Click+=delegate {SaveLog();};format.SelectedIndexChanged+=delegate{Rebuild();};encoding.SelectedIndexChanged+=delegate{Rebuild();};
            timer.Interval=100;timer.Tick+=async delegate{await Read();};LoadSettings();ResetDecoder();
            Disposed+=delegate{timer.Stop();timer.Dispose();};
        }
        private static void StyleButton(Button button,string text)
        {
            button.Text=text;button.AutoSize=true;button.MinimumSize=new Size(88,34);button.Padding=new Padding(7,4,7,4);button.Margin=new Padding(0,0,7,5);
            button.FlatStyle=FlatStyle.Flat;button.FlatAppearance.BorderSize=0;button.BackColor=Color.FromArgb(34,47,65);button.ForeColor=MainForm.Ink;
        }
        private static void AddChoice(FlowLayoutPanel row,string title,ComboBox combo,int width)
        {
            var field=new TableLayoutPanel {ColumnCount=1,RowCount=2,Width=width,Height=68,Margin=new Padding(0,0,8,0)};
            var label=new Label {Text=title,Dock=DockStyle.Fill,ForeColor=MainForm.Muted,TextAlign=ContentAlignment.MiddleLeft};
            combo.Dock=DockStyle.Fill;combo.DropDownStyle=ComboBoxStyle.DropDownList;combo.BackColor=MainForm.Field;combo.ForeColor=MainForm.Ink;combo.FlatStyle=FlatStyle.Flat;
            field.RowStyles.Add(new RowStyle(SizeType.Absolute,27));field.RowStyles.Add(new RowStyle(SizeType.Absolute,34));field.Controls.Add(label,0,0);field.Controls.Add(combo,0,1);row.Controls.Add(field);
        }
        private Encoding TextEncoding(){return encoding.SelectedIndex==1?Encoding.GetEncoding("GB18030"):encoding.SelectedIndex==2?Encoding.ASCII:new UTF8Encoding(false,false);}
        private string ChannelId(){return channel.SelectedIndex==1?"h1":channel.SelectedIndex==2?"loopback":"h2";}
        private string ChannelLabel(){return channel.SelectedIndex==1?"H1 日志":channel.SelectedIndex==2?"内部自检（目标隔离）":"H2 双向串口";}
        private void UpdateSend(){bool ready=receiving&&!stopping&&!sending&&!reopenPending&&ChannelId()!="h1";send.Enabled=ready&&!sendUncertain;retrySend.Enabled=ready&&lastSend!=null&&Convert.ToString(lastSend["session"])==session;}
        private void ResetDecoder(){decoder=TextEncoding().GetDecoder();lineStart=true;}
        private void Error(string message){if(!IsDisposed){state.Text=message;state.ForeColor=Color.Salmon;}}
        private async Task Start()
        {
            if(busy()){Error("请等待本地服务就绪或当前硬件任务结束。");return;}
            string candidate=Guid.NewGuid().ToString("N").Substring(0,8);
            var config=new Dictionary<string,object>{{"session",candidate},{"channel",ChannelId()},{"port",port()},{"baud",Int32.Parse(baud.Text)},{"bits",Int32.Parse(bits.Text)},{"parity",parity.Text.Substring(0,1)},{"stop",Int32.Parse(stop.Text)}};
            open.Enabled=false;
            try {
                await api.Call("/v1/serial/configure",config);
                if(IsDisposed)return;
                connectedPort=port();session=candidate;cursor=0;received=transmitted=0;lastSend=null;sendUncertain=false;retained=0;raw.Clear();output.Clear();ResetDecoder();receiving=true;paused=false;
                stopping=reopenPending=servicePending=false;retryAttempt=0;retryAfter=DateTime.MinValue;
                recoverySession=null;
                lastByte=DateTime.MinValue;
                close.Enabled=pause.Enabled=true;pause.Text="暂停显示";SetConfigEnabled(false);SaveSettings();timer.Start();
                state.ForeColor=MainForm.Accent;state.Text="已连接 · "+ChannelLabel()+" · "+baud.Text+" / "+bits.Text+parity.Text.Substring(0,1)+stop.Text;UpdateSend();
            } catch {open.Enabled=true;throw;}
        }
        internal async Task StartWhenAvailable()
        {
            DateTime until=DateTime.UtcNow.AddSeconds(15);
            while(busy()&&DateTime.UtcNow<until&&!IsDisposed)await Task.Delay(100);
            if(!IsDisposed)await Start();
        }
        internal async Task Stop()
        {
            if(!receiving)return;
            bool remoteClosed=true;
            stopping=true;reopenPending=servicePending=false;timer.Stop();
            try {
                UpdateSend();while((reading||sending)&&!IsDisposed)await Task.Delay(25);
                if(IsDisposed||!receiving)return;
                if(recoverySession!=null) {
                    var status=Json.Object(Json.Value(await api.Call("/v1/serial/status"),"data"));
                    if(Json.Text(status,"session")==recoverySession)session=recoverySession;
                }
                try{await api.Call("/v1/serial/close",new Dictionary<string,object>{{"session",session}});}
                catch(BackendFailure error){if(error.Code!="serial_session_expired"&&error.Code!="serial_closed"&&
                    !(error.Code=="serial_remote_error"&&(error.RemoteError=="wrong_transfer"||error.RemoteError=="invalid")))throw;}
            }catch(BackendFailure error){
                if(error.Code!="network_unavailable"){stopping=false;if(receiving&&!IsDisposed)timer.Start();throw;}
                remoteClosed=false;
            }catch{stopping=false;if(receiving&&!IsDisposed)timer.Start();throw;}
            if(IsDisposed)return;receiving=false;timer.Stop();close.Enabled=pause.Enabled=false;open.Enabled=true;SetConfigEnabled(true);
            state.ForeColor=MainForm.Muted;state.Text=remoteClosed?"串口已关闭":"本机已停止接收；连接中断，远端关闭未确认";UpdateSend();
        }
        internal void Shutdown(){SaveSettings();timer.Stop();}
        private void SetConfigEnabled(bool value){foreach(var combo in new[]{channel,baud,bits,parity,stop})combo.Enabled=value;}
        private async Task Read()
        {
            if(!receiving||stopping||reading||sending||busy()||IsDisposed||DateTime.UtcNow<retryAfter)return;reading=true;
            try {
                if(servicePending){
                    try{await api.Start();servicePending=false;}
                    catch(Exception error){throw new BackendFailure("network_unavailable","后台服务恢复失败："+error.Message);}
                    if(stopping||IsDisposed)return;
                }
                if(reopenPending){await Reopen();return;}
                var envelope=await api.Call("/v1/serial/read?session="+session+"&cursor="+cursor);
                if(IsDisposed)return;var data=Json.Object(Json.Value(envelope,"data"));
                foreach(object value in (IEnumerable)Json.Value(data,"chunks")) {
                    var chunk=Json.Object(value);uint gap=UInt32.Parse(Json.Text(chunk,"gap","0"));
                    if(gap>0){Append("\n[接收缓冲区溢出，缺失 "+gap+" 字节]\n");ResetDecoder();}
                    byte[] bytes=FromHex(Json.Text(chunk,"hex"));Receive(bytes);
                }
                cursor=UInt32.Parse(Json.Text(data,"next_cursor"));
                retryAttempt=0;retryAfter=DateTime.MinValue;
                state.ForeColor=MainForm.Accent;state.Text=Json.Bool(data,"paused")?"烧录期间 H2 已暂停，完成后自动恢复":"已连接 · "+ChannelLabel()+" · "+baud.Text+" / "+bits.Text+parity.Text.Substring(0,1)+stop.Text+(DateTime.UtcNow-lastByte<TimeSpan.FromSeconds(3)?" · 接收中":" · 等待数据");
                counts.Text="接收 "+received+" B · 发送 "+transmitted+" B · 丢弃 "+Json.Text(data,"dropped")+" B · UART 错误 "+Json.Text(data,"rx_errors")+" · 烧录暂停 "+Json.Text(data,"pause_events","0")+" 次"+(sendUncertain?" · 上次发送待确认，请重试上次":"")+(paused?" · 显示已暂停":"");UpdateSend();
            } catch(Exception error){if(!IsDisposed&&!stopping)Recover(error);}
            finally{reading=false;UpdateSend();}
        }
        internal void Recover(Exception exception)
        {
            var error=exception as BackendFailure;
            var action=SerialRecovery.Decide(error==null?"":error.Code,error==null?"":error.RemoteError);
            if(action==SerialRecoveryAction.Stop){receiving=false;timer.Stop();open.Enabled=true;close.Enabled=pause.Enabled=false;SetConfigEnabled(true);Error(exception.Message);return;}
            if(action==SerialRecoveryAction.Reopen)reopenPending=true;
            if(action==SerialRecoveryAction.RestartService){servicePending=true;reopenPending=true;}
            UpdateSend();
            int delay=SerialRecovery.DelayMs(retryAttempt++);retryAfter=DateTime.UtcNow.AddMilliseconds(delay);
            state.ForeColor=Color.Goldenrod;
            state.Text=(action==SerialRecoveryAction.Busy?"硬件任务进行中，日志等待通道":servicePending?"后台服务断开，正在恢复服务":reopenPending?"日志会话已失效，正在自动恢复":"连接暂时中断，正在自动重试")+" · "+(delay/1000.0).ToString("0.#")+" 秒后重试";
        }
        private async Task Reopen()
        {
            var envelope=await api.Call("/v1/serial/status");var status=Json.Object(Json.Value(envelope,"data"));
            string owner=Json.Text(status,"session");
            if(owner.Length>0&&owner!=session&&owner!=recoverySession)throw new BackendFailure("serial_session_taken","日志已被其他客户端接管；点击开始接收可切回本窗口。");
            if((owner==session||owner==recoverySession)&&Json.Bool(status,"closed"))throw new BackendFailure("serial_closed","日志接收已被关闭。");
            if(stopping)return;
            // Reuse the candidate if its configure reply was lost. The service
            // may already own it; it is still this window's recovery attempt.
            if(recoverySession==null)recoverySession=Guid.NewGuid().ToString("N").Substring(0,8);
            string candidate=recoverySession;
            await api.Call("/v1/serial/configure",new Dictionary<string,object>{{"session",candidate},{"port",connectedPort},
                {"channel",ChannelId()},{"baud",Int32.Parse(baud.Text)},{"bits",Int32.Parse(bits.Text)},{"parity",parity.Text.Substring(0,1)},{"stop",Int32.Parse(stop.Text)}});
            session=candidate;cursor=0;reopenPending=false;retryAttempt=0;retryAfter=DateTime.MinValue;
            recoverySession=null;
            lastSend=null;sendUncertain=false;transmitted=0;UpdateSend();
            lastByte=DateTime.MinValue;
            if(stopping||IsDisposed)return;
            Append("\n[日志会话已重建；中断期间的数据可能缺失]\n");ResetDecoder();
            state.ForeColor=MainForm.Accent;state.Text="已恢复连接 · 等待 STM32 串口数据";
        }
        internal static byte[] FromHex(string hex)
        {
            if(hex.Length%2!=0)throw new FormatException("HEX 长度无效");var bytes=new byte[hex.Length/2];for(int i=0;i<bytes.Length;++i)bytes[i]=Convert.ToByte(hex.Substring(i*2,2),16);return bytes;
        }
        internal async Task Send(bool retry)
        {
            if(!receiving||stopping||sending||reopenPending||ChannelId()=="h1")throw new Exception("请先连接 H2 双向串口。");
            if(!retry&&sendUncertain)throw new Exception("上次发送结果不确定，请先点“重试上次”，避免重复执行命令。");
            if(busy())throw new Exception("硬件任务进行中，请结束后发送。");
            sending=true;UpdateSend();
            try {
                while(reading&&!IsDisposed)await Task.Delay(20);
                if(!retry) {
                    byte[] bytes;
                    if(sendFormat.SelectedIndex==1){string text=new string(sendText.Text.Where(c=>!Char.IsWhiteSpace(c)).ToArray());bytes=FromHex(text);}
                    else {
                        string suffix=ending.SelectedIndex==1?"\r":ending.SelectedIndex==2?"\n":ending.SelectedIndex==3?"\r\n":"";
                        var strict=(Encoding)TextEncoding().Clone();strict.EncoderFallback=EncoderFallback.ExceptionFallback;bytes=strict.GetBytes(sendText.Text+suffix);
                    }
                    if(bytes.Length==0||bytes.Length>1024)throw new Exception("单次发送长度必须为 1～1024 字节。");
                    lastSend=new Dictionary<string,object>{{"session",session},{"idempotency_key",Guid.NewGuid().ToString("N")},{"hex",BitConverter.ToString(bytes).Replace("-","").ToLowerInvariant()}};
                    sendUncertain=true;
                }
                if(lastSend==null||Convert.ToString(lastSend["session"])!=session)throw new Exception("会话已改变，不能重放旧发送。");
                var data=Json.Object(Json.Value(await api.Call("/v1/serial/send",lastSend),"data"));
                sendUncertain=false;
                transmitted=Int64.Parse(Json.Text(data,"tx_total","0"));
                Append("\n[TX "+DateTime.Now.ToString("HH:mm:ss.fff")+" · "+Json.Text(data,"accepted")+" B · 从机 UART 已接收"+(retry?"，重试已去重":"")+"]\n");
                state.ForeColor=MainForm.Accent;state.Text="发送已确认 · 等待目标串口应答";SaveSettings();
            } finally {sending=false;UpdateSend();}
        }
        internal void ConfigureTest(bool loopback,string text){channel.SelectedIndex=loopback?2:0;sendText.Text=text;sendFormat.SelectedIndex=0;ending.SelectedIndex=0;encoding.SelectedIndex=0;}
        internal void Receive(byte[] bytes)
        {
            if(bytes.Length==0)return;lastByte=DateTime.UtcNow;received+=bytes.Length;raw.Enqueue(bytes);retained+=bytes.Length;
            while(retained>RetainedLimit){retained-=raw.Dequeue().Length;}
            if(!paused)Render(bytes);
        }
        private void Render(byte[] bytes)
        {
            if(decoder==null)ResetDecoder();string text;
            if(format.SelectedIndex==1)text=(timestamps.Checked?DateTime.Now.ToString("[HH:mm:ss.fff] "):"")+BitConverter.ToString(bytes).Replace('-',' ')+"\n";
            else {
                var chars=new char[TextEncoding().GetMaxCharCount(bytes.Length)];int n=decoder.GetChars(bytes,0,bytes.Length,chars,0,false);var builder=new StringBuilder();
                for(int i=0;i<n;++i){char c=chars[i];if(lineStart&&timestamps.Checked&&c!='\r'&&c!='\n'){builder.Append(DateTime.Now.ToString("[HH:mm:ss.fff] "));lineStart=false;}builder.Append(c=='\0'?'␀':c);if(c=='\n')lineStart=true;}
                text=builder.ToString();
            }
            Append(text);
        }
        private void Append(string text)
        {
            output.AppendText(text);
            if(output.TextLength>DisplayLimit){output.Select(0,output.TextLength-DisplayLimit);output.SelectedText="";}
            if(scroll.Checked){output.SelectionStart=output.TextLength;output.ScrollToCaret();}
        }
        private void Rebuild()
        {
            if(output==null||IsDisposed)return;output.Clear();ResetDecoder();
            // Bound redraw time even if a long session has filled the raw buffer.
            var latest=raw.Reverse().Take(1800).Reverse();foreach(byte[] bytes in latest)Render(bytes);
        }
        private void SaveLog()
        {
            using(var dialog=new SaveFileDialog {Filter="当前显示文本 (*.txt)|*.txt|保留的原始字节 (*.bin)|*.bin",FileName="serial_"+DateTime.Now.ToString("yyyyMMdd_HHmmss")+".txt"}) {
                if(dialog.ShowDialog(this)!=DialogResult.OK)return;
                if(dialog.FilterIndex==2){using(var stream=File.Create(dialog.FileName))foreach(byte[] bytes in raw)stream.Write(bytes,0,bytes.Length);}
                else File.WriteAllText(dialog.FileName,output.Text,new UTF8Encoding(false));
            }
        }
        private void SaveSettings()
        {
            try{Directory.CreateDirectory(BackendClient.DataDirectory);File.WriteAllText(Path.Combine(BackendClient.DataDirectory,"serial_ui_settings.json"),Json.Encode(new{baud=baud.Text,bits=bits.Text,parity=parity.SelectedIndex,stop=stop.SelectedIndex,format=format.SelectedIndex,encoding=encoding.SelectedIndex,channel=channel.SelectedIndex,sendFormat=sendFormat.SelectedIndex,ending=ending.SelectedIndex}),Encoding.UTF8);}catch(IOException){}
        }
        private void LoadSettings()
        {
            try {
                string path=Path.Combine(BackendClient.DataDirectory,"serial_ui_settings.json");if(!File.Exists(path))return;var saved=Json.Decode(File.ReadAllText(path));
                baud.Text=Json.Text(saved,"baud","115200");bits.Text=Json.Text(saved,"bits","8");
                foreach(var pair in new[]{Tuple.Create(parity,"parity"),Tuple.Create(stop,"stop"),Tuple.Create(format,"format"),Tuple.Create(encoding,"encoding"),Tuple.Create(channel,"channel"),Tuple.Create(sendFormat,"sendFormat"),Tuple.Create(ending,"ending")}) {
                    int index;if(Int32.TryParse(Json.Text(saved,pair.Item2,"0"),out index)&&index>=0&&index<pair.Item1.Items.Count)pair.Item1.SelectedIndex=index;
                }
            }catch(Exception){}
        }
        internal string DisplayedText {get{return output.Text;}}
        internal bool IsReceiving {get{return receiving;}}
        internal string SessionId {get{return session;}}
        internal long ReceivedBytes {get{return received;}}
        internal void ClearDisplay(){raw.Clear();retained=0;output.Clear();ResetDecoder();}
    }
}

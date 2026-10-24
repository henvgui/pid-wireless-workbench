using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace PidWorkbench
{
    internal sealed class BackendFailure:Exception
    {
        internal readonly string Code,RemoteError;
        internal BackendFailure(string code,string message,string remoteError=""):base(message){Code=code;RemoteError=remoteError;}
    }
    internal static class Json
    {
        internal static Dictionary<string,object> Object(object value) { return value as Dictionary<string,object> ?? new Dictionary<string,object>(); }
        internal static string Text(Dictionary<string,object> value,string key,string fallback="") { object raw;return value.TryGetValue(key,out raw)&&raw!=null?Convert.ToString(raw,System.Globalization.CultureInfo.InvariantCulture):fallback; }
        internal static bool Bool(Dictionary<string,object> value,string key) { object raw;return value.TryGetValue(key,out raw)&&raw is bool&&(bool)raw; }
        internal static object Value(Dictionary<string,object> value,string key) { object raw;return value.TryGetValue(key,out raw)?raw:null; }
        internal static string Encode(object value) { return new JavaScriptSerializer {MaxJsonLength=8*1024*1024}.Serialize(value); }
        internal static Dictionary<string,object> Decode(string value) { return Object(new JavaScriptSerializer {MaxJsonLength=8*1024*1024}.DeserializeObject(value)); }
        internal static string Pretty(object value)
        {
            string raw=Encode(value);var text=new StringBuilder();int depth=0;bool quoted=false,escaped=false;
            foreach(char c in raw) {
                if(quoted) { text.Append(c);if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;continue; }
                if(c=='"'){quoted=true;text.Append(c);}
                else if(c=='{'||c=='['){text.Append(c);text.AppendLine();text.Append(new string(' ',++depth*2));}
                else if(c=='}'||c==']'){text.AppendLine();text.Append(new string(' ',--depth*2));text.Append(c);}
                else if(c==','){text.Append(c);text.AppendLine();text.Append(new string(' ',depth*2));}
                else if(c==':')text.Append(": ");else text.Append(c);
            }
            return text.ToString();
        }
    }
    internal sealed class BackendClient
    {
        internal const string BaseUrl="http://127.0.0.1:18765";
        internal static readonly string AppDirectory=AppDomain.CurrentDomain.BaseDirectory;
        internal static readonly string DataDirectory=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"PIDWorkbench");
        internal static readonly string Python=Environment.GetEnvironmentVariable("PID_WORKBENCH_PYTHON")??Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),".platformio","penv","Scripts","python.exe");
        private string token;
        private readonly object startLock=new object();
        private Task starting;

        internal static string Quote(string value)
        {
            var output=new StringBuilder("\"");int slashes=0;
            foreach(char c in value) { if(c=='\\'){++slashes;continue;}if(c=='"'){output.Append('\\',slashes*2+1);output.Append(c);}else{output.Append('\\',slashes);output.Append(c);}slashes=0; }
            output.Append('\\',slashes*2);output.Append('"');return output.ToString();
        }
        internal Task Start()
        {
            lock(startLock){if(starting==null||starting.IsCompleted)starting=StartCore();return starting;}
        }
        private async Task StartCore()
        {
            await Task.Run(delegate {
                if(!File.Exists(Python))throw new Exception("找不到 Python："+Python+"。请安装 PlatformIO，或设置 PID_WORKBENCH_PYTHON。");
                var info=new ProcessStartInfo(Python,Quote(Path.Combine(AppDirectory,"backend","pidctl.py"))+" service") {
                    UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,
                    StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8,WorkingDirectory=AppDirectory};
                using(var process=Process.Start(info)) {
                    string output=process.StandardOutput.ReadToEnd();string error=process.StandardError.ReadToEnd();process.WaitForExit();
                    if(process.ExitCode!=0)throw new Exception(output+error);
                    var envelope=Json.Decode(output);var service=Json.Object(Json.Value(envelope,"data"));
                    string credential=Json.Text(service,"credential_path",Path.Combine(DataDirectory,"api_token.txt"));
                    token=File.ReadAllText(credential,Encoding.ASCII).Trim();
                }
            });
        }
        internal async Task<Dictionary<string,object>> Call(string path,object payload=null)
        {
            return await Task.Run(delegate {
                var request=(HttpWebRequest)WebRequest.Create(BaseUrl+path);request.Proxy=null;request.Timeout=20000;
                request.Headers["Authorization"]="Bearer "+token;request.Accept="application/json";
                string text;
                try {
                    if(payload!=null) {
                        byte[] bytes=Encoding.UTF8.GetBytes(Json.Encode(payload));request.Method="POST";request.ContentType="application/json";request.ContentLength=bytes.Length;
                        using(var stream=request.GetRequestStream())stream.Write(bytes,0,bytes.Length);
                    }
                    using(var response=request.GetResponse())using(var reader=new StreamReader(response.GetResponseStream(),Encoding.UTF8))text=reader.ReadToEnd();
                }
                catch(WebException ex) {
                    if(ex.Response==null)throw new BackendFailure("network_unavailable","本地服务连接失败："+ex.Message);
                    using(var response=ex.Response)using(var reader=new StreamReader(response.GetResponseStream(),Encoding.UTF8))text=reader.ReadToEnd();
                }
                var envelope=Json.Decode(text);
                if(!Json.Bool(envelope,"ok")) {var error=Json.Object(Json.Value(envelope,"error"));throw new BackendFailure(Json.Text(error,"code"),Json.Text(error,"message",text),Json.Text(error,"remote_error"));}
                return envelope;
            });
        }
    }
}

using System;
namespace PidWorkbench
{
    internal enum SerialRecoveryAction { Busy,Retry,RestartService,Reopen,Stop }
    internal static class SerialRecovery
    {
        internal static SerialRecoveryAction Decide(string code,string remote)
        {
            if(code=="device_busy")return SerialRecoveryAction.Busy;
            if(code=="serial_session_expired")return SerialRecoveryAction.Reopen;
            if(code=="serial_remote_error") {
                if(remote=="wrong_transfer"||remote=="invalid"||remote=="offset"||remote=="stale_request")return SerialRecoveryAction.Reopen;
                if(remote=="busy")return SerialRecoveryAction.Busy;
                if(remote=="timeout"||remote=="no_link"||remote=="link")return SerialRecoveryAction.Retry;
            }
            if(code=="network_unavailable")return SerialRecoveryAction.RestartService;
            if(code=="device_or_service_unavailable")return SerialRecoveryAction.Retry;
            return SerialRecoveryAction.Stop;
        }
        internal static int DelayMs(int attempt){return Math.Min(5000,500*(1<<Math.Min(4,Math.Max(0,attempt))));}
    }
}

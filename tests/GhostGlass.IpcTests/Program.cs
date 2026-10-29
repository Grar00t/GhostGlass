using System.IO.Pipes;
using System.Diagnostics;
using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
string root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../../../"));
string host = Path.Combine(root, "src", "GhostGlass.Host", "bin", "Release", "net10.0", "GhostGlass.Host.exe");
if (!File.Exists(host)) { Console.Error.WriteLine($"Host missing {host}"); return 74; }
using Process proc = Process.Start(new ProcessStartInfo(host){UseShellExecute=false,CreateNoWindow=true})!;
try
{
    async Task<NamedPipeClientStream> Connect()
    {
        var pipe = new NamedPipeClientStream(".", "GhostGlass.Host.v1", PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        await pipe.ConnectAsync(5000);
        return pipe;
    }
    async Task<JsonElement> Exchange(int version,string id,string op,string? value=null,string? label=null)
    {
        await using var pipe=await Connect();
        byte[] data=JsonSerializer.SerializeToUtf8Bytes(new {Version=version,Id=id,Op=op,Value=value,Label=label});
        byte[] head=new byte[4];BinaryPrimitives.WriteUInt32LittleEndian(head,(uint)data.Length);
        await pipe.WriteAsync(head);await pipe.WriteAsync(data);await pipe.FlushAsync();
        await pipe.ReadExactlyAsync(head);
        int size=checked((int)BinaryPrimitives.ReadUInt32LittleEndian(head));
        if(size > 1048576)throw new Exception("oversize_response");
        byte[] response=new byte[size];await pipe.ReadExactlyAsync(response);
        return JsonDocument.Parse(response).RootElement.Clone();
    }
    var ping=await Exchange(1,"a1","ping");
    if(!ping.GetProperty("Ok").GetBoolean()||ping.GetProperty("Result").GetString()!="pong")return 65;
    Console.WriteLine("PASS valid framing ping");
    var version=await Exchange(77,"a2","ping");
    if(version.GetProperty("Error").GetString()!="unsupported_version")return 65;
    Console.WriteLine("PASS version rejection");
    var unavailable=await Exchange(1,"a3","dangerous-write");
    if(unavailable.GetProperty("Error").GetString()!="operation_unavailable")return 65;
    Console.WriteLine("PASS unsafe operation denied");
    var secretWrite=await Exchange(1,"security1","credential.save","synthetic-secret-not-real");
    if(secretWrite.GetProperty("Ok").GetBoolean()||secretWrite.GetProperty("Error").GetString()!="credential_ui_approval_required")return 65;
    Console.WriteLine("PASS credential write refused without UI approval");
    var evidence=await Exchange(1,"a4","evidence-only");
    if(evidence.GetProperty("Error").GetString()!="insufficient_evidence")return 65;
    Console.WriteLine("PASS insufficient evidence rejected");
    string fixture=Path.Combine(root,"tests","evidence-local.txt");
    var indexed=await Exchange(1,"file-1","index-file",fixture);
    if(!indexed.GetProperty("Ok").GetBoolean())return 65;
    string sourceId=Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(fixture))).ToLowerInvariant();
    var localFound=await Exchange(1,"file-2","search","Direct2D");
    if(!localFound.GetProperty("Ok").GetBoolean()||!(localFound.GetProperty("Result").GetString()??"").Contains(sourceId))return 65;
    Console.WriteLine("PASS explicit local file index and FTS5 IPC evidence");
    var removed=await Exchange(1,"file-3","remove-index",sourceId);
    if(!removed.GetProperty("Ok").GetBoolean())return 65;
    Console.WriteLine("PASS indexed copy removal over IPC");
    await using(var pipe=await Connect()) {
        byte[] header=new byte[4];BinaryPrimitives.WriteUInt32LittleEndian(header,1048577);
        await pipe.WriteAsync(header);await pipe.FlushAsync();
    }
    await Task.Delay(150);
    var after=await Exchange(1,"a5","ping");
    if(!after.GetProperty("Ok").GetBoolean())return 65;
    Console.WriteLine("PASS oversized frame rejected and host survived");
    await using(var pipe=await Connect()) {
        byte[] header=new byte[4];BinaryPrimitives.WriteUInt32LittleEndian(header,100);
        await pipe.WriteAsync(header);await pipe.WriteAsync(Encoding.UTF8.GetBytes("{}"));
    }
    await Task.Delay(150);
    var recovered=await Exchange(1,"a6","ping");
    if(!recovered.GetProperty("Ok").GetBoolean())return 65;
    Console.WriteLine("PASS truncated frame survived");
    if(args.Contains("--live"))
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(170));
        await using(var livePipe=await Connect())
        {
            byte[] req=JsonSerializer.SerializeToUtf8Bytes(new {Version=1,Id="live-smoke-1",Op="chat",Value=JsonSerializer.Serialize(new[]{new{Role="system",Content="You are a helpful assistant."},new{Role="user",Content="Reply exactly: LOCAL_OK."}})});
            var hdr=new byte[4];BinaryPrimitives.WriteUInt32LittleEndian(hdr,(uint)req.Length);
            await livePipe.WriteAsync(hdr,timeout.Token);
            await livePipe.WriteAsync(req,timeout.Token);
            await livePipe.FlushAsync(timeout.Token);
            bool gotContent=false;
            string detectedProvider="",detectedModel="";
            for(int i=0;i<64;i++)
            {
                await livePipe.ReadExactlyAsync(hdr,timeout.Token);
                int n=checked((int)BinaryPrimitives.ReadUInt32LittleEndian(hdr));
                if(n<1||n>1048576)throw new Exception("Invalid live frame");
                byte[] bytes=new byte[n];await livePipe.ReadExactlyAsync(bytes,timeout.Token);
                using JsonDocument response=JsonDocument.Parse(bytes);
                var frame=response.RootElement;
                if(!frame.GetProperty("Ok").GetBoolean())throw new Exception("provider_error "+frame.GetProperty("Error").GetString());
                detectedProvider=frame.GetProperty("Provider").GetString()??"";
                detectedModel=frame.GetProperty("Model").GetString()??"";
                if(!string.IsNullOrWhiteSpace(frame.GetProperty("Result").GetString()))gotContent=true;
                if(gotContent)break;
                if(frame.GetProperty("Done").GetBoolean())break;
            }
            if(!gotContent||detectedProvider!="ollama-local"||detectedModel!="deepseek-coder-v2:16b")
                throw new Exception("Missing live model content/identity");
            Console.WriteLine("PASS LIVE_OLLAMA_STREAM_TOKEN provider="+detectedProvider+" model="+detectedModel);
        }
        await Task.Delay(1500);
        using var recoveredConnection=await Connect();
        Console.WriteLine("PASS LIVE_STREAM_CLIENT_CANCELLED_HOST_RECONNECTED");
    }
    Console.WriteLine("IPC_INTEGRATION_PASS");
    return 0;
}
finally { if(!proc.HasExited)proc.Kill(entireProcessTree:true); }

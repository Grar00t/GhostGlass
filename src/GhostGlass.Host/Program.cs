using System.IO.Pipes;
using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace GhostGlass.Host;
internal sealed record Request(int Version, string Id, string Op, string? Value, string? Label = null);
internal sealed record Response(int Version, string Id, bool Ok, string? Result, string? Error, bool Done = true, string? Provider = null, string? Model = null);
internal static class Program
{
    const int MaxFrame = 1024 * 1024;
    const string Pipe = "GhostGlass.Host.v1";
    static readonly JsonSerializerOptions JsonOptions = new() { PropertyNameCaseInsensitive = true };
    static async Task Main(string[] args)
    {
        if(args.Length==2 && args[0] is "--index" or "--search" or "--remove"){
            try {
                using var index=new LocalIndex();
                switch(args[0]){
                    case "--index": Console.WriteLine("INDEXED_SOURCE_ID="+index.Add(args[1]));break;
                    case "--search": foreach(var result in index.Search(args[1]))Console.WriteLine("SOURCE\t"+result);break;
                    case "--remove":Console.WriteLine(index.Remove(args[1])?"REMOVED_FROM_INDEX":"NOT_INDEXED");break;
                }
            }catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or DecoderFallbackException){
                Console.Error.WriteLine("INDEX_ERROR="+ex.GetType().Name);
                Environment.ExitCode=65;
            }
            return;
        }

        if(args.Contains("--credential-self-test")){
            string label="test-"+Guid.NewGuid().ToString("N");
            string synthetic="synthetic-test-only-"+Guid.NewGuid().ToString("N");
            try{
                CredentialVault.Save(label,synthetic);
                bool matched=CredentialVault.Exists(label)&&CredentialVault.ReadForProvider(label)==synthetic;
                Console.WriteLine(matched?"CREDENTIAL_VAULT_SELF_TEST_PASS":"CREDENTIAL_VAULT_SELF_TEST_FAIL");
                Environment.ExitCode=matched?0:65;
            }catch(Exception ex) when(ex is IOException or ArgumentException){
                Console.WriteLine("CREDENTIAL_VAULT_SELF_TEST_FAIL");
                Environment.ExitCode=65;
            }finally{CredentialVault.Remove(label);}
            return;
        }
        if (args.Contains("--provider-probe")){
            using var provider=new OllamaProvider();
            try{using var timeout=new CancellationTokenSource(TimeSpan.FromSeconds(5));bool ready=await provider.IsReadyAsync(timeout.Token);
                Console.WriteLine("PROVIDER="+provider.Id+" MODEL="+provider.Model+" READY="+ready);Environment.ExitCode=ready?0:74;
            }catch(Exception ex) when(ex is HttpRequestException or OperationCanceledException or IOException or JsonException){Console.WriteLine("PROVIDER_UNAVAILABLE");Environment.ExitCode=74;}
            return;
        }
        if (args.Contains("--history-self-test")){
            var c=new ConversationState();c.AppendUser("اسمي في هذا الاختبار ناصر.");c.CompleteAssistant("أهلاً ناصر.");c.AppendUser("ما الاسم الذي ذكرته؟");
            var h=ConversationPayload.Validate(c.Payload());
            bool ok=h.Count==4&&h[0].Role=="system"&&h[1].Role=="user"&&h[2].Role=="assistant"&&h[3].Role=="user"&&h[3].Content.Contains("الاسم");
            string[] samples={"GhostGlass — المحادثة","مرحباً، هذا اختبار عربي وإنجليزي.","ملف résumé — صورة.png"};
            foreach(string sample in samples){string rt=JsonSerializer.Deserialize<string>(JsonSerializer.Serialize(sample))??"";ok&=rt==sample&&Encoding.UTF8.GetString(Encoding.UTF8.GetBytes(sample))==sample;}
            Console.WriteLine(ok?"HISTORY_PAYLOAD_PASS roles=system,user,assistant,user":"HISTORY_PAYLOAD_FAIL");Environment.ExitCode=ok?0:65;return;
        }
        if (args.Contains("--self-test"))
        {
            var r = Handle(new Request(1, "self-test", "ping", null));
            Console.WriteLine(r.Ok && r.Result == "pong" ? "HOST_SELF_TEST_PASS" : "HOST_SELF_TEST_FAIL");
            Environment.ExitCode = r.Ok && r.Result == "pong" ? 0 : 65;
            return;
        }
        using var single = new Mutex(true, "Local\\GhostGlass.Host.v1." + Environment.UserName, out var isOwner);
        if (!isOwner) { Console.Error.WriteLine("Host already running for this user."); Environment.ExitCode = 65; return; }
        Console.WriteLine("GhostGlass Host: local pipe ready; no network listener.");
        while (true)
        {
            await using var server = new NamedPipeServerStream(Pipe, PipeDirection.InOut, 1,
                PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
            try
            {
                await server.WaitForConnectionAsync();
                using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
                while (server.IsConnected)
                {
                    byte[]? incoming = await ReadFrame(server, timeout.Token);
                    if (incoming is null) break;
                    Response response;
                    try
                    {
                        var req = JsonSerializer.Deserialize<Request>(incoming, JsonOptions);
                        if(req is { Op: "chat", Version: 1, Id.Length: > 0 and <= 128 }){
                            timeout.CancelAfter(TimeSpan.FromSeconds(180));
                            await StreamChat(server, req, timeout.Token);
                            timeout.CancelAfter(TimeSpan.FromSeconds(30));
                            continue;
                        }
                        response = req is null ? new Response(1, "", false, null, "invalid_request") : Handle(req);
                    }
                    catch (JsonException) { response = new Response(1, "", false, null, "invalid_json"); }
                    await WriteFrame(server, JsonSerializer.SerializeToUtf8Bytes(response), timeout.Token);
                }
            }
            catch (Exception ex) when (ex is IOException or InvalidDataException or OperationCanceledException)
            {
                Console.Error.WriteLine("pipe_connection_closed");
            }
        }
    }
    static async Task StreamChat(Stream pipe, Request request, CancellationToken cancellation){
        try{
            using var provider = new OllamaProvider();
            if(!await provider.IsReadyAsync(cancellation)){
                await WriteFrame(pipe, JsonSerializer.SerializeToUtf8Bytes(new Response(1,request.Id,false,null,"model_unavailable",true,provider.Id,provider.Model)), cancellation);
                return;
            }
            IReadOnlyList<ChatTurn> history;
            try{history=ConversationPayload.Validate(JsonSerializer.Deserialize<List<ChatTurn>>(request.Value??"",JsonOptions)??new());}
            catch(JsonException){
                await WriteFrame(pipe,JsonSerializer.SerializeToUtf8Bytes(new Response(1,request.Id,false,null,"invalid_conversation_payload")),cancellation);return;
            }
            await foreach(var token in provider.StreamAsync(history,cancellation)){
                await WriteFrame(pipe, JsonSerializer.SerializeToUtf8Bytes(new Response(1,request.Id,true,token,null,false,provider.Id,provider.Model)),cancellation);
            }
            await WriteFrame(pipe,JsonSerializer.SerializeToUtf8Bytes(new Response(1,request.Id,true,"",null,true,provider.Id,provider.Model)),cancellation);
        }catch(Exception ex) when(ex is HttpRequestException or InvalidDataException or JsonException or IOException or OperationCanceledException){
            if(!cancellation.IsCancellationRequested){
                string reason=ex switch{
                    HttpRequestException http when http.StatusCode.HasValue => "http_status_"+(int)http.StatusCode.Value,
                    HttpRequestException => "connection_failed",
                    JsonException => "invalid_response",
                    InvalidDataException => "invalid_response",
                    OperationCanceledException => "timeout",
                    _ => "provider_request_failed"
                };
                try{await WriteFrame(pipe,JsonSerializer.SerializeToUtf8Bytes(new Response(1,request.Id,false,null,reason,true,"ollama-local","deepseek-coder-v2:16b")),CancellationToken.None);}
                catch(IOException){ /* client disconnected; no false success */ }
            }
        }
    }

    static Response SearchEvidence(Request request){
        if(string.IsNullOrWhiteSpace(request.Value))return new(1,request.Id,false,null,"insufficient_evidence");
        try{
            using var index=new LocalIndex();
            string[] results=index.Search(request.Value??"").ToArray();
            if(results.Length==0)return new(1,request.Id,false,null,"insufficient_evidence");
            return new(1,request.Id,true,"Unverified source snippets only:\n"+string.Join("\n",results),null);
        }catch(Exception ex) when(ex is IOException or InvalidDataException or ArgumentException){
            return new(1,request.Id,false,null,"search_failed");
        }
    }
    static Response IndexSelectedFile(Request request){
        try{
            if(string.IsNullOrWhiteSpace(request.Value))return new(1,request.Id,false,null,"file_not_selected");
            using var index=new LocalIndex();
            string id=index.Add(request.Value);
            return new(1,request.Id,true,"Indexed source ID: "+id,null);
        }catch(Exception ex) when(ex is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or DecoderFallbackException){
            return new(1,request.Id,false,null,"file_index_failed");
        }
    }
    static Response RemoveSelectedIndex(Request request){
        try{
            using var index=new LocalIndex();
            bool removed=index.Remove(request.Value??"");
            return new(1,request.Id,removed,removed?"Indexed copy removed":null,removed?null:"source_not_found");
        }catch(Exception ex) when(ex is IOException or InvalidDataException or ArgumentException){
            return new(1,request.Id,false,null,"index_remove_failed");
        }
    }
    static Response CredentialRequest(Request request){
        try{
            if(string.IsNullOrWhiteSpace(request.Label))return new(1,request.Id,false,null,"credential_label_required");
            switch(request.Op){
                case "credential.save":
                    if(string.IsNullOrEmpty(request.Value))return new(1,request.Id,false,null,"secret_required");
                    CredentialVault.Save(request.Label,request.Value);
                    return new(1,request.Id,true,"credential_saved",null);
                case "credential.exists":return new(1,request.Id,true,CredentialVault.Exists(request.Label)?"present":"absent",null);
                case "credential.delete":return new(1,request.Id,CredentialVault.Remove(request.Label),"credential_deleted_or_absent",null);
            }
            return new(1,request.Id,false,null,"unsupported_credential_operation");
        }catch(Exception ex) when(ex is IOException or ArgumentException){
            return new(1,request.Id,false,null,"credential_operation_failed");
        }
    }
    static Response Handle(Request request)
    {
        if (request.Version != 1) return new(1, request.Id, false, null, "unsupported_version");
        if (string.IsNullOrWhiteSpace(request.Id) || request.Id.Length > 128) return new(1, "", false, null, "invalid_request_id");
        return request.Op switch
        {
            "ping" => new(1, request.Id, true, "pong", null),
            "capabilities" => new(1, request.Id, true, "ping,capabilities,search,evidence-only,index-file,remove-index,chat(ollama-local;deepseek-coder-v2:16b)", null),
            "search" or "evidence-only" => SearchEvidence(request),
            "index-file" => IndexSelectedFile(request),
            "remove-index" => RemoveSelectedIndex(request),
            "credential.save" or "credential.exists" or "credential.delete" => new(1,request.Id,false,null,"credential_ui_approval_required"),
            _ => new(1, request.Id, false, null, "operation_unavailable")
        };
    }
    static async Task<byte[]?> ReadFrame(Stream s, CancellationToken token)
    {
        var header = new byte[4];
        int n = await s.ReadAsync(header, token);
        if (n == 0) return null;
        while (n < 4)
        {
            int k = await s.ReadAsync(header.AsMemory(n), token);
            if (k == 0) throw new EndOfStreamException();
            n += k;
        }
        uint length = BinaryPrimitives.ReadUInt32LittleEndian(header);
        if (length == 0 || length > MaxFrame) throw new InvalidDataException("frame_size_invalid");
        var bytes = new byte[(int)length];
        await s.ReadExactlyAsync(bytes, token);
        return bytes;
    }
    static async Task WriteFrame(Stream s, byte[] data, CancellationToken token)
    {
        if (data.Length == 0 || data.Length > MaxFrame) throw new InvalidDataException("frame_size_invalid");
        var header = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(header, (uint)data.Length);
        await s.WriteAsync(header, token);
        await s.WriteAsync(data, token);
        await s.FlushAsync(token);
    }
}

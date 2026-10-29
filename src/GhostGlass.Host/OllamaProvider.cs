using System.Runtime.CompilerServices;
using System.Text;
using System.Text.Json;

namespace GhostGlass.Host;

internal interface IChatProvider
{
    string Id { get; }
    string Model { get; }
    Task<bool> IsReadyAsync(CancellationToken cancellationToken);
    IAsyncEnumerable<string> StreamAsync(IReadOnlyList<ChatTurn> history, CancellationToken cancellationToken);
}

internal sealed class OllamaProvider : IChatProvider, IDisposable
{
    private readonly HttpClient client = new()
    {
        BaseAddress = new Uri("http://127.0.0.1:11434/"),
        Timeout = Timeout.InfiniteTimeSpan
    };
    public string Id => "ollama-local";
    public string Model => "deepseek-coder-v2:16b";
    public async Task<bool> IsReadyAsync(CancellationToken token)
    {
        using var response = await client.GetAsync("api/tags", token);
        if (!response.IsSuccessStatusCode) return false;
        using JsonDocument doc = JsonDocument.Parse(await response.Content.ReadAsStreamAsync(token));
        if (!doc.RootElement.TryGetProperty("models", out var models)) return false;
        return models.EnumerateArray().Any(m => m.TryGetProperty("name", out var name) && name.GetString() == Model);
    }
    public async IAsyncEnumerable<string> StreamAsync(IReadOnlyList<ChatTurn> history,[EnumeratorCancellation] CancellationToken cancellationToken)
    {
        ConversationPayload.Validate(history);
        var request = new
        {
            model = Model,
            messages = history.Select(t=>new {role=t.Role,content=t.Content}).ToArray(),
            stream = true,
            options = new {num_predict = 384}
        };
        using var post = new HttpRequestMessage(HttpMethod.Post, "api/chat")
        {
            Content = new StringContent(JsonSerializer.Serialize(request), Encoding.UTF8, "application/json")
        };
        using var response = await client.SendAsync(post, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        int characters = 0;
        while (true)
        {
            string? line = await reader.ReadLineAsync(cancellationToken);
            if (line == null) break;
            if (line.Length > 1024*1024) throw new InvalidDataException("Provider frame too large");
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement;
            if (root.TryGetProperty("error", out var error)) throw new IOException("provider_error:" + error.GetString());
            if (root.TryGetProperty("message", out var message) && message.TryGetProperty("content",out var token))
            {
                string text = token.GetString() ?? "";
                characters += text.Length;
                if (characters > 32000) throw new InvalidDataException("Response size exceeded");
                if (text.Length != 0) yield return text;
            }
            if (root.TryGetProperty("done",out var done) && done.GetBoolean()) break;
        }
    }
    public void Dispose() => client.Dispose();
}

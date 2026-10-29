# GhostGlass protocol v1

Transport: named pipe `\\.\pipe\GhostGlass.Host.v1`. The server creates it with .NET PipeOptions.CurrentUserOnly. No network listener.

Framing: unsigned 32-bit little-endian payload byte length (4 bytes), UTF-8 JSON payload. Length must be between 1 and 1048576. A frame with unsupported version is refused.

Request: `{ "Version":1, "Id":"request-id", "Op":"ping|chat|evidence-only|capabilities", "Value":"optional prompt" }`.

Response: `{ "Version":1, "Id":"request-id", "Ok":true, "Result":"text delta", "Error":null, "Done":false, "Provider":"ollama-local", "Model":"deepseek-coder-v2:16b" }`. All strings are untrusted display data. `chat` sends multiple frames ending with `Done=true`. An error is a `Done=true` frame with `Ok=false`.

`ping`, `evidence-only`, `capabilities`: one response. `chat`: local Ollama only after a successful model probe. Cancellation currently closes the client pipe and signals CancelSynchronousIo; Host aborts on write failure. Client timeout for chat is bounded by Host at 180s. No write tool operations are exposed.

This protocol does not provide encryption at rest or guarantees beyond same-user Windows named pipe permissions. Messages are not logged. No secrets, key rotation or credentials are part of this v1 surface.

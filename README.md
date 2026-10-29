# GhostGlass — native Windows desktop assistant (work in progress)

GhostGlass is a small animated translucent ghost drawn with C++ Win32, Direct2D, Direct3D 11 and DirectComposition, with a separate native RichEdit chat panel. The desktop communicates with a .NET 10 Host using a same-user Windows named pipe (protocol v1, 4-byte little-endian framing, 1 MiB message limit). A local Ollama adapter can stream responses from the explicitly configured `deepseek-coder-v2:16b` model if it is already installed and accessible at `http://127.0.0.1:11434`.

This repository is a **source snapshot for review, not a finished or security-certified product**. The actual implementation and incomplete UI are included; the original ghost drawing is preserved.

## Requirements

Windows 10/11 x64; Visual Studio 2026 with MSVC and Windows SDK 10.0.26100.0; CMake >=3.25; .NET SDK 10.0.403 (pinned in `global.json`); PowerShell. Builds currently select the `Visual Studio 18 2026` CMake generator. No API key is needed for local Ollama. The model is **not** included or downloaded.

## Build and run

From PowerShell at the repository root:

```powershell
& .\scripts\build.ps1
& .\scripts\test.ps1
& .\scripts\run.ps1
```

Optional local inference integration check (requires a running Ollama daemon and the named local model):

```powershell
& .\scripts\test-live.ps1
```

The desktop has a tray menu to open Chat, show/hide the ghost and exit. Default hotkey: Ctrl+Alt+G; G/H can be selected through the current tray menu. The built-in model identity is currently hard-coded, not a generic provider selector. Do not paste keys into source files: Windows Credential Manager helper exists but generic IPC credential writes are deliberately disabled pending proper approval UI.

## What works and what does not

Implemented: Direct2D/DirectComposition ghost, tray/hotkey/click-through, native RichEdit panel with basic streaming/cancel, .NET Host with same-user named pipes, local Ollama /api/chat adapter, SQLite FTS5 UTF-8 text indexing for explicitly chosen TXT/MD/JSON/CSV, source SHA-256 dedup, and Windows Credential Manager wrapper. Chat includes recent **completed** role-based turns in requests; conversation state is in memory only. This is not a modern polished chat UX.

Incomplete or unverified: end-to-end manual Arabic/English multi-turn usability, full visual QA, reliable provider settings, robust cancellation edge cases, approval-bound file mutations, MCP client, PDF/DOCX extraction, screenshot/vision, voice/STT/TTS, cloud providers, full chat persistence and encrypted SQLite storage. There is no validated zero-hallucination guarantee.

Tests are synthetic or programmatic unless expressly labeled live. Passing build and self-tests does **not** mean tray, focus, multi-monitor DPI, accessibility or Arabic IME were manually verified. One older `native-panel-ipc.ps1` experiment is intentionally excluded from this snapshot because it does not represent a passing acceptance test.

## Privacy and security limitations

No default analytics, automatic uploads, or server listening on a public network. Ollama loopback HTTP is the only model endpoint used by the current adapter. Named pipes are restricted to the current Windows user but this is **not** authorization against other same-user processes. Index-file and remove-index are callable through this pipe without a cryptographically bound approval, so do not index sensitive files before this is fixed. SQLite local index is **not encrypted at rest**. Credentials are not stored in JSON and credential writes through general IPC are blocked. Synthetic test fixture data is not an instruction source.

## Licensing

**No license has been granted for GhostGlass original code. Public visibility does not make it open source or grant reuse rights.** Third-party reference repositories have separate MIT licenses; see `THIRD_PARTY_NOTICES.md` and `sources.json`. The external reference source trees, compiled binaries, private data, and historical local logs are not included.

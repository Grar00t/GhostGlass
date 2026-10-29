# Third-party notices and references

References studied during development (not redistributed as source trees):

- [Compositing](https://github.com/stuerp/Compositing), commit `deb75b0d4be9cd3ac73236ba5f8fa167c688a80e`. MIT license, Copyright (c) 2024 Peter Stuer.
- [Windows.UI.Composition-Win32-Samples](https://github.com/microsoft/Windows.UI.Composition-Win32-Samples), commit `ee50e2ea137dcef7b82ba504eff7435e5ebf5294`. MIT license, Copyright (c) Microsoft Corporation.

Both upstream references use the MIT License, which requires keeping copyright and permission notices with copied portions of software. These reference directories are excluded from this repository; no blanket claim of clean-room origin or verified attribution is made. Reviewers should inspect the actual source for any copied code before reuse.

Windows platform libraries, C++ standard libraries and the .NET SDK are installed by the developer's environment and are not vendored into this repository. `winsqlite3.dll` is loaded from Windows at runtime. Local Ollama is optional and its model weights are not redistributed.

**No license for GhostGlass's original source is selected yet.**

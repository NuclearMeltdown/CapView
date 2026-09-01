# Third-party components

CapView itself is under the MIT License; see [LICENSE](LICENSE).

## Bundled in the source tree

**Dear ImGui** — `third_party/imgui` — Copyright (c) 2014-2025 Omar Cornut,
MIT License. Its own `LICENSE.txt` ships alongside the sources, and its
copyright notice must be preserved in any redistribution.

This is the only third-party code in the tree, and it is the only thing linked
into the executable beyond what Windows itself provides.

## Not bundled

**ffmpeg** is not shipped and not linked. Where it is used — recording, and
writing AVIF — CapView starts it as a separate process and talks to it over a
pipe. It is fetched from upstream by the user, on the user's own terms.
Invoking a program is not linking against it, so ffmpeg's licence does not
reach CapView's binary.

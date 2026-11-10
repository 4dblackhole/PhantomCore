# PhantomCore working agreement

This file applies to the entire repository.

## Session onboarding

- Read `Docs/EngineOverview.md` before changing engine code in a new session.
  It is the canonical index for features, ownership, source locations, Client
  boundaries, validation, and the remaining detailed documents.
- For a combined engine/Client task, also read the Client repository's
  `AGENTS.md` and `Docs/ExecutionFlow.md` before deciding where a change belongs.
- Keep `README.md`, `Docs/EngineOverview.md`, and feature documentation in sync
  when a major public feature, ownership boundary, or validation rule changes.

## Purpose and boundary

- This repository contains reusable Windows game-engine code only.
- Build with C++20, Visual Studio 2022, Win32, DirectX 12, and FMOD behind the
  backend-neutral audio API.
- `Engine/SDK/PhantomCore.vcxproj` is the supported consumer project. It builds
  one `PhantomCore.lib` and generates the single public `PhantomCore.h` facade.
- Do not add game-specific assets, Scene IDs, gameplay rules, or private
  Client implementation here.
- Do not commit FMOD SDK files, import libraries, or runtime DLLs.

## Source rules

- Keep feature-first physical folders and keep a feature's `.h` and `.cpp`
  together. Keep `.vcxproj.filters` paths synchronized with physical paths.
- Public APIs must not expose FMOD or D3D12 implementation-only types.
- Client-facing headers and their declaration order are listed once in
  `Engine/SDK/PublicHeaders.props`, shared by MSBuild and
  `Engine/SDK/GenerateCoreHeader.ps1`; never hand-edit generated
  `Engine/SDK/PhantomCore.h`.
- Prefer RAII and `Microsoft::WRL::ComPtr`. Raw pointers are non-owning unless
  an API explicitly documents ownership.
- Keep the unlimited Update loop, QPC input timestamps, render deadline,
  independent audio update deadline, and frame-resource fence model intact.
- Raw Input uses Win32 Virtual-Key values directly; do not add an engine key
  enum.
- `SceneManager` owns Scene factories and lifetime policy. Register through
  `RegisterScene`, transition through deferred `ChangeScene`, and never let a
  Scene delete itself.
- Common shaders belong to `Engine/Graphics.D3D12/Shader` and are embedded at
  build time.
- Keep `Engine/Core/Visual2D` nodes, components, input routing, nine anchors,
  and surface UV mapping backend-neutral. A world presentation composes a
  `Visual2DCanvas`; it must not subclass it. D3D12 drawing belongs in
  `Engine/Graphics.D3D12/Visual2D`.
- Treat absolute mouse position as UI placement data. Rhythm judgement must
  continue to use the ordered Raw Input QPC event stream.
- Keep functions readable as they grow. If a function performs multiple
  operations, extract each operation into a clearly named helper/private
  function. If a long function still represents one cohesive operation, add
  short section comments at each meaningful phase boundary to explain the
  intent and required ordering; do not add comments that merely restate an
  obvious statement.

## Repository synchronization

- Work directly on `main` unless the user explicitly requests a separate
  branch. Do not create a task branch by default.
- Treat synchronization as part of every completed engine task. Fetch the
  engine and Client remotes, verify the local work branch matches its upstream,
  and report exact commit IDs when handing work back.
- In a combined engine/Client workspace, make the final engine commit available
  on the engine remote before updating the Client submodule gitlink. Verify the
  Client gitlink and the checked-out engine `HEAD` are identical.
- After approved PRs are merged, fast-forward both `main` branches and run the
  Client's recursive submodule synchronization/update before declaring the
  repositories current.
- When the user explicitly requests a task branch, publish it after validation,
  create or update its PR, mark it ready, and merge it into `main`. For combined
  changes, merge the engine first and update the Client gitlink to the resulting
  engine `main` commit before merging the Client.
- Never claim that `main` is current while a required PR remains open. Do not
  bypass failed checks, merge conflicts, or branch protection; report such a
  blocker instead. Preserve unrelated working-tree files while synchronizing.

## Verification

After engine changes, rebuild `PhantomCore.sln` in Debug and Release x64.
Run `bin/x64/<Configuration>/PhantomCore.Collision.Tests.exe` after collision or public
SDK changes. Tests must include only `PhantomCore.h`.

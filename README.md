# module-rhi

> **Beta.** MealKit is in beta: interfaces may still change between minor versions.

MealKit engine module `rhi` (`module.txt`, `type engine`): the glue for `mkrhi` from [MkRender](https://github.com/MealKit-Engine/MkRender) (pinned in the engine's `third_party/mkrender`). It names the library `mealkit::rhi` for engine code, routes its log into the engine log and loads GPU capture tools; the DX12 and Vulkan backends live in MkRender since v0.2.0. The engine repository vendors a locked copy in `modules/<name>/` (`modules.lock`, `tools\module.ps1 engine`); a game adds it to its own `modules/` with the engine's `tools\module.ps1 add <name>[@version]`.

Working on it: clone it beside `MealKit` and build the engine with `tools\module.ps1 engine local on <name>`, then tag a version and `tools\module.ps1 engine bump`. MIT license.

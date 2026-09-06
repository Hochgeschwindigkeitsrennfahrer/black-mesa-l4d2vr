# Black Mesa offsets (scanned)

Install: `C:\Program Files (x86)\Steam\steamapps\common\Black Mesa`

| Name | Module | Expected RVA | Status | Found RVAs |
| --- | --- | --- | --- | --- |
| RenderView | client.dll | `0x20EE40` | MATCH | `IViewRender` slot 6 of `CBlackMesaViewRender` (`ret 0xC`, 3-arg `CViewSetup&`). Old `0x207730` is a float-arg helper, not RenderView. |
| g_pClientMode | client.dll | `0x16AD56` | MATCH | `0x16AD56` |
| CreateMove | client.dll | `0x110310` | MATCH | `0x110310` |
| m_hMyWeapons | client.dll | `0xEE4` | MATCH | `DT_BaseCombatCharacter` RecvTable `FUN_100a3de0`, count `0x30`. `m_hActiveWeapon` at `0xFA4`. |
| m_hUseEntity | client.dll | `0x1344` | MATCH | `DT_BasePlayer` RecvTable `FUN_100b6a00`. Held pickup / use target. |
| Grab hold origin | server.dll | `0x77240` | MATCH | `CBasePlayer` vtable `+0x474` `FUN_10077240`. `CGrabController::UpdateObject` `FUN_10470da0` hold origin (not `Weapon_ShootPosition` `+0x23C`). Distance `2*radius+24`. |
| Grab SetTargetPosition | server.dll | `0x470990` | MATCH | `CGrabController::SetTargetPosition` `FUN_10470990`. Final hold point after `2*radius+24`. |
| CalcViewModelView | client.dll | `0x29D930` | MATCH | `0x29D930` |
| AdjustEngineViewport | client.dll | `0x1102C0` | MATCH | `0x1102C0` |
| LevelInit | client.dll | `0x110A80` | MATCH | `0x110A80` |
| LevelShutdown | client.dll | `0x110B30` | MATCH | `0x110B30` |
| OverrideView | client.dll | `0x110BE0` | MOVED | `0x1D40`, `0x1E70`, `0x2770`, `0x6FA0`, `0x77C0`, `0x8AC0`, `0xFF90`, `0x10840` |
| DrawModelExecute | engine.dll | `0x113E80` | MATCH | `CModelRender` IModelRender slot 19 (`+0x4C`), thiscall 3-arg `ret 0xC`. **Not** `0xF6A20` (DispInfo displacement helper; that hang 2026-08-18). Do **not** `FindMaterial` from this detour (2026-08-19 load crash on first eligible `v_*`). Hide arms from `DrawModelState` studiohwdata materials. |
| VGui_Paint | engine.dll | `0x238C50` | MATCH | `0x238C50` |
| GetRenderTarget | materialsystem.dll | `0x68820` | MATCH | `0x68820` |
| GetViewport | materialsystem.dll | `0x68A70` | MATCH | `0x68A70` |
| Viewport | materialsystem.dll | `0x69F30` | MATCH | `0x69F30` |
| PushRenderTargetAndViewport | materialsystem.dll | `0x6A3D0` | MATCH | `0x6A3D0` |
| PopRenderTargetAndViewport | materialsystem.dll | `0x6A250` | MATCH | `0x6A250` |
| ProcessUsercmds | server.dll | `0x5320F0` | MATCH | `0x5320F0` |

# AssetHive UE Plugin

Download the plugin from [Releases](https://github.com/Biscuits-Lab/AssetHivePlugin/releases).

The current source version is **1.2.8**. Extract the AssetHive folder into your project Plugins directory and rebuild it with your project's engine. Close Unreal Editor before replacing an existing installation.

## 1.2.8

- Synchronize existing character assets from AssetHive while retaining their UE paths and compatible skeleton/physics references.
- Reuse explicitly registered shared materials and textures, including references to older copies.
- Fix character material handoff, Blockout identity upgrades, ShadingMask updates and non-cloth LOD generation.
- Character textures default to VT. Zion eye materials use the VT variant while retaining regular height sampling for iris parallax; sync the accompanying project material assets through P4.

The older 1.1.0 release included Windows x64 editor binaries for Unreal Engine 5.5.4 (BuildId 37670630); those binaries do not apply to the current source version.

## 1.2.1

- 3D Plants static meshes consistently use the `SM_Env_Foliage_` naming prefix.

## 1.2.0

- 3D Assets and 3D Plants support mesh-only reimport. It preserves existing material bindings, textures and FoliageType assets. New meshes still receive a full import.
- Zion plant variants can share material instances and textures when their UV3 remapping settings match. Foliage diffuse textures retain their opacity in the alpha channel.

## 1.1.9 foliage changes

- Reuse verified LOD0 wind on ordinary LODs while preserving each LOD's texture UV0.
- Export Billboard only in LOD mode, with separate material instances and texture groups. Billboard keeps Shared only; Micro disables all wind and interaction.
- Support original layer manifests, root contact attenuation and UV3 mode 2. Reject incomplete or stale source bindings.
- Restore float precision splits only when the declared island count and individual bounds remain consistent; replay the same partition during baking.
- Keep measured root volumes fixed after interpolation creates new Opaque or lower-LOD vertices, without changing the original LOD0 weights.
- Use `Op` for exported cut representations and `_Op` for their material instances; continue reading existing library `_OPAQUE` sources.

## Editor connection recovery

Animation exports use `characterAnimationV1`. They open Unreal's native FBX animation import window by default, with animation-only, 60 FPS and Exported Time presets. Existing AnimSequences use the native animation reimporter, preserving references. Root Motion and Force Root Lock follow the desktop options. Disable the native window in the desktop dialog to import with those presets automatically. Missing Skeletons, cancelled/failed imports and save failures return per-animation results. Rebuild the plugin with the target engine to enable this capability.

Zion 3D Assets support named material groups: match authored mesh slot names to
one texture set, create one MI per material identity, and reuse the same MI and
textures across a batch in the same destination category. Missing names and
conflicting sets are reported per asset, without positional fallback. Rebuild
this plugin alongside the desktop change; the hello capability
`zion3dMaterialNames` prevents named jobs from reaching older plugins.

Zion 3D 资产的二级目录接在各自分类目录之后，例如
`/Game/Environment/Asset/Kit/Wall/`、
`/Game/Environment/Asset/Destructible/Wall/`。
`Kit` 和 `Kits` 标签均导出到 `Kit` 文件夹；二级目录留空则直接使用分类目录。

3D Assets 的纹理最大尺寸达到 1024 时即开启 VT（含 ORM），同时遵守工程的
`r.VirtualTextures` 和 `r.VT.EnableAutoImport` 开关；尺寸上限仍按既有分类预设。

Zion 3D Plants 支持按材质名称检测和复用同批纹理套组，并在父材质、采样及透明度等状态一致时复用 MI。
植被纹理和 MI 默认使用 `T_Env_Foliage_<资产名>_<类型>` 和 `MI_Env_Foliage_<资产名>`。Zion 目录、网格和 FoliageType 使用资产名。Zion 材质组的 `opacityInDiffuseAlpha` 表示用颜色图的 A 通道驱动 Masked，植被导出对应的 hello 能力为 `plantExportV2`。
Wind JSON 的明确纹理引用用于识别材质名称，保留树干、枝叶与 Billboard 的原有父材质规则。
共享原生风实例采用按名称自然排序的首个成功变体（优先变体 1）的普通风参数，RippleFlexibility 等差异不阻止复用。
UV3 Remapped 默认按变体创建 MI；全部源 LOD 的 UV3 有效、且最终填入 MI 的七个解码参数及编码约定完全一致时才共享。
不同模型的顶点 UV3 数值可以不同，合批后共用解码参数的同系列变体可共用一个 MI。
没有完整证明时保留独立 MI，相同纹理仍可共享。此路径要求 hello 能力 `zionPlantMaterialReuse`，旧插件在桌面派发前被拦截。

The plugin first tries localhost port 13430, then ports 13431 through 13439 so
multiple editors can run concurrently. If every port is unavailable, startup
retries every two seconds until a listener starts. Successful startup stops the
retry ticker, leaving heartbeat socket work on the background thread.

Update the AssetHive desktop app as well to discover backup ports and match the
selected project. Older desktop versions only connect to port 13430. Each plugin
serves one desktop connection at a time; avoid running an installed desktop app
and a development preview against the same editor simultaneously.

After changing plugin source, rebuild with your engine and restart the editor.
The regression test `AssetHive.Transport.ListenerRecovery` checks exclusive port
selection and recovery when a previously occupied port becomes available.


## Suit import progress

Suit sends keep one parent progress dialog open throughout the import and save batch. Nested UE tasks reuse this dialog; unchanged batches do not open it. Texture settings are applied together only when properties differ, preserving the existing compression and color-space contract. Per-item validation, receipts and import timings remain available. Environment and animation import flows are unchanged.

## Weapon import

Character weapon jobs use `/Game/Characters/Weapon_PJZ/<category>/<identity>/`, with `Material`, `Texture`, and `Mesh/SkeletalMesh` subdirectories. Arrow is an independent asset under Bow. Desktop management retains SM, but UE jobs exclude it.

A bound FBX supplies its own skeleton on first import; the resulting Skeleton uses `SKEL_<identity>` and compatible later imports reuse it. The plugin does not rig meshes or create animation/physics assets. Weapon jobs do not use suit body materials or the suit shared Skeleton. Weapon instances reuse /Game/MaterialLibrary/Weapon/Common/M_Weapon_Slab; no parent material is generated. D/N/ORM/A bind Diffuse/Normal/ORM/OpacityMasked, with Use VT disabled and Use OpacityMasked enabled only when A exists. Missing master materials produce an error. Existing generated-parent instances migrate on their next send; old parent assets are not automatically deleted. Inspection, incremental sends, receipts, and managed deletion recovery use the character job protocol.
Weapon PhysicsAsset policy: create missing SK_<canonical>_Physics beside the skeletal mesh; preserve and reuse existing assigned physics assets, including artist edits. Suit imports are unchanged.

Animation imports validate the whole batch before importing: source files, package paths, Skeletons, destination types, read-only files, dirty existing animations and compression settings. `allowEngineCompressionFallback` permits engine default compression only for imported animations when their compression settings are invalid; project settings are never changed. Success requires valid compressed data and a saved asset file, reports the actual object path, and selects the asset in the Content Browser. P4 checkout remains explicit; the importer does not check out files automatically.

`characterAnimationInspectV1` enables read-only animation version checks using `characterAnimation.inspectOnly`. Inspection reports package existence, the saved package hash and loaded-package dirty state without importing or saving. Import receipts include the source FBX hash and saved package hash so the desktop can distinguish archive updates from Unreal edits for each target project.

After the entire animation batch finishes, the Content Browser focuses and selects all successfully saved animations. Failed or cancelled entries do not affect the final selection. The explicit send action can synchronize a locked browser as well.

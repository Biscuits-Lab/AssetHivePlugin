# AssetHive UE Plugin

Download the plugin from [Releases](https://github.com/Biscuits-Lab/AssetHivePlugin/releases).

Current source version: **1.1.9**. Extract the AssetHive folder into your project Plugins directory and rebuild it with your project's engine. Close Unreal Editor before replacing an existing installation.

The older 1.1.0 release included Windows x64 editor binaries for Unreal Engine 5.5.4 (BuildId 37670630); those binaries do not apply to the current source version.

## 1.1.9 foliage changes

- Reuse verified LOD0 wind on ordinary LODs while preserving each LOD's texture UV0.
- Export Billboard only in LOD mode, with separate material instances and texture groups. Billboard keeps Shared only; Micro disables all wind and interaction.
- Support original layer manifests, root contact attenuation and UV3 mode 2. Reject incomplete or stale source bindings.
- Restore float precision splits only when the declared island count and individual bounds remain consistent; replay the same partition during baking.
- Keep measured root volumes fixed after interpolation creates new Opaque or lower-LOD vertices, without changing the original LOD0 weights.
- Use `Op` for exported cut representations and `_Op` for their material instances; continue reading existing library `_OPAQUE` sources.

## Editor connection recovery

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

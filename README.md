# AssetHive UE Plugin

Download the plugin from [Releases](https://github.com/Biscuits-Lab/AssetHivePlugin/releases).

Version 1.1.0 includes Windows x64 editor binaries built for Unreal Engine 5.5.4 (BuildId 37670630), plugin source and Content. Extract the AssetHive folder into your project Plugins directory. Close Unreal Editor before replacing an existing installation.

Other engine versions require rebuilding from source.

## Editor connection recovery

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
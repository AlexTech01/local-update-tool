# Local Update Tool

Homebrew tool for modded Switch 2s running kaiHEN to allow installing game updates from other consoles through "Match Version With Local Users" that would normally be rejected.

> [!WARNING]
> This tool is intended for transferring update data between 2 consoles you own for games you own for legitimate purposes such as modding and datamining. Please do not use this tool to facilitate piracy.

> [!NOTE]
> If you have existing update data for the game you're trying to perform the transfer for, I recommend performing an encrypted backup of your current update with a tool such as [GameVault](https://github.com/flex36ty/switch2-gamevault) beforehand, so that you can restore it later if you wish. As said above, please do not share your dumps with anyone or download dumps from another person.

## Use cases:

- Transferring newer game updates that would normally require a firmware update (these cannot be launched as of right now, but can still be dumped)
- Transferring older updates (provided you have another console with an older update installed)
- Transferring Switch 1 update data for games with free Switch 2 updates (also possible with [match-version-unlocker](https://github.com/Knight1/match-version-unlocker))

## Usage:

1. Install Local Update Tool by downloading the latest version from Releases and placing it as `/switch/local-update-tool/local-update-tool.nro` on your SD card.
2. On the modded Switch 2, launch Local Update Tool from hbmenu, and create a room for the game you'd like to transfer update data for.
3. On the stock console, press + on the game and navigate to Software Update -> Match Version With Local Users, then join the room created by your Switch 2.
4. On the modded Switch 2, press A to start and begin transferring the update data.

## Building:

Install [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) along with the `switch-zlib` package and `python3` + `make`. Once all dependencies have been installed, build with `./build.sh`.
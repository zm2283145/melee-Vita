Vita pause-button artwork
=========================

The four PNGs in this folder were supplied by the user from the
ConsoleMods Wiki Button Icons collection:
https://consolemods.org/wiki/Category:Button_Icons

Files used: ButtonIcon-PSvita-Bumper_Left.png,
ButtonIcon-PSvita-Bumper_Right.png, ButtonIcon-PSvita-Start.png,
ButtonIcon-PSvita-Select.png.
Credit: ConsoleMods Wiki contributors. The site's general disclaimer says
its media is Creative Commons Attribution unless marked otherwise:
https://consolemods.org/wiki/ConsoleMods_Wiki:General_disclaimer

To regenerate the checked-in Vita-only IA4 texture, first extract GmPause.dat
from your own Melee disc to build-vita/GmPause.dat, then run
`python platforms/vita/generate_pause_exit_prompt.py`. The generator keeps
the stock RESET and RETRY lettering from that archive. Normal builds only
need the checked-in include; other platforms keep their original archive art.

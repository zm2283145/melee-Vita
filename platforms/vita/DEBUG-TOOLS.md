# PS Vita Debug Tools Guide

This guide covers the hidden developer tools included in the optimized PS Vita
Release build. These tools reuse Melee's original developer-menu scenes and
in-match debug functions, with Vita-specific navigation, labels, safety blocks,
and controller mappings.

The feature does not enable DebugNet, GDB, render tracing, or compiler debug
symbols. Unlock state lasts only for the current game process and is cleared
when the game is fully closed.

## Important warnings

- Treat developer scenes as experimental. They were built for internal
  GameCube development, not normal play.
- Back up save data before experimenting, even though known persistent and
  memory-card operations are blocked.
- Start with ordinary fighters, stages, and items. Some unused characters,
  Adventure enemies, test stages, movies, and unusual combinations may not be
  valid in every scene.
- The Vita input backend supplies one human controller, GameCube port 1.
- Do not use D-pad diagonals for debug shortcuts. The debug input filter removes
  conflicting vertical and horizontal D-pad directions.

## Vita-to-GameCube controls

| PS Vita control | GameCube control |
| --- | --- |
| Left stick | Main stick |
| Right stick | C-stick |
| D-pad | D-pad |
| Cross | A |
| Circle | B |
| Square | X |
| Triangle | Y |
| L shoulder | L trigger, fully pressed |
| R shoulder | R trigger, fully pressed |
| Select | Z |
| Start | Start |

In the instructions below, "hold" means keep the modifier pressed first, then
tap the other button. This is the most reliable way to trigger combinations
that distinguish held buttons from newly pressed buttons.

## Unlock and open Debug Tools

1. Start the game normally and open **Options**.
2. Using the D-pad, enter:
   **Up, Up, Down, Down, Left, Right, Left, Right, Select**.
3. A sixth row named **DEBUG TOOLS** appears.
4. Highlight **DEBUG TOOLS** and press Cross.

Only exact digital D-pad presses count. Left-stick movement does not advance the
sequence. A wrong input resets sequence progress. The row remains available
until the process is closed.

Entering Debug Tools temporarily raises Melee to `DebugRom`, which enables its
developer scenes and in-match debug shortcuts. Returning to the normal title or
menu flow restores the previous release state.

## Developer-menu navigation

| Input | Action |
| --- | --- |
| D-pad Up / Down | Move between entries |
| D-pad Left / Right | Decrease/increase the selected value |
| Cross | Open a submenu, run an action, or apply a value |
| Circle | Return to the parent page |
| Start | Run an entry only when the selected page supports it |

The top line is a breadcrumb showing the current page. The bottom line explains
the selected entry. Disabled entries are visually dimmed, show a reason, and
ignore value changes and activation buttons.

## Main Debug Tools pages

### Match Setup

Use **Versus Mode > Match Setup** to configure and launch a debug match.

The page provides submenus for:

- Characters for player slots 1-4
- Player scale
- Human, CPU, demo, or disabled slot control
- Costume and sub-costume indices
- Starting damage
- Attack and defense ratios
- CPU behavior and CPU level
- Team assignment
- Stage
- Melee/match kind

Select **Start Match** after configuring the match.

Recommended first test:

1. Set player 1 to a normal playable character and **Human**.
2. Set player 2 to a normal character and **CPU**, or disable it.
3. Leave players 3 and 4 disabled.
4. Choose a normal multiplayer stage.
5. Leave scale, ratios, costumes, and match kind at ordinary values.
6. Select **Start Match**.

Bosses, wireframes, Sandbag, Popo-only, unused stages, test stages, Adventure
stages, and extreme scale or ratio values are available because the original
developer data exposes them. They are not guaranteed to work in arbitrary
combinations.

### Rules

Use **Versus Mode > Rule** to configure:

- Time, Stock, Coin, or Endless mode
- Minutes and seconds
- Stock count
- Damage ratio
- Rumble settings
- Item frequency

The Vita port currently has no rumble output, so rumble settings do not produce
hardware feedback.

### Result Test

Configures a standalone results-screen test: player characters, ranks, win
states, color, animation stepping, and panel visibility. It does not require a
full match.

### Sound Test

The Vita menu exposes Melee's original Sound Test as a top-level entry. It
provides:

- Sound mode
- Master, sound-effect, and music volumes
- DSP level
- Sound-effect group and sound-effect selection
- Background-music selection

Use D-pad Left/Right to select values. Cross on the sound-effect or
background-music entry plays the selection. Circle stops the active selection
or returns, depending on the current page. Use the explicit **EXIT** entry to
leave the Sound Test.

Very high volume values can be unpleasant through headphones. Begin with the
existing values.

### Test Categories

These are the original developer-specific pages with clearer Vita names:

| Page | Intended use | Vita status |
| --- | --- | --- |
| Match Counts & Players | Character/stage selection and record counters | Navigation works; record-changing fields are blocked |
| Unlocks & Records | Unlock flags, records, coins, and route shortcuts | Persistent mutations are blocked; scene shortcuts remain experimental |
| 1P Route Stage Select | Launch Melee, Adventure, Classic, or All-Star at selected route points | Enabled |
| Movies & Endings | Opening, visual scenes, bonus movie, and ending tests | Enabled, experimental |
| Results, Card & Movies | Results and miscellaneous scene tests | Card and progressive-scan paths are blocked |
| Event & Fixed Camera | Select an event and start its fixed-camera scene | Enabled, experimental |
| Match & Credits | Start a configured match or the credits sequence | Enabled |
| Memory Card & Snapshots | GameCube card and snapshot operations | Blocked |

The historical names shown in older documentation may differ. The Vita
presentation uses functional aliases such as **Match Setup**, **Characters**,
**CPU Behavior**, and **Event & Fixed Camera**.

## In-match debug shortcuts

These shortcuts are active in matches launched while Debug Tools owns the
temporary DebugRom state. They use player 1's Vita controls.

### Quick reference

| Vita input | Function |
| --- | --- |
| D-pad Down alone | Spawn the selected item above player 1 |
| Hold L + D-pad Up/Down | Select next/previous item |
| Hold L + D-pad Right/Left | Select next/previous Pokemon |
| Hold R + D-pad Up | Cycle fighter and item collision displays |
| Hold R + D-pad Right | Cycle player 1 collision display |
| Hold R + D-pad Left | Cycle miscellaneous fighter/item ranges |
| Hold R + D-pad Down | Cycle stage and camera visual layers |
| Hold Triangle + D-pad Down | Toggle fighter animation information |
| Hold Triangle + D-pad Left | Apply Super Mushroom growth |
| Hold Triangle + Cross + D-pad Left | Apply Poison Mushroom shrinking |
| Hold Triangle + D-pad Right | End Super Mushroom growth |
| Hold Triangle + Cross + D-pad Right | End Poison Mushroom shrinking |
| Hold Square + D-pad Down | Cycle HUD/background visual presets |
| Hold Square + D-pad Left | Cycle sound-effect/music debug state |
| Hold Circle + D-pad Down | Toggle CPU/handicap information |
| Hold Circle + D-pad Left | Toggle bonus-score information |
| Hold Cross + D-pad Right | Toggle 5x game speed |
| D-pad Up alone | Cycle normal, free, and fighter-follow debug cameras |
| Start, then hold L + R and press Cross (bottom button) | End the match as No Contest |

### Item and Pokemon spawner

1. Hold L.
2. Tap D-pad Up or Down to choose an item.
3. Tap D-pad Right or Left while still holding L to choose the Pokemon used by
   a Poke Ball.
4. Release L and every other button.
5. Tap D-pad Down by itself.

The selected item appears approximately 60 game units above player 1. The
purple item/Pokemon readout appears when the selection changes and fades after
about two seconds.

Important details:

- D-pad Down spawns only when no other face, shoulder, Start, Select, or
  horizontal D-pad button is held.
- The selected Pokemon is used when a spawned Poke Ball opens; changing the
  Pokemon does not spawn one directly.
- Normal items are the safest choices.
- The list also contains internal enemy and Adventure objects. Some depend on a
  particular stage or mode and may fail, hang, or crash elsewhere.
- Game object and item limits still apply. A spawn may do nothing when a limit
  has been reached.

### Debug camera

From a normal gameplay camera:

- Tap D-pad Up to enter free camera.
- Tap D-pad Up again to follow player 1.
- Tap D-pad Up once more to restore the normal gameplay camera.

While free or follow camera is active:

| Input | Camera action |
| --- | --- |
| Right stick | Orbit |
| Hold D-pad Left + move right stick vertically | Dolly/zoom |
| Hold D-pad Right + move right stick | Pan |

On Vita, moving the right stick does not activate debug camera by itself. The
camera consumes the right stick only after D-pad Up explicitly activates it.
Cycle back to the normal camera to restore ordinary C-stick attacks.

The separate **Camera Mode** scene also uses Vita controller port 1. Its
original GameCube implementation expected controller port 4. Circle backs out
of its current camera UI, Cross confirms, Select enters its snapshot UI, and
D-pad Left/Right changes choices where shown. GameCube snapshot storage itself
remains unsupported on Vita.

### Collision and visualization tools

**Hold R + D-pad Up** reaches two original handlers and therefore cycles both
fighter and item collision visualization. Repeated presses rotate through the
available collision-display modes.

**Hold R + D-pad Right** changes player 1's fighter collision display without
changing every fighter. **Hold R + D-pad Left** cycles miscellaneous fighter
visuals, including internal pickup/stomp/coin range flags where applicable.

**Hold R + D-pad Down** cycles stage rendering presets, including combinations
of shadows, stage layers, and camera-related visuals. **Hold Square + D-pad
Down** cycles another combined HUD/background visualization state. Repeatedly
use the same chord to return to the normal presentation.

### Fighter and match information

- **Triangle + D-pad Down** toggles an overlay containing each fighter's motion,
  submotion, animation frame, and state flags.
- **Circle + D-pad Down** toggles player-slot, CPU type/level, handicap, and
  attack/defense ratio data.
- **Circle + D-pad Left** toggles live bonus and score information for player 1.
- **Square + D-pad Left** cycles four sound-effect/music enable combinations.
  The sound diagnostic overlay appears during part of that cycle.

### Size and speed tools

Hold Triangle and tap D-pad Left to apply a Super Mushroom size effect; add
Cross to apply the Poison Mushroom effect instead. Use the corresponding
D-pad Right combinations to remove the effects.

**Cross + D-pad Right** toggles 5x game speed. Toggle it off before leaving a
test if normal timing is needed.

## Pause, retry, exit, and recovery

To leave a running debug match:

1. Press Start to pause.
2. Hold L and R.
3. Press Cross, the bottom face button, while both shoulders remain held.

In DebugRom, Melee's No Contest combination is GameCube L + R + A. Unlike the
retail combination, Start is not part of the held chord after pausing. The Vita
port permits this escape for every match launched through the hidden Debug
Tools route, even when the selected developer scenario does not set Melee's
normal No Contest permission flag.

While paused, Select requests Retry in modes that support it.

If the camera is still detached, tap D-pad Up until the normal gameplay camera
returns. If a scene stops responding, close the application from the Vita
system UI and relaunch it. A full close also clears the Debug Tools unlock and
restores the normal release state.

## Deliberately unavailable functions

The Vita safety policy blocks these entries before initialization and callback
execution:

- Language and publicity persistence
- Changing the global debug level
- Arbitrary global-data editing
- Saved match-count and player-count records
- Coins, trophies, event unlocks, and other persistent unlock/record mutations
- GameCube memory-card formatting, creation, deletion, and snapshot operations
- Unsupported GameCube hardware paths, including progressive-scan tests

Two original `Develop`-level allocation/item-state shortcuts are also inactive
because the hidden Vita route deliberately uses `DebugRom`, not the more
invasive `Develop` level.

The original **Triangle + D-pad Up** engine screenshot shortcut targets a
GameCube USB path and is not useful on Vita. Use the Vita system screenshot
facility instead.

## Source reference

The principal implementations are:

- `src/melee/if/soundtest.c`: developer-menu descriptors and Vita safety policy
- `src/melee/if/textlib_1.c`: Vita navigation, aliases, breadcrumbs, and help
- `src/melee/db/dbitem.c`: item/Pokemon selection and spawning
- `src/melee/db/dbcamera.c`: debug camera and stage-visual controls
- `src/melee/db/dbanim.c`: collision, animation, and size tools
- `src/melee/db/dbcpu.c`: CPU/handicap information
- `src/melee/db/dbsound.c`: in-match sound controls
- `src/melee/db/dbbonus.c`: bonus-score information
- `src/melee/db/dbscreenshot.c`: 5x speed and legacy screenshot handling
- `src/melee/gm/gmvs.c`: pause, retry, and No Contest handling
- `platforms/vita/game/pad.c`: Vita-to-GameCube input mapping

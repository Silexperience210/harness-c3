# λ Harness C3

[![build](https://github.com/Silexperience210/harness-c3/actions/workflows/build.yml/badge.svg)](https://github.com/Silexperience210/harness-c3/actions/workflows/build.yml)
[![web flasher](https://img.shields.io/badge/web_flasher-esp--web--tools-blue)](https://silexperience210.github.io/harness-c3/webflasher/)

A USB companion dial for the [Harness](https://github.com/autonomous-ai/openharness)
daemon, ported from the reference ESP32-S3 dial to the low-cost
**ESP32-2424S012C / ESP32-2424S012C-I** (ESP32-C3-MINI-1U + 1.28″ GC9A01
240×240 round IPS + CST816D capacitive touch). Plug it into the computer
running the daemon: it shows what each coding agent is doing, lets you answer
their questions with a tap, stop a runaway turn, and scroll the window.

*Section française plus bas.* 🇫🇷

## What it does

- **Agent carousel** — a status ring per agent (grey idle / blue working /
  amber waiting / green done / red error), name, engine · machine, the live
  status line, page dots and the account-wide fleet badge.
- **Questions** (AskUserQuestion) take the face: tap an option (or several
  for multi-select), then ✓. Dismiss with ✕ or a sideways swipe — the
  question stays pending and an amber chip on the home screen brings it
  back. Up to three questions queue; several items per request are walked
  one by one.
- **Stop** a running turn (tap *Stop*, then *Confirm?*) — `turn.stop`.
- **Scrollpad** — the dial becomes a touchpad that scrolls the computer's
  window (`scroll` with fling velocity).
- **Finished turns** and **errors** pop up as toasts (the board has no
  buzzer); the screen dims after 60 s, switches off after 10 min, and a
  question always wakes it. A tap on a dark screen only wakes it.
- **Settings** (pull down): brightness (kept in NVS), firmware / touch chip,
  free RAM, link error counters.
- Speaks the upstream **cable** protocol (binary framing + JSON vocabulary)
  over the ESP32-C3's native USB — no Wi-Fi, no account, no pairing.

| Gesture | Action |
|---|---|
| Swipe ← / → (or tap ‹ ›) | next / previous agent (sends `focus`) |
| Tap the agent card | open it on the computer (`agent.open`) |
| Pull ↓ / push ↑ | settings / scrollpad |
| BOOT short / long | back · open a waiting question / screen off–on |

## Hardware

Board **ESP32-2424S012C** (sold in a case as **-C-I**). Everything is on the
board; the pin map is the default and needs no wiring:

| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| LCD SCK / MOSI | 6 / 7 | Touch SDA / SCL | 4 / 5 |
| LCD CS / DC | 10 / 2 | Touch INT / RST | 0 / 1 |
| LCD RST | tied to EN (SW reset) | BOOT button | 9 |
| Backlight (PWM) | 3 | USB D− / D+ | 18 / 19 |

> [!IMPORTANT]
> Use a **USB-A → USB-C** data cable. The board has no CC resistors, so a
> USB-C ↔ USB-C cable does not power it. If the port does not appear for
> flashing, hold **BOOT** while plugging in.

A hand-wired build (bare ESP32-C3 + GC9A01 breakout + two buttons, optional
buzzer) is still supported: `menuconfig → Harness C3 Configuration → Board →
Custom wiring`, then set the pins.

## Flashing

**Web flasher (easiest):** open
<https://silexperience210.github.io/harness-c3/webflasher/> in Chrome/Edge,
click *Connect & Flash*. CI keeps it on the latest `main` build.

**esptool:** grab `harness-c3-merged.bin` from
[Releases](https://github.com/Silexperience210/harness-c3/releases) (or from
the `firmware-bins` artifact of any CI run):

```sh
esptool.py --chip esp32c3 write_flash 0x0 harness-c3-merged.bin
```

**From source (ESP-IDF v5.5):**

```sh
cd firmware
idf.py set-target esp32c3
idf.py build flash monitor
```

### First flash: orientation check

The GC9A01 driver's scan direction differs between panel batches. If the
picture is mirrored or upside down, toggle *Display → Mirror X / Mirror Y*
in menuconfig; if taps land in the wrong place, apply the same transform in
*Touch → mirror / swap*. Both are logged at boot (`display:` / `touch:`).

## Configuration (menuconfig → *Harness C3 Configuration*)

Board, every pin, SPI clock (80 MHz on this board), panel orientation /
colour order, touch transforms, default brightness, dim / off delays, UI
language (Français / English), max agents held.

## Development

- `firmware/` — ESP-IDF project (target `esp32c3`, dual-OTA 4 MB, LVGL
  **9.2.2** and `esp_lcd_gc9a01` **2.0.4** pinned, `dependencies.lock`
  committed).
- `firmware/test/host/run_tests.sh` — protocol tests on your laptop: every
  shared framing vector, golden JSON for every message, struct-layout check.
- `firmware/test/sim/run_sim.sh` — **UI simulator**: the real `ui.c` +
  `cable_client.c` + LVGL on a virtual round panel, driven by a scripted
  daemon and finger (72 checks); writes a PNG per screen and a contact sheet
  (`out/screens.png`). Needs gcc, python3, Pillow.
- `firmware/scripts/gen_fonts.sh` — regenerates the UI fonts (Latin-1,
  French typography, λ, LVGL symbols) with `lv_font_conv`.
- `PROTOCOL.md` — the normative cable protocol. `SPEC.md` — this port.

## Limits

- No voice (no mic), no machine wheel / swarms / model picker — the
  carousel covers the window's active tab.
- Free-text questions (no options) must be answered on the computer.
- **`fw.offer` is never answered** (anti-brick, SPEC §9): upstream images
  target ESP32-S3. Dual-OTA partitions and rollback are in place for a
  future C3-aware updater; until then, use the web flasher or esptool.
- Not yet validated on every panel batch — see *orientation check* above.

## Credits

Protocol implementation, framing code, test vectors and the protocol
specification are adapted from
[autonomous-ai/openharness](https://github.com/autonomous-ai/openharness)
(MIT) — see `LICENSE`. Fonts: Montserrat and Font Awesome (SIL OFL 1.1),
DejaVu Sans (λ) — see `firmware/main/fonts/README.md`.

---

# λ Harness C3 — en français

## C'est quoi ?

Un cadran USB compagnon pour le daemon
[Harness](https://github.com/autonomous-ai/openharness), sur la carte
**ESP32-2424S012C / ESP32-2424S012C-I** (ESP32-C3-MINI-1U + écran rond
GC9A01 1,28″ 240×240 + tactile capacitif CST816D). Branché sur l'ordinateur
qui fait tourner le daemon, il montre ce que fait chaque agent de code, et
permet de répondre à ses questions d'un tap, d'arrêter un tour en cours et de
faire défiler la fenêtre.

- **Carrousel d'agents** : anneau de statut coloré (gris inactif / bleu en
  cours / ambre en attente / vert terminé / rouge erreur), nom, moteur ·
  machine, ligne de statut en direct, points de pagination, badge du total
  de la flotte.
- **Questions** : elles prennent l'écran ; touchez une option (ou plusieurs
  en choix multiple) puis ✓. ✕ ou un swipe latéral = « plus tard » : la
  question reste en attente et une pastille ambre sur l'accueil la rouvre.
  Jusqu'à 3 questions en file.
- **Stop** d'un tour en cours (touchez *Stop* puis *Confirmer ?*).
- **Pavé de défilement** (poussez vers le haut) : l'écran devient un
  touchpad qui fait défiler la fenêtre de l'ordinateur.
- Tours terminés et erreurs en notifications (pas de buzzer sur la carte) ;
  l'écran s'atténue après 60 s, s'éteint après 10 min, une question le
  rallume. Un toucher sur écran éteint ne fait que le réveiller.
- **Réglages** (tirez vers le bas) : luminosité mémorisée, version, puce
  tactile, RAM libre, compteurs d'erreurs du lien.

| Geste | Action |
|---|---|
| Swipe ← / → (ou ‹ ›) | agent suivant / précédent |
| Toucher la carte de l'agent | l'ouvrir sur l'ordinateur |
| Tirer ↓ / pousser ↑ | réglages / pavé de défilement |
| BOOT court / long | retour · ouvrir la question en attente / écran on–off |

## Matériel et câble

Carte **ESP32-2424S012C(-I)** : aucun câblage, le brochage ci-dessus est
celui par défaut. **Câble USB-A → USB-C obligatoire** (pas de résistances CC
sur la carte : un câble USB-C ↔ USB-C ne l'alimente pas). Si le port
n'apparaît pas pour flasher : maintenir **BOOT** en branchant. Un montage
maison (module nu + écran GC9A01 + 2 boutons) reste possible :
*menuconfig → Harness C3 Configuration → Board → Custom wiring*.

## Flasher

- **Web flasher** : <https://silexperience210.github.io/harness-c3/webflasher/>
  dans Chrome/Edge → *Connect & Flash*.
- **esptool** : `esptool.py --chip esp32c3 write_flash 0x0 harness-c3-merged.bin`
  (binaire sur la page Releases ou dans les artefacts CI).
- **Depuis les sources** : `cd firmware && idf.py set-target esp32c3 && idf.py
  build flash monitor` (ESP-IDF v5.5).

**Premier flash** : si l'image est en miroir ou à l'envers, basculez
*Display → Mirror X / Mirror Y* dans menuconfig ; si les touchers tombent à
côté, appliquez la même transformation dans *Touch*.

## Limites

Pas de voix, pas de molette machines / swarms / modèles ; les questions à
réponse libre se traitent sur l'ordinateur ; les mises à jour firmware
proposées par le daemon (images ESP32-S3) sont **toujours ignorées** (risque
de brick — SPEC §9) : mise à jour via le web flasher ou esptool.

## Crédits

Protocole, tramage, vecteurs de test et spécification adaptés de
[autonomous-ai/openharness](https://github.com/autonomous-ai/openharness)
(MIT) — voir `LICENSE`.

# harness-wrap — rendre visible sur le cadran tout agent lancé à la main

Le daemon Harness n'adopte que les panes tmux dont la **session commence par
`harness-`** (vérifié le 02/10/2026 : même `claude`, session `dev-test-…` jamais
adoptée, renommée `harness-…` adoptée en 3 s). Un `claude` lancé directement dans
ptyxis (`ptyxis → bash → claude`, sans tmux) reste donc invisible : la lampe ne
réagit pas, et ce n'est pas une panne.

`harness-wrap` règle ça : quand **vous** tapez `claude` (ou `hermes`, `codex`…) dans
un terminal, la commande part dans une session tmux `harness-<moteur>-<dossier>`,
votre terminal s'y attache, et le cadran affiche la tuile dès le premier tour.

## Fichiers

| Fichier | Rôle |
|---|---|
| `~/.hermes/scripts/harness-wrap` | le lanceur |
| `~/.hermes/scripts/harness-wrap.sh` | fonctions `claude`, `hermes`… (un par moteur installé) + `harness_wrap` générique, sourcé par `~/.bashrc` |
| `~/.hermes/scripts/harness-wrap.engines` | table « exécutable → moteur » extraite de `~/.harness/cli/cli.js`, régénérée quand le daemon est mis à jour |
| `~/.hermes/scripts/test-harness-wrap.sh` | batterie de tests (faux moteur, serveur tmux privé, coût nul) |

Ligne ajoutée à `~/.bashrc` (sauvegarde : `~/.bashrc.avant-harness-wrap-<date>`) :

```sh
# harness-wrap : agents interactifs visibles sur le cadran (doc : ~/.hermes/scripts/harness-wrap.md)
[ -f "$HOME/.hermes/scripts/harness-wrap.sh" ] && . "$HOME/.hermes/scripts/harness-wrap.sh"
```

## Ce qu'il fait, cas par cas

| Situation | Comportement |
|---|---|
| stdin **ou** stdout pas un terminal (tube, fichier, cron, script) | `exec` du vrai binaire : sortie et code identiques, rien d'affiché |
| `-p`, `--print`, `--output-format`, `--input-format`, `--no-session-persistence`, `--json`, `--bg`, `-h`, `--version`… | idem |
| sous-commandes (`claude mcp`, `claude update`, `hermes gateway`, `hermes cron`, `hermes -z`, `hermes chat -q … --oneshot`, `codex exec`, `opencode run`…) | idem |
| `HARNESS_WRAP=0` | idem |
| appelé par un agent (`CLAUDECODE`, `HERMES_SESSION_KEY` ou `HERMES_AGENT` présent) | idem — `HARNESS_WRAP=1` force l'enveloppe |
| déjà dans une session `harness-*` (dont celles du daemon) | idem — jamais de tmux dans le tmux |
| hors tmux, interactif | crée `harness-<moteur>-<dossier>` et s'y attache ; une ligne sur stderr l'annonce |
| dans un autre tmux (session `dev`, etc.) | crée la session `harness-*` à côté et y bascule ; à la fin, retour à la session d'origine. La session d'origine n'est **ni renommée ni modifiée** |
| nom déjà pris | suffixe horodaté `-AAAAMMJJ-HHMMSS`, jamais d'écrasement ni de jonction |
| tmux absent, cassé, création refusée | lance la commande normalement + un avertissement (une fois par démarrage) |
| tmux d'une autre socket que celle du daemon | lance normalement + un avertissement |

Le moteur garde votre environnement, votre dossier courant et vos arguments ; son
code de sortie vous est rendu. S'il sort en erreur, les dernières lignes de son
écran sont recopiées sur stderr (la session tmux disparaît avec lui).

- Détacher sans arrêter : `Ctrl-b d` — le lanceur rend la main et indique
  `tmux attach -t <session>` pour y revenir.
- Session tuée de l'extérieur : le lanceur rend la main avec le code 129.

## Désactiver

- pour une commande : `HARNESS_WRAP=0 claude …`, ou `command claude …`, ou `\claude …`
- pour la coquille en cours : `unset -f claude hermes`
- n'importe quelle commande, explicitement : `harness_wrap aider …`

## Désinstaller

```sh
~/.hermes/scripts/harness-wrap --uninstall   # retire les 2 lignes (sauvegarde datée avant)
```

puis ouvrir un nouveau terminal. Les fichiers de `~/.hermes/scripts/` peuvent rester ;
sans la ligne de source, ils ne font rien. Réinstaller : `harness-wrap --install`
(idempotent ; régénère aussi la table des moteurs).

## Commandes utiles

```sh
~/.hermes/scripts/harness-wrap --engines          # moteurs installés et reconnus
~/.hermes/scripts/harness-wrap --refresh-engines  # relire cli.js après une mise à jour du daemon
~/.hermes/scripts/test-harness-wrap.sh            # rejouer la batterie (≈ 1 min, ne coûte rien)
```

## Limites connues

- **Application de bureau Claude Code, extensions IDE (VS Code, JetBrains),
  claude.ai/code** : elles ne passent pas par un terminal bash, donc pas par la
  fonction. Elles restent invisibles sur le cadran.
- **Lancements depuis l'interface graphique** (raccourci `.desktop`, menu GNOME,
  lanceur d'applications) : pas de `~/.bashrc` interactif, pas d'enveloppe.
  Contournement : faire pointer le raccourci vers `~/.hermes/scripts/harness-wrap claude`
  (à l'intérieur d'un terminal).
- **Chemin absolu** (`~/.local/bin/claude`) ou `command claude` : contourne la
  fonction, volontairement.
- **Coquilles non interactives** (`bash -c`, scripts, cron) : `~/.bashrc` y sort
  avant la ligne de source ; aucune fonction n'est définie. Pour envelopper partout,
  y compris par le PATH, un lien `claude → harness-wrap` placé en tête du PATH
  fonctionne aussi (le lanceur retrouve alors le vrai binaire plus loin dans le PATH),
  mais n'est pas installé par défaut.
- **zsh** : `harness-wrap.sh` est écrit pour bash et zsh, mais zsh n'est pas installé
  ici ; `--install` traitera `~/.zshrc` s'il apparaît.
- **Agent lancé par un agent** (Hermes en pseudo-terminal, Claude Code) : pass-through
  par choix, la tuile n'apparaît pas. `HARNESS_WRAP=1` pour forcer.
- **`hermes-agent`** : point d'entrée d'automatisation, pas de fonction.
- **Écran après la sortie** : comme la session tmux se ferme avec le moteur, l'écran
  final disparaît (seules les dernières lignes sont recopiées en cas d'erreur).
- **Tuile après un `kill-session`** : le daemon garde la tuile ; la retirer avec
  `node ~/.hermes/cache/scratch/harness-ws-probe.mjs agent_delete '{"agentId":"…"}'`.
- Le nom de session ne sert qu'à l'adoption : le daemon reconnaît le moteur au **nom
  d'exécutable** des processus du pane (table extraite de `cli.js`).

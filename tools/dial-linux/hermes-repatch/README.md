# Réapplication des correctifs Hermes sur le CLI Harness (`repatch-hermes.py`)

Le CLI Harness **se met à jour tout seul**, et chaque mise à jour écrase nos correctifs locaux.
Ce dossier est la **source de vérité** des deux pièces qui les remettent en place ; elles sont
aussi installées sur la machine, et **il faut corriger les deux copies** (même règle que pour
`harness-wrap` — les chemins sont auto-relatifs, aucune ne présuppose l'autre).

| fichier | installé en | rôle |
|---|---|---|
| `repatch-hermes.py` | `~/.harness/repatch-hermes.py` | réapplique les correctifs dans `~/.harness/cli/cli.js` (idempotent, repérage par regex car les noms minifiés changent à chaque version, sauvegarde `.avant-repatch`, `node --check`, retour arrière) |
| `harness_repatch.sh` | `~/.hermes/scripts/harness_repatch.sh` | veilleur cron `no_agent` (toutes les 5 min) : muet si tout est en place, sinon réapplique + redémarre `harness.service` + une ligne de rapport. **Code 2 = motif amont changé, à regarder.** |

Correctifs portés : `383` (`harness new hermes` — **fusionné en amont** via la PR #412, ne sert
plus que de garde-fou), `lock` (questions `clarify` visibles au cadran), `prompt`
(`FIRST_PROMPT_ARGS.hermes = ['chat','-q']`).

## Le cas du 07/10/2026 : un correctif qui devient inutile

Le CLI est passé de 0.3.26 à **0.3.64**. Le motif du pied de question
`enter to (select|confirm|submit|lock)` est désormais **livré par l'amont** (c'est la partie
« pied » de la PR #410) et le marqueur `▸` a disparu du texte de la question.

Conséquence : la moitié du correctif `lock` qui retirait ce marqueur n'avait plus de cible et
échouait — le veilleur criait « extraction de la question introuvable » **toutes les 5 minutes**
alors que rien n'était cassé. Corrigé en ne lançant le retrait **que si le marqueur existe**.

Leçon à garder : **un correctif qui échoue n'est pas toujours une régression — il peut être
devenu inutile parce que l'amont l'a repris.** Avant de « réparer », vérifier que le motif visé
n'est pas déjà livré par le nouveau code.

## Vérifier que les questions remontent vraiment au cadran

Après un changement de correctif et un redémarrage de `harness.service`, la preuve se lit dans
le journal du daemon, pas dans le code :

```bash
tmux new-session -d -s harness-test-lock
tmux send-keys -t harness-test-lock 'cd ~ && hermes chat -q "Appelle l'"'"'outil clarify avec la question « Test cadran » et deux options A et B."' Enter
journalctl --user -u harness.service --since '-2 min' | grep '\[question\]'
# attendu :  [question] <session> asking the user · "Test cadran" · req=q_…
tmux kill-session -t harness-test-lock
```

Le daemon met ~10 s à publier la question. La tuile de test disparaît d'elle-même et le cadran
revient à son compte d'agents normal. Mesuré le 07/10/2026 : question poussée,
`agents → 2 of 2` après nettoyage.

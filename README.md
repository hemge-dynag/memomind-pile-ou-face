# MemoMind — Pile ou Face (pile-ou-face)

Application **autonome** pour lunettes MemoMind : lancer une pièce par un geste de tête, avec animation du résultat (pile ou face).

## Composition

| Côté | Dossier | Artefact |
| --- | --- | --- |
| Lunettes (plugin natif C) | `glass/pile-ou-face/` | `builds/pile-ou-face.gmp` |

Pas de composant téléphone (app autonome).

## Fonctionnement

- Geste de tête pour déclencher le lancer.
- Animation de la pièce puis résultat (pile / face) affiché via LVGL.

## Build

Nécessite le SDK officiel MemoMind (`memomind-open/plugin-open-platform`). Déposer `glass/pile-ou-face/` dans `GlassSDK/examples/`, puis :

```
python3 build.py glass --force
```

→ `builds/pile-ou-face.gmp`

# Protocole téléphone → écran

Le téléphone (appli Android) pousse l'affichage vers l'ESP32 en HTTP sur le Wi-Fi local.
L'ESP32 s'annonce en mDNS sous `nowplaying.local` (service `_nowplaying._tcp`, port 80).

## Authentification

Toutes les routes `POST` exigent l'en-tête `X-Token: <jeton>`. Le jeton est défini dans
`menuconfig` côté firmware et dans les réglages de l'appli. Sans jeton valide : `401`.
Si le firmware est compilé sans jeton, toutes les requêtes `POST` sont refusées (`503`).

## Géométrie

L'écran physique fait 800×480 (paysage) mais il est utilisé en portrait (480×800).

Le téléphone compose l'image en **portrait 480×800**, puis la tourne pour obtenir un JPEG
**800×480**. Le sens de rotation est donné par l'en-tête `X-Rotation` de `/frame` :

| `X-Rotation` | Rotation appliquée par le téléphone | Point portrait `(px, py)` → point écran `(x, y)` |
|---|---|---|
| `90` (défaut) | 90° sens horaire (`Matrix.postRotate(90f)`) | `x = 799 - py`, `y = px` |
| `270` | 90° sens antihoraire (`Matrix.postRotate(270f)`) | `x = py`, `y = 479 - px` |

On choisit l'un ou l'autre selon le sens dans lequel le boîtier est posé.

### Barre de progression

La barre est dessinée par l'ESP32, pas par le téléphone. Elle occupe un rectangle fixe en
**coordonnées portrait** :

```
BAR_X = 40, BAR_Y = 720, BAR_W = 400, BAR_H = 8
```

Le téléphone peut dessiner ce qu'il veut dans ce rectangle, l'ESP32 le recouvre entièrement :
la partie écoulée avec la couleur `X-Bar-Fg`, le reste avec `X-Bar-Bg`.

## Routes

### `POST /frame`

Nouvelle image, envoyée à chaque changement de morceau (et de nouveau si la pochette change
pour le même morceau).

En-têtes :

| En-tête | Obligatoire | Contenu |
|---|---|---|
| `X-Token` | oui | jeton |
| `Content-Type` | non | `image/jpeg` |
| `X-Track-Id` | oui | identifiant opaque du morceau, 64 caractères max |
| `X-Rotation` | non | `90` ou `270`, défaut `90` |
| `X-Bar-Fg` | non | couleur de la partie écoulée, `RRGGBB` en hexadécimal, défaut `FFFFFF` |
| `X-Bar-Bg` | non | couleur du reste de la barre, `RRGGBB`, défaut `404040` |

Corps : JPEG **baseline** (pas progressif) de 800×480 exactement, 512 Ko max.

Réponses : `204` affiché, `400` en-tête manquant ou JPEG illisible ou mauvaise taille,
`401` jeton, `413` trop gros.

L'image est affichée d'un coup (double buffer). Le rétroéclairage est rallumé s'il était éteint.
La position de lecture est remise à zéro et la lecture considérée en pause jusqu'au prochain `/state`.

### `POST /state`

État de lecture, envoyé juste après `/frame`, puis au play, à la pause, au changement de
vitesse et à chaque déplacement dans le morceau.

```json
{
  "track_id": "…",
  "playing": true,
  "position_ms": 83000,
  "duration_ms": 215000,
  "speed": 1.0
}
```

`position_ms` est la position **au moment de l'envoi** (le téléphone l'extrapole depuis
`PlaybackState.getLastPositionUpdateTime()`). L'ESP32 interpole ensuite
`position_ms + (maintenant - réception) * speed` tant que `playing` est vrai.
`speed` est facultatif (défaut `1.0`).

Réponses : `204`, `400` JSON invalide, `401` jeton,
`409` si `track_id` ne correspond pas à l'image affichée (par exemple après un redémarrage de
l'ESP32) : le téléphone doit renvoyer `/frame` puis `/state`.

### `POST /off`

Le UGREEN s'est déconnecté : l'ESP32 éteint le rétroéclairage et oublie le morceau courant.
Corps vide. Réponse `204`.

### `GET /status`

Sans jeton, pour le débogage : JSON avec le morceau courant, l'état, la mémoire libre, le RSSI
et l'uptime.

```sh
curl http://nowplaying.local/status
curl -X POST -H "X-Token: $TOKEN" -H "X-Track-Id: test" --data-binary @frame.jpg http://nowplaying.local/frame
curl -X POST -H "X-Token: $TOKEN" -d '{"track_id":"test","playing":true,"position_ms":0,"duration_ms":180000}' http://nowplaying.local/state
curl -X POST -H "X-Token: $TOKEN" http://nowplaying.local/off
```

# Protocole téléphone → écran

Le téléphone (appli Android) pousse l'affichage vers l'ESP32 en HTTP sur le Wi-Fi local.
L'ESP32 s'annonce en mDNS sous `nowplaying.local` (service `_nowplaying._tcp`, port 80).
Il n'a qu'une radio 2,4 GHz : le téléphone peut être sur une autre bande tant qu'il est sur le
même réseau local, sans isolation des clients.

## Authentification

Toutes les routes `POST` exigent l'en-tête `X-Token: <jeton>`. Le jeton est défini dans
`menuconfig` côté firmware et dans les réglages de l'appli. Sans jeton valide : `401`.
Si le firmware est compilé sans jeton, toutes les requêtes `POST` sont refusées (`503`).

## Image

L'écran physique fait 800×480 (paysage) mais il est utilisé en portrait (480×800).
L'ESP32 affiche l'image telle quelle : tout le rendu est fait par le téléphone.

Le téléphone compose l'écran en **portrait 480×800**, le tourne de **90° dans le sens horaire**
(`Matrix.postRotate(90f)`), puis l'encode en JPEG **baseline** (pas progressif) de **800×480**.
Ce sens a été vérifié sur le boîtier : le coin haut-gauche du portrait apparaît bien en haut à gauche.

### Mise en page (portrait 480×800)

| Zone | Rectangle portrait | Contenu |
|---|---|---|
| Pochette | `x 0–480, y 0–480` | pochette carrée, pleine largeur, sans marge |
| Texte | `x 0–480, y 480–800` | titre puis artiste, centrés horizontalement, sur le fond coloré (`androidx.palette`) |

Valeurs validées sur l'écran (image de test du 5 octobre) :

| Élément | Police | Couleur | Position |
|---|---|---|---|
| Titre | gras, 38 px | blanc | centré sur `x = 240`, ligne de base `y = 590` |
| Artiste | normal, 30 px | `RGB(205, 212, 230)` sur le fond de test | centré sur `x = 240`, ligne de base `y = 645` |
| Fond texte | | couleur tirée de la pochette (`RGB(38, 52, 92)` pour le test) | `y 480–800` |

Un titre ou un artiste trop long passe sur deux lignes, puis est tronqué avec « … ».

## Routes

### `POST /frame`

Nouvelle image, envoyée à chaque changement de morceau (et de nouveau si la pochette change
pour le même morceau).

| En-tête | Obligatoire | Contenu |
|---|---|---|
| `X-Token` | oui | jeton |
| `Content-Type` | non | `image/jpeg` |
| `X-Track-Id` | oui | identifiant opaque du morceau, 64 caractères max |

Corps : JPEG baseline 800×480, 512 Ko max.

Réponses : `204` affiché, `400` en-tête manquant ou JPEG illisible ou mauvaise taille,
`401` jeton, `413` trop gros.

L'image est affichée d'un coup (double buffer, bascule au VSYNC). Le rétroéclairage est
rallumé s'il était éteint.

### `POST /state`

État de lecture, envoyé juste après `/frame`, puis au play et à la pause. Il sert surtout à
détecter que l'ESP32 a perdu l'image (redémarrage, coupure de courant).

```json
{ "track_id": "…", "playing": true }
```

Les autres champs éventuels sont ignorés.

Réponses : `204`, `400` JSON invalide, `401` jeton,
`409` si `track_id` ne correspond pas à l'image affichée : le téléphone doit renvoyer `/frame`
puis `/state`.

### `POST /off`

Le UGREEN s'est déconnecté : l'ESP32 éteint le rétroéclairage et oublie le morceau courant.
Corps vide. Réponse `204`.

### `GET /status`

Sans jeton, pour le débogage : JSON avec le morceau courant, l'état de lecture, le RSSI,
l'uptime et la mémoire libre.

```sh
curl http://nowplaying.local/status
curl -X POST -H "X-Token: $TOKEN" -H "X-Track-Id: test" --data-binary @frame.jpg http://nowplaying.local/frame
curl -X POST -H "X-Token: $TOKEN" -d '{"track_id":"test","playing":true}' http://nowplaying.local/state
curl -X POST -H "X-Token: $TOKEN" http://nowplaying.local/off
```

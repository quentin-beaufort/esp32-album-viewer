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

### Pixels non carrés

Les pixels de la dalle ne sont pas carrés : 0,1188 mm × 0,1122 mm (zone active 95,04 × 53,86 mm).
En portrait, un pixel est donc 6 % plus haut que large, et un carré de 480×480 pixels apparaît
étiré en hauteur (53,9 × 57,0 mm).

Le téléphone compose donc l'écran en **unités carrées**, sur une hauteur de
`800 × 0,1188 / 0,1122 ≈ 847`, puis le resserre verticalement dans les 800 pixels
(`Canvas.scale(1, 800 / 847)`). Les positions ci-dessous sont en unités carrées.

### Mise en page (portrait 480×847 unités carrées)

| Zone | Rectangle | Contenu |
|---|---|---|
| Pochette | `x 0–480, y 0–480` | pochette carrée, pleine largeur, sans marge |
| Texte | `x 0–480, y 480–847` | titre, artiste puis artistes invités, centrés horizontalement, sur le fond coloré (`androidx.palette`) |

| Élément | Police | Couleur | Position |
|---|---|---|---|
| Titre | gras, 38 | blanc sur fond sombre | centré sur `x = 240`, ligne de base `y = 596` |
| Artiste | normal, 30 | couleur du titre mêlée à 20 % de fond | centré sur `x = 240`, ligne de base `y = 654` |
| Invités | normal, 24, « feat. A, B » | couleur du titre mêlée à 35 % de fond | sous l'artiste, à la moitié de l'écart titre-artiste |
| Fond texte | | couleur tirée de la pochette, `RGB(38, 52, 92)` à défaut | `y 480–847` |

Ces valeurs reprennent l'image de test validée le 5 octobre (lignes de base 590 et 645 en pixels),
avec la même distance physique sous la pochette.

Un titre ou un artiste trop long passe sur deux lignes, puis est tronqué avec « … ». Avec la
ligne des invités, le bloc de texte reste centré sur le même milieu.

Les invités sont les artistes du morceau selon l'API publique de Deezer
(`https://api.deezer.com/track/{id}`, champ `contributors`), moins l'artiste de la session
multimédia. L'identifiant vient de l'identifiant de média de Deezer, `0.{id}`. Sans réponse en
3 secondes, l'image part sans cette ligne.

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

État de lecture, envoyé juste après `/frame`, au play et à la pause, et répété toutes les
15 secondes tant qu'une image doit être affichée. Il sert surtout à détecter que l'ESP32 n'a pas
l'image voulue (redémarrage, coupure de courant, envoi de `/frame` interrompu) : sans cette
répétition, l'écran resterait noir ou sur l'ancien titre jusqu'au morceau suivant. Le `track_id`
envoyé est toujours celui de la dernière image voulue, jamais d'une image plus ancienne.

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

### `GET /status` (et `GET /`)

Sans jeton, pour le débogage : JSON avec le morceau courant, l'état de lecture, le RSSI,
l'uptime, la raison du dernier redémarrage (`reset_reason`), le nombre de coupures Wi-Fi depuis
le démarrage (`wifi_disconnects`) et la mémoire libre.

## Écran de diagnostic

Du démarrage jusqu'à la première image (ou `/off`), l'ESP32 affiche en texte sur fond noir :
raison du redémarrage, état du Wi-Fi et IP, état du serveur HTTP, dernière requête reçue et son
résultat, mémoire libre, puis les dernières lignes du journal. Il s'éteint au bout de 10 minutes
sans image (réglable dans `menuconfig`).

```sh
curl http://nowplaying.local/status
curl -X POST -H "X-Token: $TOKEN" -H "X-Track-Id: test" --data-binary @frame.jpg http://nowplaying.local/frame
curl -X POST -H "X-Token: $TOKEN" -d '{"track_id":"test","playing":true}' http://nowplaying.local/state
curl -X POST -H "X-Token: $TOKEN" http://nowplaying.local/off
```

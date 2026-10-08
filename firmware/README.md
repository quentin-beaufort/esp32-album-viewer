# Firmware de l'écran (ESP32-S3-Touch-LCD-4.3B)

Projet ESP-IDF **v5.5** ou plus récent. Le protocole est décrit dans [`../docs/protocol.md`](../docs/protocol.md).

## Configuration

```sh
cd firmware
idf.py set-target esp32s3
idf.py menuconfig   # menu "Now Playing" : SSID, mot de passe, jeton X-Token
```

Le fichier `sdkconfig` généré contient le mot de passe Wi-Fi et le jeton : il est ignoré par git.

## Compilation et flash

```sh
idf.py build
idf.py -p COM3 flash monitor   # adapter le port
```

Au démarrage, l'écran affiche un diagnostic en texte jusqu'au premier `POST /frame` : raison du
redémarrage (une « baisse de tension » désigne un problème d'alimentation ou de câble), Wi-Fi et
IP, serveur HTTP, dernière requête reçue, mémoire libre et dernières lignes du journal. Il
s'éteint après 10 minutes sans image. Ces deux réglages sont dans `menuconfig`, menu
« Now Playing ». Le moniteur série affiche les mêmes lignes de journal.

Ensuite, un appui sur l'écran tactile affiche ce diagnostic, et un nouvel appui revient à la
pochette. Les pochettes reçues pendant ce temps sont gardées et s'affichent au retour.

## Test sans téléphone

Il faut un JPEG baseline 800×480 (par exemple avec ImageMagick :
`magick in.png -resize 800x480! -interlace none frame.jpg`).

```sh
TOKEN=...   # le jeton mis dans menuconfig
curl http://nowplaying.local/status
curl -X POST -H "X-Token: $TOKEN" -H "X-Track-Id: test" --data-binary @frame.jpg http://nowplaying.local/frame
curl -X POST -H "X-Token: $TOKEN" -d '{"track_id":"test","playing":true}' http://nowplaying.local/state
curl -X POST -H "X-Token: $TOKEN" http://nowplaying.local/off
```

## Organisation

| Fichier | Rôle |
|---|---|
| `main/board.c` | broches et timings de l'écran RGB, expandeur CH422G (rétroéclairage, reset du tactile), repris de l'exemple Waveshare |
| `main/display.c` | décodage JPEG dans le framebuffer caché, bascule au VSYNC |
| `main/player.c` | morceau affiché et état de lecture (pour le 409 de `/state`) |
| `main/net.c` | Wi-Fi (sans économie d'énergie, reconnexion automatique) et mDNS |
| `main/api.c` | serveur HTTP : `/frame`, `/state`, `/off`, `/status` |
| `main/diag.c` | écran de diagnostic (démarrage et appui sur l'écran), copie des lignes du journal |
| `main/touch.c` | contrôleur tactile GT911 : un appui affiche ou masque le diagnostic |
| `main/console.c` | texte sur fond noir dans le framebuffer, en portrait |
| `main/font_mono.c` | police monospace générée par `tools/gen_font.py` (DejaVu Sans Mono), à ne pas éditer |

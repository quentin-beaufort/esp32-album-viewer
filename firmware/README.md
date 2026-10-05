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

Au démarrage l'écran reste éteint jusqu'au premier `POST /frame`. Le moniteur série affiche
l'adresse IP et `http://nowplaying.local`.

## Test sans téléphone

Il faut un JPEG baseline 800×480 (par exemple avec ImageMagick :
`magick in.png -resize 800x480! -interlace none frame.jpg`).

```sh
TOKEN=...   # le jeton mis dans menuconfig
curl http://nowplaying.local/status
curl -X POST -H "X-Token: $TOKEN" -H "X-Track-Id: test" --data-binary @frame.jpg http://nowplaying.local/frame
curl -X POST -H "X-Token: $TOKEN" -d '{"track_id":"test","playing":true,"position_ms":0,"duration_ms":60000}' http://nowplaying.local/state
curl -X POST -H "X-Token: $TOKEN" http://nowplaying.local/off
```

La barre de progression doit se remplir en une minute.

## Organisation

| Fichier | Rôle |
|---|---|
| `main/board.c` | broches et timings de l'écran RGB, expandeur CH422G (rétroéclairage), repris de l'exemple Waveshare |
| `main/display.c` | décodage JPEG dans le framebuffer caché, bascule au VSYNC, dessin de la barre |
| `main/player.c` | position interpolée et rafraîchissement de la barre |
| `main/net.c` | Wi-Fi (sans économie d'énergie, reconnexion automatique) et mDNS |
| `main/api.c` | serveur HTTP : `/frame`, `/state`, `/off`, `/status` |

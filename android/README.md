# Appli Android

Suit la lecture de Deezer et pousse la pochette, le titre et l'artiste vers l'écran ESP32
selon [`../docs/protocol.md`](../docs/protocol.md). Kotlin, minSdk 31, sans Compose.

## Compilation

```sh
cd android
./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
./gradlew lintDebug
```

Il faut un `local.properties` avec `sdk.dir=...` (SDK Android avec la plateforme 36) et un vrai
JDK 17 ou plus dans `JAVA_HOME`. Sous Windows, le lanceur `C:\Program Files\Common Files\Oracle\Java\javapath\java.exe`
perd l'argument `-classpath ""` de `gradlew.bat` (« -classpath requires class path specification ») :
définir `JAVA_HOME` sur le dossier du JDK (par exemple `C:\Program Files\Java\jdk-21`) suffit.

## Réglages

Au premier lancement :

1. **Récepteur Bluetooth** : choisir le UGREEN parmi les appareils appairés (l'appli demande
   l'accès aux appareils Bluetooth). L'appli n'est active que lorsqu'il est connecté, et envoie
   `/off` à sa déconnexion. « Aucun » la rend toujours active.
2. **Jeton** : le même que `CONFIG_NP_TOKEN` dans `menuconfig`.
3. **IP de secours** (facultative) : utilisée si `nowplaying.local` ne se résout pas.
4. **Accès aux notifications** : à accorder à Now Playing. C'est ce qui permet de lire la
   session multimédia de Deezer.
5. **Envoyer un test** : affiche l'aperçu sur l'écran et indique les codes de réponse.

## Fonctionnement

| Fichier | Rôle |
|---|---|
| `NowPlayingService.kt` | `NotificationListenerService` : suit le UGREEN (A2DP) et la session de `deezer.android.app` |
| `DeezerApi.kt` | artistes du morceau via l'API publique de Deezer, pour la ligne « feat. » |
| `FrameRenderer.kt` | rendu portrait 480×800, rotation de 90° horaire, JPEG 800×480 |
| `Sender.kt` | file sérialisée vers l'écran : une image en attente est remplacée par la plus récente |
| `EspClient.kt` | résolution de l'adresse et mini client HTTP |
| `SettingsActivity.kt` | écran de réglages |

Une nouvelle image est envoyée 300 ms après le dernier changement de métadonnées, puis de
nouveau si la pochette arrive plus tard. L'état play/pause suit avec `/state`, répété toutes
les 15 secondes ; une réponse `409` (écran redémarré, ou image précédente mal reçue) fait
renvoyer la dernière image voulue puis l'état. Chaque requête est coupée au bout de 20 s, pour
qu'un écran injoignable ne bloque pas les morceaux suivants.

### Pourquoi un client HTTP sur socket

L'écran parle HTTP en clair et son adresse vient de notre propre résolution, dans cet ordre :
résolveur du système pour `nowplaying.local`, découverte mDNS du service `_nowplaying._tcp`
(`NsdManager`), puis l'IP de secours. L'adresse trouvée est gardée jusqu'au premier échec.

`HttpURLConnection` ne permet pas de choisir l'adresse IP tout en gardant le nom d'hôte : il
faudrait appeler `http://192.168.1.112/…`, et autoriser le clair pour une IP qui change revient à
l'autoriser partout. L'appli envoie donc ses requêtes avec un petit client HTTP/1.1 sur
`java.net.Socket` (`Host: nowplaying.local`, `Connection: close`, réponse lue selon
`Content-Length`). La politique réseau d'Android ne s'applique qu'aux piles HTTP de la
plateforme : `network_security_config.xml` interdit donc le clair partout, ce qui couvre tout
autre accès réseau de l'appli (par exemple une pochette en `http://` est refusée, en `https://`
elle est téléchargée).

Aucune bibliothèque HTTP n'est utilisée : OkHttp avait deux problèmes ouverts gênants pour nous
au moment du choix (plantage sur un proxy opérateur mal formé avec 5.5.0, incompatibilité
binaire de 5.3.x avec Kotlin 2.3).

Les réglages contiennent le jeton : ils sont exclus des sauvegardes et des transferts d'appareil.

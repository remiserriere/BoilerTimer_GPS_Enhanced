# 🕒 ESP-01 Boiler Timer with GPS Time (Enhanced Version)

**Français** | [English below](#english)

---

## Description

Ce projet permet de piloter une chaudière ou un système de chauffage avec un ESP-01, en utilisant un module GPS pour obtenir l’heure courante, même sans WiFi ni accès Internet.  
Le GPS est utilisé uniquement comme source d’heure : la minuterie fonctionne de façon autonome selon les plages horaires configurées.

L’interface web (via WiFi) est utilisée uniquement pour la configuration (réseau, fuseau horaire, plages horaires, options GPS/NTP), mais elle propose également un **mode manuel** permettant un contrôle temporaire du chauffage.

- Fonctionne hors ligne grâce à la synchronisation de l’heure via GPS
- Bascule automatique sur NTP si le WiFi est disponible
- Sauvegarde de la configuration en EEPROM

---

## Fonctionnalités

- ⏱️ **Programmation horaire** (10 plages horaires configurables)
- 🛰️ **Heure GPS** : fonctionnement autonome sans WiFi/Internet
- 🌐 **Mode NTP** : l’heure peut être prise sur Internet si le WiFi est connecté
- 🖥️ **Interface Web** : configuration ET contrôle manuel ON/OFF
- 🔒 **Mode manuel** : possibilité de forcer l’allumage ou l’arrêt du chauffage via la page web ; le chauffage n’est alors plus piloté par les horaires tant que ce mode est actif. Retour au mode auto possible à tout moment.
- 💾 **Sauvegarde EEPROM** : toutes les options restent après redémarrage

---

## API JSON

Le firmware propose une API simple pour récupérer l’état du système et l’heure au format JSON, pratique pour intégration domotique ou supervision.

- Accéder à `/status` (GET) :  
  Exemple de retour :
  ```json
  {
    "relay": true,
    "manual": false,
    "currentTime": "08:21:34",
    "source": "GPS",
    "gpsValid": true,
    "ntpValid": false,
    "timeZone": 2
  }
  ```
- Accéder à `/settings` ou `/wifi` pour les paramètres et la config réseau.

---

## Schéma de raccordement

- **ESP-01**
  - GPIO0 : relais (chauffage)
  - RX/TX : GPS (SoftwareSerial ou matériel selon montage)
- **Module GPS** (NMEA ou UBX compatible)
- **Relais** pour piloter la chaudière

---

## Installation

1. **Préparer le matériel** (ESP-01, GPS, relais)
2. **Flasher le firmware** (`BoilerTimer_GPS.ino`) avec Arduino IDE
3. **Connecter au WiFi** (pour configurer via l’interface web)
4. **Configurer** : fuseau horaire, plages horaires, priorité GPS/NTP, WiFi (optionnel)
5. **Débrancher le WiFi si souhaité** : le système reste autonome grâce au GPS

---

## Utilisation

- Le chauffage s’active/désactive selon les horaires, même sans WiFi
- Le GPS fournit l’heure (format UTC, ajusté avec le fuseau et l’éventuelle heure d’été)
- Si le GPS ne fournit plus l’heure, le système tente de basculer sur NTP (si le WiFi est disponible)
- Le relais commute automatiquement selon la plage horaire courante
- **Contrôle manuel via l’interface web** :  
  - Depuis la page principale, cliquer sur "Allumer" ou "Éteindre" force l’état du chauffage (mode manuel)
  - Cliquer sur "Mode Auto" pour revenir à la programmation horaire automatique

---

## Limitations

- Le WiFi n’est pas requis en usage normal, seulement pour la configuration et le mode manuel à distance
- L’interface web ne permet PAS de contrôle distant en dehors du réseau local (sauf configuration réseau avancée)

---

## Exemples d’usage

- Résidences secondaires sans Internet : programmation fiable grâce au GPS
- Sites isolés : pas besoin de RTC ni de pile bouton, l’heure vient du GPS

---

## English

# 🕒 ESP-01 Boiler Timer with GPS Time (Enhanced Version)

## Description

This project lets you control a boiler or heating system with an ESP-01, using a GPS module to obtain current time even without WiFi or Internet.  
GPS is used only as a time source: the timer works autonomously according to configured time slots.

The web interface (via WiFi) is for configuration, but also provides a **manual mode** for direct temporary control of the boiler.

- Works offline thanks to GPS time
- Automatically falls back to NTP if WiFi is available
- Configuration stored in EEPROM

---

## Features

- ⏱️ **Time scheduling** (10 configurable time slots)
- 🛰️ **GPS time**: fully autonomous operation without WiFi/Internet
- 🌐 **NTP mode**: gets time from the Internet if WiFi is connected
- 🖥️ **Web interface**: for setup AND manual ON/OFF control
- 🔒 **Manual mode**: allows forcing the boiler ON or OFF via the web page; in this mode, schedules are ignored until you return to auto mode.
- 💾 **EEPROM save**: all options persist after reboot

---

## JSON API

The firmware exposes a simple JSON API for status and time, easy to integrate with home automation or monitoring systems.

- Access `/status` (GET):  
  Example response:
  ```json
  {
    "relay": true,
    "manual": false,
    "currentTime": "08:21:34",
    "source": "GPS",
    "gpsValid": true,
    "ntpValid": false,
    "timeZone": 2
  }
  ```
- Access `/settings` or `/wifi` for parameters/network config.

---

## Wiring

- **ESP-01**
  - GPIO0: relay (boiler control)
  - RX/TX: GPS (via SoftwareSerial or hardware)
- **GPS module** (NMEA or UBX compatible)
- **Relay** to drive the heater/boiler

---

## Installation

1. **Prepare hardware** (ESP-01, GPS, relay)
2. **Flash the firmware** (`BoilerTimer_GPS.ino`) with Arduino IDE
3. **Connect to WiFi** (for configuration via web interface)
4. **Configure**: timezone, time slots, GPS/NTP priority, WiFi (optional)
5. **Disconnect WiFi if desired**: system stays autonomous thanks to GPS

---

## Usage

- Heating switches ON/OFF according to schedule, even without WiFi
- GPS provides time (UTC, adjusted for timezone and DST)
- If GPS time is unavailable, system tries NTP (if WiFi is present)
- The relay switches automatically according to the current time slot
- **Manual control via web interface**:  
  - On the main page, click "Allumer" (Turn ON) or "Éteindre" (Turn OFF) to force boiler state (manual mode)
  - Click "Mode Auto" to return control to the scheduling system

---

## Limitations

- WiFi is NOT required in regular use, only for configuration and manual control via the local network
- The web interface does NOT allow remote (outside LAN) control unless you set up advanced network forwarding

---

## Example use-cases

- Remote houses/cabins without Internet: reliable heating schedule thanks to GPS time
- Isolated sites: no need for RTC or coin cell, time comes from GPS

---

## License

MIT

---

*Ce projet met l’accent sur la robustesse et l’autonomie grâce à la synchronisation horaire GPS.  
This project prioritizes robustness and autonomy via GPS timekeeping.*
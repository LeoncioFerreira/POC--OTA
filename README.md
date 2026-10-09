# POC OTA com ESP-IDF e GitHub Actions

POC para validar CI/CD de firmware no ESP32-WROOM-32D:

- push e pull request compilam o firmware;
- tags `v*` publicam `poc-ota.bin` em uma GitHub Release;
- o ESP32 verifica a última versão e atualiza por HTTPS;
- a tabela de partições usa dois slots OTA e rollback.

## Preparar o ESP-IDF

```bash
cd ~/.espressif/v5.5.5/esp-idf
./install.sh esp32
source export.sh
```

## Configurar Wi-Fi

As credenciais ficam somente no `sdkconfig` local, ignorado pelo Git.

```bash
cd ~/Área\ de\ Trabalho/Repositorio/POC--OTA
idf.py set-target esp32
idf.py menuconfig
```

Abra **POC OTA configuration** e configure SSID, senha e URL OTA. A URL padrão é:

```text
https://github.com/LeoncioFerreira/POC--OTA/releases/latest/download/poc-ota.bin
```

O repositório precisa estar público para esta POC sem autenticação.

## Primeira gravação por USB

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Saia do monitor com `Ctrl+]`.

## Publicar uma atualização

```bash
git add .
git commit -m "Atualiza firmware"
git push origin main
git tag v0.2.0
git push origin v0.2.0
```

O workflow cria a Release e publica `poc-ota.bin`. O ESP32 verifica a URL, instala a nova versão e reinicia.

Esta é uma POC. Antes de produção, habilite assinatura de firmware, Secure Boot, Flash Encryption, autenticação por dispositivo e liberação gradual.

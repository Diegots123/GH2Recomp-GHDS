# GH2 Recomp

Recompilação nativa para PC do **Guitar Hero II (PS2, USA, SLUS-21447, v1.00)**, feita a partir do **seu próprio disco**.
O executável do jogo é traduzido para C++ e compilado para Windows (ou Linux). O que roda é o motor do próprio GH2, sem mods: mesma jogabilidade e mesmo timing do PS2, com loads mais rápidos, resolução maior e guitarras atuais.
Feito do mesmo jeito que o [GH1 Recomp](https://github.com/Diegots123/GH1Recomp-GHDS), com os mesmos ajustes.

*English below.*

> **Este repositório não contém nada do jogo.** Não há executável, áudio, vídeo, texturas nem código recompilado aqui, e as Releases não trazem o exe.
> Para jogar, cada pessoa compila o jogo a partir da imagem do próprio disco (Redump, Guitar Hero II (USA) v1.00).
> O exe gerado contém código traduzido do seu disco: **é só para você, não o distribua.**
>
> Projeto não oficial, sem ligação com a Harmonix, a RedOctane ou a Activision. Guitar Hero é marca da Activision.

## O que funciona

- Boot, vídeos (com o som da abertura), menus, Quick Play com músicas inteiras e saves no cartão de memória (`memcards\`).
- Vídeo pelo GS do PCSX2 em OpenGL ou Direct3D 11 (`renderer` no `GH2Recomp.ini`; resolução interna 3× por padrão, tela cheia com F11 ou Alt+Enter); VU1 pelo microVU do PCSX2, numa thread própria a partir de 2 threads de CPU.
- Com `vsync = 1`, o ritmo do jogo casa com monitores de 60, 120, 180 e 240 Hz: cada quadro dura o mesmo número de atualizações da tela.
- Os reflexos de luz do palco, lidos do Z buffer um ou dois quadros depois (imperceptível), sem o jogo esperar a placa de vídeo.
- Extras: `audio_offset`, a calibração de áudio que o jogo não tem no menu (o som das músicas mais cedo ou mais tarde em relação às notas, sem mexer no disco), e `venue = off` para placas fracas (só a pista, sem o palco, a banda e a plateia).
- Controles: guitarras atuais (Xbox 360, PS3, Wii por raphnet, Rock Band 4, Riffmaster, World Tour de PC), guitarra e DualShock 2 de PS2 por adaptador USB (inclusive o Twin USB, que manda a guitarra no modo digital: tudo funciona menos o whammy; `gamepad = guitar|dualshock` no ini decide se o reconhecimento automático errar), e teclado (configurado com F1 no menu principal; no modo palhetando, as teclas dos trastes valem nos menus, para os cheats do menu principal). Como num PS2 com um controle só, a porta 2 fica vazia.
- Só abre com a imagem idêntica à do Redump: nada modificado.

Controles e configurações em detalhe: [`dist/windows-gh2/LEIA-ME.txt`](dist/windows-gh2/LEIA-ME.txt). Notas de desenvolvimento: [`GH2_PROGRESS.md`](GH2_PROGRESS.md).

## Como compilar (exe de Windows)

O exe é compilado no Linux (no Windows, pelo WSL) com o clang-cl, contra a biblioteca C e o SDK do Windows que o [xwin](https://github.com/Jake-Shadle/xwin) baixa da Microsoft. A primeira compilação leva de 20 a 40 minutos, conforme o PC (mais, se a internet for lenta), e usa uns 5 GB de disco.

**1. Ubuntu 24.04.** No Windows, abra o PowerShell como administrador e rode `wsl --install -d Ubuntu-24.04`, reinicie e crie seu usuário no Ubuntu. Depois, no terminal do Ubuntu:

```sh
sudo apt update
sudo apt install -y git cmake ninja-build python3 build-essential pkg-config curl \
    clang-20 clang-tools-20 lld-20 llvm-20
```

**2. O SDK do Windows, pelo xwin.** Rode na sua pasta do Ubuntu (o `cd ~`; dentro de `/mnt/c` o xwin falha). O `--accept-license` aceita a licença da Microsoft para esses arquivos:

```sh
cd ~
curl -LO https://github.com/Jake-Shadle/xwin/releases/download/0.6.5/xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz
tar xf xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz
./xwin-0.6.5-x86_64-unknown-linux-musl/xwin --accept-license --arch x86_64 splat --use-winsysroot-style --output ~/xwin-sdk
```

(Quem já compilou o GH1 Recomp já tem o `~/xwin-sdk` e pode pular este passo.)

**3. Este repositório e a sua ISO.** O disco do Windows aparece no Ubuntu em `/mnt/c` (por exemplo, `C:\Jogos\Guitar Hero II (USA).iso` é `/mnt/c/Jogos/Guitar Hero II (USA).iso`):

```sh
cd ~
git clone https://github.com/Diegots123/GH2Recomp-GHDS.git
cd GH2Recomp-GHDS
python3 tools/gh2.py verify "/mnt/c/Jogos/Guitar Hero II (USA).iso"      # confere com o Redump
python3 tools/gh2.py build --windows ~/xwin-sdk "/mnt/c/Jogos/Guitar Hero II (USA).iso"
```

O `build` lê só o executável do disco (`SLUS_214.47`), confere o SHA-1, recompila, baixa o PCSX2 e o SDL3 nas versões fixadas e compila. Cada etapa grava um log em `build/`. Se a compilação parar por falta de memória, limite os processos em paralelo e rode o mesmo comando de novo: `CMAKE_BUILD_PARALLEL_LEVEL=2 python3 tools/gh2.py build ...`.

**4. Jogar.** O resultado é `build/GH2Recomp-win64.zip`. Copie para o Windows (`cp build/GH2Recomp-win64.zip /mnt/c/Jogos/`), extraia, coloque a ISO na pasta `iso` e abra o `GH2Recomp.exe`. É preciso Windows 10 ou 11 de 64 bits, placa de vídeo com OpenGL 4.3 ou Direct3D 11 (vídeo integrado serve) e processador de 2 threads ou mais.

O exe não tem assinatura digital, então o Windows (SmartScreen) ou o antivírus podem estranhá-lo na primeira vez, principalmente se o zip passar pela internet. Ele é compilado na sua máquina, a partir deste código.

### Linux

Além dos pacotes do passo 1, instale as bibliotecas de janela, som e vídeo e compile sem o `--windows`:

```sh
sudo apt install -y libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libgl-dev libegl-dev libasound2-dev libpulse-dev libdbus-1-dev libudev-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev
python3 tools/gh2.py build "Guitar Hero II (USA).iso"
build/runtime/ps2xRuntime/ps2EntryRunner "Guitar Hero II (USA).iso"
```

## Como funciona

- O [PS2Recomp](https://github.com/ran-j/PS2Recomp) traduz cada função do executável do GH2 (MIPS R5900) para C++, e o runtime dele faz o papel do kernel e do IOP do PS2.
- Os nomes das funções (`config/gh2-retail.symbols`, só endereços e nomes) são a lista pública do GH2 do GH Classic.
- Só funções das bibliotecas da Sony e da libc com identidade provada são trocadas por código do runtime (`tools/gh1_stub_gate.py`, nunca as de `config/gh2-stub-denylist.txt`); todo o código de jogo é o do GH2.
- `pcsx2gs/` compila sozinho o GS e o microVU do PCSX2 (sem modificar o código deles) e liga ao jogo uma janela SDL3, com os controles e o som.
- `gh2/` é a camada do GH2: o servidor de som (o módulo de synth do GH Classic), o áudio (com o dos vídeos), os controles, o launcher, os extras e os traces de diagnóstico.

## Estrutura

| Pasta | Conteúdo |
|---|---|
| `gh2/` | camada do GH2 ligada ao runtime |
| `pcsx2gs/` | GS e microVU do PCSX2 fora do emulador; janela, controles e teclado (SDL3) |
| `src/` | módulo de synth do GH Classic e cabeçalhos que ele usa |
| `lib/PS2Recomp/` | recompilador e runtime (fork com as mudanças do GH1 e do GH2) |
| `tools/` | `gh2.py` (verificar, compilar), símbolos, empacotamento, toolchain do Windows |
| `config/` | nomes das funções do GH2 e a lista de funções que nunca viram stub |
| `dist/` | o que vai no zip: `GH2Recomp.ini` e `LEIA-ME.txt` (`windows-gh2/`), `gamecontrollerdb.txt` (`windows/`) |

## Créditos e licenças

- **[GH Classic](https://github.com/noahbaxter/GHClassic)** (noahbaxter), GPL-3.0: a base deste projeto. O módulo de synth, as ferramentas de símbolos, a lista de nomes do GH2 e o fork do PS2Recomp vêm de lá.
- **[PS2Recomp](https://github.com/ran-j/PS2Recomp)** (ran-j e colaboradores), GPL-3.0: recompilador e runtime. `lib/PS2Recomp` é o fork do GH Classic com as mudanças do GH1 e do GH2; os módulos de PS Vita (`.suprx`) ficaram de fora.
- **[PCSX2](https://github.com/PCSX2/pcsx2)**, GPL-3.0-or-later: GS e microVU, baixados na compilação (commit fixado em `tools/gh2.py`) e usados sem modificação.
- **[SDL3](https://github.com/libsdl-org/SDL)** (zlib), **[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)** (zlib), **[raylib](https://github.com/raysan5/raylib)** (zlib), **[FFmpeg](https://ffmpeg.org)** (LGPL-2.1-or-later), e as bibliotecas que o PCSX2 e o PS2Recomp trazem.

Este projeto é distribuído sob a **GPL-3.0** ([`LICENSE`](LICENSE)). O zip gerado inclui todas as licenças em `LICENCAS.txt`.

---

## English

A native PC recompilation of **Guitar Hero II (PS2, USA, SLUS-21447, v1.00)**, built from **your own disc**, the same way as [GH1 Recomp](https://github.com/Diegots123/GH1Recomp-GHDS). The game's executable is translated to C++ and compiled for Windows (or Linux). It runs GH2's own engine with no mods: the PS2's gameplay and timing, with faster loads, higher resolution and modern guitars. Two opt-in extras: `audio_offset`, the audio calibration the game's menu lacks (the songs' audio earlier or later against the notes, with the disc untouched), and `venue = off` for weak GPUs. Controls: modern guitars, PS2 guitars and DualShock 2s on USB adapters (Twin USB included, without the whammy it cannot pass), and the keyboard, whose fret keys also work in the menus for the cheats. OpenGL or Direct3D 11; the VU1 runs on its own thread.

> **This repository contains nothing from the game**: no executable, audio, video, textures or recompiled code, and the Releases carry no exe. Everyone builds the game from their own disc image (Redump, Guitar Hero II (USA) v1.00). The resulting exe holds code translated from your disc: **it is for you only; do not distribute it.** Unofficial; not affiliated with Harmonix, RedOctane or Activision.

**Building the Windows exe** (Ubuntu 24.04, or WSL on Windows; 20 to 40 minutes the first time). Run it from your Linux home folder (`cd ~`): xwin fails inside `/mnt/c`.

```sh
sudo apt install -y git cmake ninja-build python3 build-essential pkg-config curl clang-20 clang-tools-20 lld-20 llvm-20
cd ~
curl -LO https://github.com/Jake-Shadle/xwin/releases/download/0.6.5/xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz
tar xf xwin-0.6.5-x86_64-unknown-linux-musl.tar.gz
./xwin-0.6.5-x86_64-unknown-linux-musl/xwin --accept-license --arch x86_64 splat --use-winsysroot-style --output ~/xwin-sdk
git clone https://github.com/Diegots123/GH2Recomp-GHDS.git && cd GH2Recomp-GHDS
python3 tools/gh2.py verify "Guitar Hero II (USA).iso"
python3 tools/gh2.py build --windows ~/xwin-sdk "Guitar Hero II (USA).iso"
```

This makes `build/GH2Recomp-win64.zip`: extract it, put the ISO in its `iso` folder and run `GH2Recomp.exe` (Windows 10/11 64-bit, OpenGL 4.3 or Direct3D 11, a CPU with 2 threads or more). The exe is unsigned, so SmartScreen or an antivirus may question it the first time. For a Linux build, install the X11/OpenGL/ALSA/PulseAudio/D-Bus/udev/FFmpeg development packages listed above and run `tools/gh2.py build` without `--windows`.

**Credits:** [GH Classic](https://github.com/noahbaxter/GHClassic) (noahbaxter), which this project is built on; [PS2Recomp](https://github.com/ran-j/PS2Recomp); [PCSX2](https://github.com/PCSX2/pcsx2) (GS and microVU, unmodified); SDL3, SDL_GameControllerDB, raylib, FFmpeg. Licensed under the GPL-3.0.

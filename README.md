# RP-ROM

M27C256 EPROM エミュレータ。RP2354A (RP2350 QFN-60) を搭載した自作 PCB で、32KB の ROM コンテンツを USB 経由で書き込み・読み出しできる。

## 概要

| 項目 | 内容 |
| --- | --- |
| 対象 EPROM | M27C256 互換 (32KB, 15-bit address, 8-bit data) |
| MCU | RP2354A (RP2350 QFN-60, 360 MHz 動作) |
| インターフェース | USB-C (CDC Serial) |
| フラッシュ保持 | 電源断後も ROM データを内蔵フラッシュに保存 |

## リポジトリ構成

```text
RP-ROM/
├── hardware/          # KiCad プロジェクト (回路図・PCB)
│   └── production/    # 製造用ファイル (BOM, Gerber, 部品配置)
├── software/          # RP2350 ファームウェア (C / Pico SDK)
│   └── main.c
└── tools/             # ホスト側 Python ツール
    └── rom_tool.py
```

## ハードウェア

KiCad で設計。主要部品:

| Ref | 部品 | 備考 |
| --- | --- | --- |
| IC1 | RP2354A | メイン MCU (QFN-60) |
| U1 | RT9080-33GJ5 | 3.3V LDO |
| U2 | CH213K | USB ESD 保護 |
| J4 | USB-C | USB 2.0 (CDC) |
| J1, J2 | 1×14 ピンヘッダ | ROM ソケット接続 |
| J3 | JST SH 3P | 外部電源 (オプション) |
| Y1 | 12 MHz 水晶 | システムクロック基準 |

製造ファイルは `hardware/production/` に格納。

## ファームウェア

### 必要環境

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) v2.2.0
- CMake 3.13+
- ARM GCC ツールチェーン 14.2

### ビルド

```bash
cd software
mkdir build && cd build
cmake ..
make -j
```

生成物: `build/RP-ROM.uf2`

### 書き込み

BOOTSEL ボタンを押しながら USB 接続 → USB マスストレージとして認識 → `RP-ROM.uf2` をコピー。

### 動作

- **Core 1**: ROM アドレス線監視・データ出力 (フラッシュ不使用のタイトループ、360 MHz)
- **Core 0**: USB シリアル経由のコマンド処理
- **永続化**: 書き込んだ ROM データを内蔵フラッシュ末尾 36KB に保存。電源投入時に自動ロード。

## ツール (tools/)

Python 製のホストツール。詳細は [tools/README.md](tools/README.md) を参照。

```bash
cd tools
uv run rom_tool.py write COM3 firmware.hex
uv run rom_tool.py read  COM3 dump.bin
uv run rom_tool.py verify COM3 firmware.hex
uv run rom_tool.py erase COM3
uv run rom_tool.py info  COM3
```

対応フォーマット: `.bin` / Intel HEX (`.hex`)

## ライセンス

[MIT License](LICENSE)

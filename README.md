# RP-ROM

M27C256 EPROM エミュレータ。RP2354A (RP2350 QFN-60) を搭載した自作 PCB で、32KB の ROM コンテンツを USB 経由で書き込み・読み出しできる。

## 概要

| 項目             | 内容                                            |
| ---------------- | ----------------------------------------------- |
| 対象 EPROM       | M27C256 互換 (32KB, 15-bit address, 8-bit data) |
| MCU              | RP2354A (RP2350 QFN-60, 360 MHz 動作)           |
| インターフェース | USB-C (CDC Serial)                              |
| フラッシュ保持   | 電源断後も ROM データを内蔵フラッシュに保存     |

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

KiCad で設計。製造 BOM の概要:

| Ref                 | 数量 | 部品 / 値                   | パッケージ / 備考              | LCSC Part # |
| ------------------- | ---: | --------------------------- | ------------------------------ | ----------- |
| IC1                 |    1 | RP2354A                     | QFN-60, メイン MCU             | C41378174   |
| U1                  |    1 | RT9080-33GJ5                | TSOT-23-5, 3.3V LDO            | C7528830    |
| J1                  |    1 | USB_C_Receptacle_USB2.0_16P | USB-C                          | C2843970    |
| J2, J3              |    2 | Conn_01x14_Pin              | 1×14 ピンヘッダ                |             |
| J4                  |    1 | Conn_01x03                  | JST SH 3P, デバッガー          | C7430452    |
| SW1                 |    1 | SW_Push                     | TS-1088 系タクトスイッチ       | C720477     |
| Y1                  |    1 | 12 MHz                      | 3225 4-pin 水晶                | C9002       |
| L1                  |    1 | 3.3uH                       | AOTA-B201610S3R3-101-T         | C42411119   |
| D1, D2              |    2 | RB521S-30                   | SOD-523 ショットキーダイオード | C8523       |
| D3                  |    1 | LED                         | 0402                           | C51933305   |
| C1                  |    1 | 4.7nF                       | 0402                           | C1538       |
| C2, C3              |    2 | 1uF                         | 0402                           | C52923      |
| C5, C8              |    2 | 15pF                        | 0402                           | C1548       |
| C9, C10, C11        |    3 | 4.7uF                       | 0402                           | C23733      |
| C4, C6, C7, C12-C14 |    6 | 0.1uF                       | 0402                           | C1525       |
| R1                  |    1 | 1MΩ                         | 0402                           | C26083      |
| R2, R3              |    2 | 5.1kΩ                       | 0402                           | C25905      |
| R4, R5, R6          |    3 | 1k                          | 0402                           | C11702      |
| R7, R8              |    2 | 27Ω                         | 0402                           | C25100      |
| R9                  |    1 | 33Ω                         | 0402                           | C25105      |

製造ファイルは `hardware/production/` に格納。

## ファームウェア

### 必要環境

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) v2.2.0

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

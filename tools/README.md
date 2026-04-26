# RP-ROM Tools

RP-ROM EPROM エミュレータ用ホストツール。USB シリアル (CDC) 経由で ROM データの書き込み・読み出し・検証を行う。

## セットアップ

[uv](https://github.com/astral-sh/uv) を使用:

```bash
cd tools
uv sync
```

または pip:

```bash
pip install pyserial
```

## 使い方

```text
python rom_tool.py <command> <COMx> [file]
```

### コマンド一覧

| コマンド | 説明 |
| --- | --- |
| `write <port> <file>` | ROM イメージをデバイスに書き込む |
| `read <port> <file>` | デバイスから ROM イメージを読み出す |
| `verify <port> <file>` | デバイス内容をファイルと照合する |
| `erase <port>` | ROM を消去する (全バイト 0xFF) |
| `info <port>` | デバイス情報を表示する |

### 例

```bash
# .hex または .bin の書き込み
uv run rom_tool.py write COM3 firmware.hex
uv run rom_tool.py write COM3 firmware.bin

# 読み出し
uv run rom_tool.py read COM3 dump.bin
uv run rom_tool.py read COM3 dump.hex

# 書き込み後の検証
uv run rom_tool.py verify COM3 firmware.hex

# 消去
uv run rom_tool.py erase COM3

# デバイス情報
uv run rom_tool.py info COM3
```

## 対応ファイル形式

| 拡張子 | 形式 |
| --- | --- |
| `.hex` | Intel HEX |
| `.bin` / `.rom` / `.epr` | バイナリ |
| その他 | 先頭バイトで自動判別 |

ROM サイズ (32KB) に満たないバイナリは 0xFF でパディングされる。

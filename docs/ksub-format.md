# The `.ksub` file: Kode's sub-GHz capture and code format

`.ksub` is Kode's own text format for a sub-GHz signal an app keeps on the SD card: either the
runs of a capture (RAW) or one code of a fixed-code remote (CODE). The F\*CK SDK writes and reads
it in `fck_drv_subghz_file` (`fck_subghz_to_file()`, `fck_subghz_code_to_file()`,
`fck_subghz_from_file()`); nothing in it follows another program's file, and no other
program's file is read. Version 1 of the format is what this page describes.

## Shape of a file

A file is text, one line after another, ending in `\n` or `\r\n`.

- The first line that is not blank and does not start with `#` must be `kode-subghz 1`.
- Every other such line is `key: value`. Blanks around the key and the value are not part of them.
- Case: the header, the keys and the words `raw`, `code` and `ook` are read in any case (`Kind: CODE`
  is `kind: code`), and so are the hex digits of a code. The writer always writes lowercase. A protocol
  name is the one thing read as written: lowercase only.
- An app that reads only the first lines of a long recording, to list it without reading every pulse,
  judges those lines by these same rules. It cannot see what comes after
  them: it needs `kind`, `frequency`, `modulation` and `pulses` above the first `raw` line, where the
  writer always puts them, and the file is judged whole only when it is opened.
- Blank lines and lines that start with `#` can be anywhere.
- A key the reader does not know is skipped, so that a later version of the format may add keys
  without breaking this reader.
- The order of the keys is free, except that `pulses` comes before the first `raw` line.

## Keys every file has

| Key | Value |
|---|---|
| `kind` | `raw` or `code` |
| `frequency` | Where it was heard or is to be sent, in hertz: a decimal number from 1 to 4294967295 |
| `modulation` | `ook` (on-off keying; the only one version 1 has) |

## A RAW file (`kind: raw`)

```
kode-subghz 1
kind: raw
frequency: 433920000
modulation: ook
pulses: 6
raw: 436 -1308 1308 -436 436 -9000
```

| Key | Value |
|---|---|
| `pulses` | How many runs the file holds in all: 1 or more |
| `raw` | Runs, separated by blanks: signed decimal microseconds, positive for the carrier on, negative for off, never 0, from -2147483648 to 2147483647 (a 32-bit signed number). As many `raw` lines as it takes, 512 runs to a line when written |

The runs are the ones `fck_subghz_capture()` gives, in order. The number of runs on all the `raw`
lines must be exactly `pulses`.

## A CODE file (`kind: code`)

```
kode-subghz 1
kind: code
frequency: 433920000
modulation: ook
protocol: pt2262
bits: 24
code: 5155c3
pulse: 436
```

| Key | Value |
|---|---|
| `protocol` | The protocol's name: 1 to 31 characters, each a lowercase letter, a digit, `_` or `-`. The SDK's own: `ev1527`, `pt2262`, `ht12e` |
| `bits` | How many bits the code has: 1 to 32 |
| `code` | The code, in hexadecimal, up to 8 digits, no prefix, the highest bit first; it must fit in `bits` |
| `pulse` | The base pulse (te) in microseconds: 1 to 65535. The writer always writes it; the SDK's reader of a code file needs it and checks it against the protocol's band |

The file reader (`fck_drv_subghz_file`) keeps any protocol name, whatever it says; it is the SDK's
layer above (`fck_subghz_from_file()`) that accepts only the protocols the SDK reads and writes,
with the bit counts and pulse band each has (`fck_subghz_protocol_info()`). A file of another name
is refused there, not here.

## What the reader refuses

The reader reads exactly the bytes it is given, never past them, and takes nothing it cannot hold.
A file is refused, and nothing is written to the reader's output, when:

- the first line is not `kode-subghz 1` (another version, or no header), or the text is empty;
- a NUL byte is anywhere in it;
- a line has no colon;
- a known key is given twice (`raw` lines excepted);
- `kind`, `frequency` or `modulation` is missing, `kind` is neither `raw` nor `code`,
  `frequency` is 0 or not a number that fits, or `modulation` is not `ook`;
- a key of the other kind is present (`protocol`, `bits`, `code` or `pulse` in a RAW file;
  `pulses` or `raw` in a CODE file);
- a RAW file has no `pulses`, no runs, a `raw` line before `pulses`, a run that is 0 or not a
  number, or a count of runs that is not `pulses`;
- a CODE file has no `protocol`, `bits` or `code`, a protocol name outside the characters above,
  `bits` of 0 or over 32, a `code` of more than 8 digits, with a character that is not a
  hexadecimal digit, or with a bit above `bits`, or a `pulse` of 0 or over 65535;
- any number does not parse or does not fit its type (no sign, no blank, no `0x`, except a run's
  minus).

A RAW file with more runs than the caller has room for is not refused: the first runs that fit are
kept and the result says it was cut.

## Writing

The writer always puts the keys in the order of the examples above, one run list in lines of 512
runs, and ends the text with a `\n` and a NUL (not part of the file). `FCK_SUBGHZ_FILE_RAW_MAX(n)`
bytes always hold a RAW file of `n` runs, and `FCK_SUBGHZ_FILE_CODE_MAX` bytes a CODE file.

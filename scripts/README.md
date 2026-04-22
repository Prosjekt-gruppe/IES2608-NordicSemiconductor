# Scripts

## Field log decoder

Decode `fieldlog dump` captures into a readable table:

```sh
python scripts/fieldlog_decode.py fieldlog_dump.txt
```

Write expanded CSV:

```sh
python scripts/fieldlog_decode.py fieldlog_dump.txt --format csv --output fieldlog_decoded.csv
```

Read from stdin:

```sh
type fieldlog_dump.txt | python scripts/fieldlog_decode.py -
```


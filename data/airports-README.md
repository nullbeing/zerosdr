# Airport table

`airports.csv` is a filtered extract of the OurAirports airport data:

- Source: https://davidmegginson.github.io/ourairports-data/airports.csv
- Project: https://ourairports.com/data/
- License: public domain (the upstream data is released into the public
  domain; the repository carries an Unlicense-style dedication)

The extract keeps airports with a four-letter ICAO identifier that either
have scheduled service or are a large/medium airport, and reduces each row to
`ident,name,city,country,iata`. Names are converted to ASCII and stripped of
common suffixes, and the municipality is reduced to its first component, so
that the 320x170 UI can print a short label such as `TAIYUAN` or `GUANGZHOU`.

Regenerate after a new upstream dump:

```bash
tools/build_airports_csv.py /path/to/airports.csv
```

The file is installed to `/usr/share/zerosdr/airports.csv`. When it is
missing, ACARS falls back to showing the raw ICAO codes.

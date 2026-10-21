#!/usr/bin/env python3
"""Export one xctrace table as rows of {mnemonic: (fmt, value)}, resolving refs.

usage: xctrace_table.py TRACE SCHEMA [--limit N]   (prints JSON lines)
"""
import json
import subprocess
import sys
import xml.etree.ElementTree as ET


def rows(trace, schema):
    xml = subprocess.run(
        ['xcrun', 'xctrace', 'export', '--input', trace, '--xpath',
         f'/trace-toc/run[@number="1"]/data/table[@schema="{schema}"]'],
        capture_output=True, check=True).stdout
    root = ET.fromstring(xml)
    node = root.find('node')
    if node is None:
        return
    cols = [c.findtext('mnemonic') for c in node.find('schema').findall('col')]
    ids = {}

    def resolve(el):
        if 'ref' in el.attrib:
            return ids[el.attrib['ref']]
        if 'id' in el.attrib:
            ids[el.attrib['id']] = el
        for child in el:
            resolve(child)
        return el

    for row in node.findall('row'):
        out = {}
        for col, el in zip(cols, list(row)):
            el = resolve(el)
            if el.tag == 'sentinel':
                continue
            out[col] = (el.attrib.get('fmt', ''), el.text)
        yield out


if __name__ == '__main__':
    limit = int(sys.argv[sys.argv.index('--limit') + 1]) if '--limit' in sys.argv else None
    for i, r in enumerate(rows(sys.argv[1], sys.argv[2])):
        if limit is not None and i >= limit:
            break
        print(json.dumps(r))

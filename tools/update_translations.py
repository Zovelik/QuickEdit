"""Brings the translation catalogs (lang/*.json) up to date with the code.

Lists the texts of the interface: the literals given to tr(), the labels of
the undo history (commit(), edit = ..., result.label = ...), and the tables
translated where they are shown (tools, presets, effects, transitions).
Each catalog gets the new texts with an empty translation (shown in
English until translated) and loses the ones the code no longer has.

    python tools/update_translations.py           # update every catalog
    python tools/update_translations.py --check   # list, change nothing
"""

import codecs
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STR = r'"(?:[^"\\\n]|\\.)*"'
PIECE = r'(?:ICON_MD_\w+|' + STR + r')'
SEQ = PIECE + r'(?:\s*' + PIECE + r')*'

# Calls and assignments holding texts of the interface.
PATTERNS = [
    r'\btr\(\s*(' + SEQ + r')\s*\)',
    r'\bcommit\(\s*(' + SEQ + r')\s*\)',
    r'\bcommit\([^;]*?\?\s*(' + STR + r')\s*:\s*(' + STR + r')',
    r'\bedit\s*=\s*(' + STR + r');',
    r'\bedit\s*=\s*[^;?]*\?\s*(' + STR + r')\s*:\s*(' + STR + r');',
    r'finishedEdit\(edit,\s*(' + STR + r')\)',
    r'finishedEdit\(edit,[^;]*?\?\s*(' + STR + r')\s*:\s*(' + STR + r')\)',
    r'result\.(?:label|edit)\s*=\s*(' + STR + r');',
    r'result\.(?:label|edit)\s*=\s*[^;?]*\?\s*(' + STR + r')\s*:\s*(' + STR + r');',
    r'\blabel\s*=\s*(' + STR + r');',
    r'\blabel\s*=\s*[^;?]*\?\s*(' + STR + r')\s*:\s*(' + STR + r');',
    r'propertyLabel\(\s*(' + STR + r')\)',
    r'animatedRow\(\s*(' + STR + r')',
    r'cropRow\(\s*(' + STR + r')',
    r'combo\(\s*(' + STR + r')',
    r'properties\.push_back\(\{\s*(' + STR + r'),\s*(' + STR + r')',
    r'case Easing::\w+:\s*return\s*(' + STR + r')',
    r'(?:toolbarButton|panelButton)\(\s*ICON_MD_\w+(?:\s*' + STR + r')?\s*,\s*(' + STR + r')',
    r'panelButton\(\s*ICON_MD_\w+\s*(' + STR + r')',
    r'optionButton\(\s*(' + STR + r')',
]


def c_string(literals):
    raw = ''.join(m[1:-1] for m in re.findall(STR, literals))
    return codecs.escape_decode(raw.encode('utf-8'))[0].decode('utf-8', errors='replace')


def key_of(text):
    """The part tr() looks up: without the ImGui ID, nor spaces at the ends."""
    return text.split('##')[0].strip()


def is_text(key):
    return re.search(r'[A-Za-z]{2}', re.sub(r'%[-+ #0-9.]*[a-z]+', '', key)) is not None


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def literals_between(text, start, end):
    i = text.index(start)
    return [m.group(0) for m in re.finditer(STR, text[i:text.index(end, i)])]


def texts():
    keys = set()

    # The sources of the interface, and its headers (inline widgets).
    for path in glob.glob(os.path.join(ROOT, 'src', 'app', '*.cpp')) + glob.glob(os.path.join(ROOT, 'src', 'app', '*.h')):
        source = read(path)

        for pattern in PATTERNS:
            for match in re.finditer(pattern, source):
                for group in match.groups():
                    if group:
                        keys.add(key_of(c_string(group)))

    gui = read(os.path.join(ROOT, 'src', 'app', 'GUI.cpp'))
    keys.update(key_of(c_string(l)) for l in literals_between(gui, 'timelineTools()\n{', 'return tools;'))
    keys.update(key_of(c_string(l)) for l in literals_between(gui, 'exportPresets()\n{', 'return presets;'))

    inspector = read(os.path.join(ROOT, 'src', 'app', 'Inspector.cpp'))
    for table in ('ResolutionPresets[]', 'LetterAnimationNames[]', 'FrameRatePresets[]', 'ScaleFilterNames[]', 'ScaleFilterTooltips[]'):
        keys.update(key_of(c_string(l)) for l in literals_between(inspector, table, ';'))

    layers = read(os.path.join(ROOT, 'src', 'core', 'Layers.cpp'))
    i = layers.index('const char* transitionName')
    keys.update(c_string(m.group(1)) for m in re.finditer(r'return\s*(' + STR + r');', layers[i:layers.index('\n}', i)]))

    effects = read(os.path.join(ROOT, 'src', 'core', 'VideoEffects.cpp'))
    keys.update(c_string(m.group(1)) for m in re.finditer(r'\{\s*VideoEffectType::\w+,\s*' + STR + r',\s*(' + STR + r')', effects))
    keys.update(c_string(m.group(1)) for m in re.finditer(r'\{\s*(' + STR + r'),\s*-?[\d.]+f', effects))

    project = read(os.path.join(ROOT, 'src', 'core', 'Project.cpp'))
    for function in ('const char* fitModeName', 'const char* blendModeName'):
        i = project.index(function)
        keys.update(c_string(m.group(1)) for m in re.finditer(r'return\s*(' + STR + r');', project[i:project.index('\n}', i)]))

    shaders = read(os.path.join(ROOT, 'src', 'core', 'Shaders.cpp'))
    i = shaders.index('const char* shaderKindName')
    keys.update(c_string(m.group(1)) for m in re.finditer(r'return\s*(' + STR + r');', shaders[i:shaders.index('\n}', i)]))

    return sorted(k for k in keys if k and is_text(k))


def main():
    check = '--check' in sys.argv
    keys = texts()
    print(f'{len(keys)} texts in the code')

    for path in sorted(glob.glob(os.path.join(ROOT, 'lang', '*.json'))):
        catalog = json.loads(read(path))
        missing = [k for k in keys if not catalog.get(k)]
        obsolete = [k for k in catalog if not k.startswith('_') and k not in keys]
        print(f'{os.path.basename(path)}: {len(missing)} to translate, {len(obsolete)} obsolete')

        for k in missing:
            print(f'  + {k!r}')
        for k in obsolete:
            print(f'  - {k!r}')

        if check:
            continue

        updated = {k: v for k, v in catalog.items() if k.startswith('_')}
        for k in keys:
            updated[k] = catalog.get(k, '')

        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(updated, f, ensure_ascii=False, indent=2)
            f.write('\n')


if __name__ == '__main__':
    main()

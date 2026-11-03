import re
from pathlib import Path

TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)

def function_span(text, name):
        masked = TOKEN.sub(lambda match: ' ' * len(match[0]), text)
        match = re.search(r'(?m)^[\w \t*]+\b'+re.escape(name)+r'\([^;]*?\)\s*\{', masked)
        if not match:
                raise ValueError(name)
        start = masked.index('{', match.start())
        depth = 1
        end = start + 1
        while depth:
                depth += (masked[end] == '{') - (masked[end] == '}')
                end += 1
        return match.start(), end

def replace_function(file, name, replacement):
        path = Path(file)
        text = path.read_text()
        a, b = function_span(text, name)
        path.write_text(text[:a]+replacement.strip()+'\n'+text[b:])

from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[4]
source = (root / 'OptiScaler/hooks/LibraryLoad_Hooks.cpp').read_text(encoding='utf-8-sig')

def body(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]', source[opening:]):
        if token.group() == '{': depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth: return source[start:opening+token.end()]
    raise RuntimeError(signature)

output = Path(sys.argv[1])
(output / 'scheduler.inc').write_text(
    'namespace {\n' + body('void RunObservationWorker() {') + '\n}\n' +
    body('void LibraryLoadHooks::SchedulePending() noexcept'), encoding='utf-8')
(output / 'roles.inc').write_text(
    'enum class OtaPlugin { None, Dlss, Dlssg, Reflex, Pcl, Common };\n' +
    body('OtaPlugin OtaRole('), encoding='utf-8')
